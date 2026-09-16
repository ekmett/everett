/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Keeps acknowledged catalog identities with their shared immutable owners.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <diet/object_path.h>

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace diet {
  // Embedded in a shared immutable owner, rather than kept in a weak-owner
  // table. Catalog copies can share an identity, so the canonical local root
  // also distinguishes their file namespaces. This cache performs no I/O.
  template <class T> struct catalog_bindings {
    using pointer = std::shared_ptr<T const>;
  private:
    struct slot {
      object_id catalog;
      std::filesystem::path root;
      std::mutex mutex;
      pointer value;
      slot(object_id id, std::filesystem::path path) : catalog(std::move(id)), root(std::move(path)) {}
    };
  public:
    // Prepare every slot before acquiring a batch. Once any claim is held,
    // try_lock is the only permitted acquisition: unrelated producers may be
    // holding a parent while waiting for one of our dependencies.
    // A locked producer is thread-affine: move it only within that thread,
    // and install, release or destroy it on the thread which acquired it.
    struct producer {
      producer(producer const &) = delete;
      producer & operator=(producer const &) = delete;
      producer(producer &&) noexcept = default;
      producer & operator=(producer &&) = delete;
      bool try_lock() { return lock_.try_lock(); }
      pointer value() const {
        if (!lock_.owns_lock()) throw std::logic_error("unclaimed catalog owner");
        return entry_->value;
      }
      void release() noexcept { if (lock_.owns_lock()) lock_.unlock(); }
      void install(pointer value) {
        if (!lock_.owns_lock() || entry_->value || !value)
          throw std::logic_error("invalid catalog owner installation");
        entry_->value = std::move(value);
        lock_.unlock();
      }
    private:
      friend struct catalog_bindings;
      std::shared_ptr<slot> entry_;
      std::unique_lock<std::mutex> lock_;
      explicit producer(std::shared_ptr<slot> entry)
        : entry_(std::move(entry)), lock_(entry_->mutex, std::defer_lock) {}
    };
    producer prepare(object_id const & catalog, std::filesystem::path const & root) const {
      return producer(locate(catalog, root, true));
    }
    catalog_bindings() = default;
    catalog_bindings(catalog_bindings const &) = delete;
    catalog_bindings & operator=(catalog_bindings const &) = delete;

    pointer find(object_id const & catalog, std::filesystem::path const & root) const {
      auto entry = locate(catalog, root, false);
      if (!entry) return {};
      std::lock_guard lock(entry->mutex);
      return entry->value;
    }

    // A producer installs only an acknowledged immutable result. An exception
    // leaves the slot empty: another healthy backend can make a fresh attempt.
    // Producers may resolve immediate dependencies in an acyclic owner graph,
    // but must not retain locks on unrelated roots. Queries never use this lock.
    template <class F> pointer get_or_create(object_id const & catalog,
        std::filesystem::path const & root, F && produce) const {
      auto entry = locate(catalog, root, true);
      std::lock_guard lock(entry->mutex);
      if (!entry->value) {
        pointer value = std::invoke(std::forward<F>(produce));
        if (!value) throw std::logic_error("null catalog owner binding");
        entry->value = std::move(value);
      }
      return entry->value;
    }

  private:
    mutable std::mutex mutex_;
    mutable std::vector<std::shared_ptr<slot>> entries_;

    std::shared_ptr<slot> locate(object_id const & catalog, std::filesystem::path const & root, bool create) const {
      std::lock_guard lock(mutex_);
      for (auto const & entry : entries_)
        if (entry->catalog == catalog && entry->root == root) return entry;
      if (!create) return {};
      auto entry = std::make_shared<slot>(catalog, root);
      entries_.push_back(entry);
      return entry;
    }
  };
}
