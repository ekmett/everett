/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Keeps ordered nursery edits behind fully persistent snapshots.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

namespace everett {
  // One owner edits a transient map. Frozen snapshots share immutable bindings
  // and can be read or independently thawed while that owner keeps editing.
  // Keys are copy-constructible, values are move-constructible, and neither
  // type needs assignment. A traversal callback must not edit that transient.
  // Comparators must be stable strict weak orders; values must not mutate
  // externally shared pointees behind the map's const access.
  //
  // freeze() closes one shared edit token, without walking the tree. The next
  // edit copies its AVL path; subsequent private edits reuse owned nodes. A
  // snapshot copy is O(1), lookups/edits take O(log n) comparisons, and edits
  // allocate at most O(log n) nodes. Final-owner reclamation is separate work.
  template <class Key, class Value, class Compare = std::less<Key>> struct nursery_map {
  private:
    struct edit_token { bool open = true; };
    struct binding {
      Key key;
      Value value;
      template <class K, class V> binding(K && next_key, V && next_value)
        : key(std::forward<K>(next_key)), value(std::forward<V>(next_value)) {}
    };
    using binding_pointer = std::shared_ptr<binding const>;
    struct node;
    using node_pointer = std::shared_ptr<node>;
    struct node {
      binding_pointer entry;
      node_pointer left, right;
      std::shared_ptr<edit_token> edit;
      unsigned height = 1;
      node(binding_pointer value, std::shared_ptr<edit_token> owner)
        : entry(std::move(value)), edit(std::move(owner)) {}
      node(node const & old, std::shared_ptr<edit_token> owner)
        : entry(old.entry), left(old.left), right(old.right), edit(std::move(owner)), height(old.height) {}
    };

  public:
    using key_type = Key;
    using mapped_type = Value;
    using compare_type = Compare;

    struct statistics {
      std::size_t nodes_created = 0;
      std::size_t path_copies = 0;
      std::size_t rotations = 0;
    };

    struct snapshot_type {
      snapshot_type() : compare_(std::make_shared<Compare const>()) {}
      snapshot_type(snapshot_type const &) = default;
      snapshot_type & operator=(snapshot_type const &) = default;
      snapshot_type(snapshot_type && other) noexcept
        : root_(std::move(other.root_)), compare_(other.compare_), size_(std::exchange(other.size_, 0)) {}
      snapshot_type & operator=(snapshot_type && other) noexcept {
        if (this != &other) {
          root_ = std::move(other.root_); compare_ = other.compare_; size_ = std::exchange(other.size_, 0);
        }
        return *this;
      }
      Value const * find(Key const & key) const { return find_in(root_, key, *compare_); }
      std::size_t size() const noexcept { return size_; }
      bool empty() const noexcept { return !size_; }
      template <class F> void for_each(F && visit) const { visit_in(root_, visit); }
    private:
      friend struct nursery_map;
      node_pointer root_;
      std::shared_ptr<Compare const> compare_;
      std::size_t size_ = 0;
      snapshot_type(node_pointer root, std::shared_ptr<Compare const> compare, std::size_t size) noexcept
        : root_(std::move(root)), compare_(std::move(compare)), size_(size) {}
    };

    explicit nursery_map(Compare compare = {}) : compare_(std::make_shared<Compare const>(std::move(compare))) {}
    nursery_map(nursery_map const &) = delete;
    nursery_map & operator=(nursery_map const &) = delete;
    nursery_map(nursery_map && other) noexcept
      : root_(std::move(other.root_)), compare_(other.compare_), edit_(std::move(other.edit_)),
        size_(std::exchange(other.size_, 0)), work_(std::exchange(other.work_, {})),
        failed_(std::exchange(other.failed_, false)) {}
    nursery_map & operator=(nursery_map && other) noexcept {
      if (this != &other) {
        root_ = std::move(other.root_); compare_ = other.compare_; edit_ = std::move(other.edit_);
        size_ = std::exchange(other.size_, 0); work_ = std::exchange(other.work_, {});
        failed_ = std::exchange(other.failed_, false);
      }
      return *this;
    }

    snapshot_type freeze() {
      require_active();
      if (edit_) edit_->open = false;
      return {root_, compare_, size_};
    }
    static nursery_map thaw(snapshot_type const & snapshot) {
      return nursery_map(snapshot.root_, snapshot.compare_, snapshot.size_);
    }
    Value const * find(Key const & key) const { require_active(); return find_in(root_, key, *compare_); }
    std::size_t size() const { require_active(); return size_; }
    bool empty() const { return !size(); }
    template <class F> void for_each(F && visit) const { require_active(); visit_in(root_, visit); }
    bool failed() const noexcept { return failed_; }
    statistics work() const noexcept { return work_; }

    // Returned pointers belong to this version. A transient pointer must not
    // survive another edit unless the caller retains a snapshot containing it.
    // Any exception inside an edit poisons that transient; existing snapshots
    // remain valid. Argument construction before entry leaves it unchanged.
    bool insert_or_assign(Key key, Value value) {
      require_active();
      try {
        begin_edit();
        auto inserted = insert(root_, key, value);
        size_ += std::size_t(inserted);
        return inserted;
      } catch (...) { failed_ = true; throw; }
    }
    bool erase(Key const & key) {
      require_active();
      try {
        begin_edit();
        auto erased = remove(root_, key);
        size_ -= std::size_t(erased);
        return erased;
      } catch (...) { failed_ = true; throw; }
    }

  private:
    node_pointer root_;
    std::shared_ptr<Compare const> compare_;
    std::shared_ptr<edit_token> edit_;
    std::size_t size_ = 0;
    statistics work_;
    bool failed_ = false;

    nursery_map(node_pointer root, std::shared_ptr<Compare const> compare, std::size_t size)
      : root_(std::move(root)), compare_(std::move(compare)), size_(size) {}
    void require_active() const {
      if (failed_) throw std::logic_error("failed nursery map");
    }
    void begin_edit() {
      if (!edit_ || !edit_->open) edit_ = std::make_shared<edit_token>();
    }
    void editable(node_pointer & item) {
      // A child's reference count cannot detect snapshots sharing its parent.
      // Only the current, still-open edit identity permits mutation in place.
      if (item->edit != edit_) {
        item = std::make_shared<node>(*item, edit_);
        ++work_.nodes_created; ++work_.path_copies;
      }
    }
    static unsigned height(node_pointer const & item) noexcept { return item ? item->height : 0; }
    static void refresh(node_pointer const & item) noexcept {
      item->height = 1 + std::max(height(item->left), height(item->right));
    }
    static int balance(node_pointer const & item) noexcept {
      return int(height(item->left)) - int(height(item->right));
    }
    void rotate_left(node_pointer & item) {
      editable(item->right);
      auto next = std::move(item->right);
      item->right = std::move(next->left);
      refresh(item);
      next->left = std::move(item);
      refresh(next);
      item = std::move(next);
      ++work_.rotations;
    }
    void rotate_right(node_pointer & item) {
      editable(item->left);
      auto next = std::move(item->left);
      item->left = std::move(next->right);
      refresh(item);
      next->right = std::move(item);
      refresh(next);
      item = std::move(next);
      ++work_.rotations;
    }
    void rebalance(node_pointer & item) {
      refresh(item);
      if (balance(item) > 1) {
        editable(item->left);
        if (balance(item->left) < 0) rotate_left(item->left);
        rotate_right(item);
      } else if (balance(item) < -1) {
        editable(item->right);
        if (balance(item->right) > 0) rotate_right(item->right);
        rotate_left(item);
      }
    }
    bool insert(node_pointer & item, Key & key, Value & value) {
      if (!item) {
        auto entry = std::make_shared<binding const>(std::move(key), std::move(value));
        item = std::make_shared<node>(std::move(entry), edit_);
        ++work_.nodes_created;
        return true;
      }
      bool inserted;
      if ((*compare_)(key, item->entry->key)) {
        editable(item); inserted = insert(item->left, key, value);
      } else if ((*compare_)(item->entry->key, key)) {
        editable(item); inserted = insert(item->right, key, value);
      } else {
        // Preserve the original representative of comparator-equivalent keys.
        auto entry = std::make_shared<binding const>(item->entry->key, std::move(value));
        editable(item); item->entry = std::move(entry);
        return false;
      }
      rebalance(item);
      return inserted;
    }
    binding_pointer remove_first(node_pointer & item) {
      if (!item->left) {
        auto entry = item->entry;
        auto next = item->right;
        item = std::move(next);
        return entry;
      }
      editable(item);
      auto first = remove_first(item->left);
      rebalance(item);
      return first;
    }
    bool remove(node_pointer & item, Key const & key) {
      if (!item) return false;
      bool erased;
      if ((*compare_)(key, item->entry->key)) {
        editable(item); erased = remove(item->left, key);
      } else if ((*compare_)(item->entry->key, key)) {
        editable(item); erased = remove(item->right, key);
      } else {
        if (!item->left || !item->right) {
          auto next = item->left ? item->left : item->right;
          item = std::move(next);
          return true;
        }
        editable(item); item->entry = remove_first(item->right);
        erased = true;
      }
      if (erased) rebalance(item);
      return erased;
    }
    static Value const * find_in(node_pointer const & root, Key const & key, Compare const & compare) {
      auto item = root.get();
      while (item) {
        if (compare(key, item->entry->key)) item = item->left.get();
        else if (compare(item->entry->key, key)) item = item->right.get();
        else return &item->entry->value;
      }
      return nullptr;
    }
    template <class F> static void visit_in(node_pointer const & item, F & visit) {
      if (!item) return;
      visit_in(item->left, visit);
      std::invoke(visit, std::as_const(item->entry->key), std::as_const(item->entry->value));
      visit_in(item->right, visit);
    }
  };
}
