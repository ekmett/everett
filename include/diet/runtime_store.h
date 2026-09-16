/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Seals runtime frontiers and restores named taps through immutable mappings.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <diet/runtime_checkpoint.h>
#include <diet/sqlite_catalog.h>

#include <array>
#include <map>
#include <unordered_map>
#include <unordered_set>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/random.h>
#endif

namespace diet {
  namespace runtime_store_detail {
    // Owner identity, not a mapped address, determines reuse. A rotating
    // cursor bounds housekeeping even when callers retain many snapshots.
    template <class T, class V> struct owner_cache {
      using key_type = std::weak_ptr<T const>;
      using map_type = std::map<key_type, V, std::owner_less<key_type>>;
      using iterator = typename map_type::iterator;

      owner_cache() : next_(entries_.end()) {}
      owner_cache(owner_cache const &) = delete;
      owner_cache & operator=(owner_cache const &) = delete;
      owner_cache(owner_cache && other) noexcept : owner_cache() { swap(other); }
      owner_cache & operator=(owner_cache && other) noexcept {
        if (this != &other) { owner_cache moved(std::move(other)); swap(moved); }
        return *this;
      }
      void swap(owner_cache & other) noexcept {
        auto at_end = next_ == entries_.end(), other_at_end = other.next_ == other.entries_.end();
        entries_.swap(other.entries_);
        std::swap(next_, other.next_);
        // Element iterators survive map::swap, but past-end iterators do not
        // transfer to the other container's sentinel.
        if (other_at_end) next_ = entries_.end();
        if (at_end) other.next_ = other.entries_.end();
      }
      iterator find(key_type const & key) { return entries_.find(key); }
      iterator end() noexcept { return entries_.end(); }
      V const & at(key_type const & key) const { return entries_.at(key); }
      std::size_t size() const noexcept { return entries_.size(); }
      void insert_or_assign(key_type key, V value) {
        // New facade owners bring their own small cleanup allowance. A large
        // restored frontier cannot grow the cache faster than it is inspected.
        prune(2);
        entries_.insert_or_assign(std::move(key), std::move(value));
      }
      std::size_t prune(std::size_t budget = 16) {
        if (next_ == entries_.end()) next_ = entries_.begin();
        std::size_t examined = 0;
        while (examined != budget && next_ != entries_.end()) {
          auto current = next_++;
          if (current->first.expired()) entries_.erase(current);
          ++examined;
        }
        return examined;
      }

    private:
      map_type entries_;
      iterator next_;
    };
  }

  // Physical and operation identities use OS entropy; semantic fingerprints
  // are unrelated. Catalog reservations and exclusive installation still
  // reject a collision instead of replacing an existing object.
  struct random_object_ids {
    object_id operator()() const {
#if defined(__APPLE__) || defined(__linux__)
      std::array<unsigned char, 16> bytes{};
      if (::getentropy(bytes.data(), bytes.size()))
        throw std::system_error(errno, std::generic_category(), "Diet identity entropy");
      constexpr char digits[] = "0123456789abcdef";
      std::string text(32, '0');
      for (std::size_t i = 0; i != bytes.size(); ++i) {
        text[i * 2] = digits[bytes[i] >> 4]; text[i * 2 + 1] = digits[bytes[i] & 15];
      }
      return object_id(std::move(text));
#else
      throw std::system_error(std::make_error_code(std::errc::operation_not_supported), "Diet identity entropy");
#endif
    }
  };

  template <class P, class Family = binary_runtime_family<P>> struct stored_runtime {
    catalog_tap_head head;
    typename Family::snapshot_type snapshot;
    std::vector<std::byte> semantic;
  };

  // Single-owner adapter over a trusted, existing, durable directory. All
  // persisted roots remain catalog-pinned. Private failures disable this
  // adapter; reopening selects only complete root/checkpoint publications.
  template <class P, class Ids = random_object_ids, class Ops = sqlite_catalog_ops, class Family = binary_runtime_family<P>> struct runtime_store {
    using policy_type = P;
    using family_type = Family;
    using codec_type = runtime_storage_codec<Family>;
    using snapshot_type = typename Family::snapshot_type;
    using stored_type = stored_runtime<P, Family>;
    using node_type = typename Family::node_type;
    using native_type = typename Family::native_type;
    using pair_type = typename node_type::pair_type;
    using native_pointer = typename node_type::native_pointer;
    using mapped_pointer = decltype(std::declval<node_type const &>().mapped());
    using mapped_type = std::remove_const_t<typename mapped_pointer::element_type>;
    using mapped_resolver = mapped_cola_resolver<P, mapped_type>;
    using catalog_type = sqlite_catalog<P, Ops>;
    static_assert(std::is_same_v<P, typename Family::policy_type>);

    static runtime_store create(std::filesystem::path const & root, Ids ids = {},
        catalog_options options = {}, Ops ops = {}) {
      auto identity = ids();
      return runtime_store(catalog_type::create_taps(root, identity, options, std::move(ops)), std::move(ids));
    }
    static runtime_store open(std::filesystem::path const & root, Ids ids = {},
        catalog_options options = {}, Ops ops = {}) {
      auto db = catalog_type::open(root, options, std::move(ops));
      if (db.schema_version() != 4) throw std::invalid_argument("named runtime needs catalog version 4");
      return runtime_store(std::move(db), std::move(ids));
    }
    runtime_store(runtime_store const &) = delete;
    runtime_store & operator=(runtime_store const &) = delete;
    runtime_store(runtime_store &&) noexcept(std::is_nothrow_move_constructible_v<Ids>) = default;
    runtime_store & operator=(runtime_store &&) = delete;
    bool failed() const noexcept { return failed_ || catalog_.poisoned(); }
    std::filesystem::path const & root() const & noexcept { return catalog_.root(); }
    std::filesystem::path const & root() const && = delete;

    std::optional<stored_type> find(std::string_view name) {
      require_active();
      auto head = catalog_.find_tap(name);
      return head ? std::optional<stored_type>{restore(std::move(*head))} : std::nullopt;
    }
    std::optional<stored_type> find_save(std::string_view name) {
      require_active();
      auto head = catalog_.find_saved_tap(name);
      return head ? std::optional<stored_type>{restore(std::move(*head))} : std::nullopt;
    }
    stored_type create_tap(std::string_view name, snapshot_type const & source,
        std::span<std::byte const> semantic = {}) {
      require_active();
      try {
        auto id = persist(source);
        auto checkpoint = codec_type::encode(source, semantic,
          [&](pair_type const & pair) { return nodes_.at(pair); },
          [&](native_pointer const & native) { return natives_.at(native); });
        auto op = operation();
        return restore(catalog_.create_tap(op, name, id.head, checkpoint, id.auxiliary));
      } catch (...) { failed_ = true; throw; }
    }
    stored_type publish(catalog_tap_head const & expected, snapshot_type const & source,
        std::span<std::byte const> semantic = {}) {
      require_active();
      try {
        auto id = persist(source);
        auto checkpoint = codec_type::encode(source, semantic,
          [&](pair_type const & pair) { return nodes_.at(pair); },
          [&](native_pointer const & native) { return natives_.at(native); });
        auto op = operation();
        auto published = catalog_.publish_tap(op, expected, id.head, checkpoint, id.auxiliary);
        if (!published.published) throw std::runtime_error("named Diet tap was advanced by another connection");
        return restore(std::move(published.head));
      } catch (...) { failed_ = true; throw; }
    }
    stored_type fork(std::string_view name, catalog_tap_head const & source) {
      require_active();
      try { auto op = operation(); return restore(catalog_.fork_tap(op, name, source)); }
      catch (...) { failed_ = true; throw; }
    }
    void save(std::string_view name, catalog_tap_head const & source) {
      require_active();
      try { auto op = operation(); catalog_.save_tap(op, name, source); }
      catch (...) { failed_ = true; throw; }
    }
    std::string const & last_operation() const noexcept { return last_operation_; }

  private:
    template <class T, class V> using cache = std::map<std::weak_ptr<T const>, V,
      std::owner_less<std::weak_ptr<T const>>>;
    catalog_type catalog_;
    Ids ids_;
    runtime_store_detail::owner_cache<node_type, blob_identity> nodes_;
    runtime_store_detail::owner_cache<native_type, object_id> natives_;
    bool failed_ = false;
    std::string last_operation_;

    runtime_store(catalog_type catalog, Ids ids) : catalog_(std::move(catalog)), ids_(std::move(ids)) {}
    void require_active() const { if (failed()) throw std::logic_error("failed Diet runtime store; reopen it"); }
    std::string operation() { last_operation_ = ids_().hex(); return last_operation_; }
    // Mapping one checkpoint shares physical mappings and typed facades across
    // every visible and hidden root, preserving exact immutable dependencies.
    struct resolver {
      runtime_store & store;
      mapped_resolver mapped;
      std::unordered_map<std::string, native_pointer> natives;
      std::unordered_map<std::string, pair_type> pairs;
      bool restricted = false;
      explicit resolver(runtime_store & value) : store(value), mapped(value.root()) {}
      native_pointer native(object_id const & id) {
        auto found = natives.find(id.hex());
        if (found == natives.end()) {
          if (restricted) throw std::invalid_argument("checkpoint native has no durable pin");
          auto value = native_type::from_mapped(mapped.native(id));
          store.natives_.insert_or_assign(value, id);
          found = natives.emplace(id.hex(), std::move(value)).first;
        }
        return found->second;
      }
      pair_type pair(blob_identity const & id) {
        if (restricted) {
          auto found = pairs.find(id.index.hex());
          if (found == pairs.end() || found->second->mapped()->identity() != id)
            throw std::invalid_argument("checkpoint pair has no durable pin");
          return found->second;
        }
        auto physical = mapped.pair(id);
        std::vector<mapped_pointer> pending;
        pair_type result;
        for (auto p = physical; p; p = p->main_target()) {
          if (auto known = pairs.find(p->identity().index.hex()); known != pairs.end()) { result = known->second; break; }
          pending.push_back(p);
        }
        for (auto i = pending.rbegin(); i != pending.rend(); ++i) {
          auto const & p = *i;
          result = node_type::from_mapped_parts(p, native(p->identity().native), result,
            p->index_object()->secondary_id() ? native(*p->index_object()->secondary_id()) : native_pointer{});
          pairs.emplace(p->identity().index.hex(), result);
          store.nodes_.insert_or_assign(result, p->identity());
        }
        return result;
      }
    };
    stored_type restore(catalog_tap_head head) {
      nodes_.prune(); natives_.prune();
      resolver loaded(*this);
      // Load retained roots first, both to validate every durable pin and to
      // make all subsequent checkpoint references share their exact owners.
      (void)loaded.pair(head.timeline.head);
      for (auto const & pair : head.auxiliary.pairs) (void)loaded.pair(pair);
      for (auto const & native : head.auxiliary.natives) (void)loaded.native(native);
      loaded.restricted = true;
      auto restored = codec_type::decode(head.checkpoint, head.timeline.head, loaded);
      return {std::move(head), std::move(restored.snapshot), std::move(restored.semantic)};
    }
    struct persisted { blob_identity head; catalog_auxiliary_roots auxiliary; };
    persisted persist(snapshot_type const & source) {
      nodes_.prune(); natives_.prune();
      std::vector<pair_type> roots, outputs;
      std::vector<native_pointer> native_roots, native_outputs;
      cache<node_type, blob_identity> planned_pairs;
      cache<native_type, object_id> planned_natives;
      std::vector<catalog_object_reservation> reservations;
      std::vector<blob_identity> inputs;
      std::unordered_set<node_type const *> seen_pairs;
      std::unordered_set<native_type const *> seen_natives;
      codec_type::collect(source, [&](pair_type value) {
          if (value && seen_pairs.insert(value.get()).second) roots.push_back(std::move(value));
        }, [&](native_pointer value) {
          if (value && seen_natives.insert(value.get()).second) native_roots.push_back(std::move(value));
        });
      auto plan_native = [&](native_pointer const & native) {
        if (auto known = natives_.find(native); known != natives_.end()) return known->second;
        if (auto known = planned_natives.find(native); known != planned_natives.end()) return known->second;
        if (native) {
          if constexpr (requires { native->sealed(); }) if (auto seal = native->sealed()) {
            if (!native->mapped() || seal->catalog != catalog_.identity())
              throw std::invalid_argument("sealed native belongs to another backing catalog");
            catalog_.verify_sealed(seal->receipt, file_kind::native_blob);
            planned_natives.emplace(native, seal->receipt.object);
            return seal->receipt.object;
          }
        }
        if (!native || !native->owned()) throw std::invalid_argument("mapped native was not opened by this store");
        auto id = ids_(); planned_natives.emplace(native, id); native_outputs.push_back(native);
        reservations.push_back({id, file_kind::native_blob}); return id;
      };
      std::unordered_set<node_type const *> visiting;
      auto plan_pair = [&](auto && self, pair_type const & pair) -> blob_identity {
        if (auto known = nodes_.find(pair); known != nodes_.end()) { inputs.push_back(known->second); return known->second; }
        if (auto known = planned_pairs.find(pair); known != planned_pairs.end()) return known->second;
        if (!pair || !pair->built()) throw std::invalid_argument("mapped runtime snapshot was not opened by this store");
        if (!visiting.insert(pair.get()).second) throw std::invalid_argument("cyclic runtime pair graph");
        if (pair->main_target()) (void)self(self, pair->main_target());
        if (pair->secondary_target()) (void)plan_native(pair->secondary_target());
        blob_identity id{plan_native(pair->native_owner()), ids_()};
        planned_pairs.emplace(pair, id); reservations.push_back({id.index, file_kind::fractional_index});
        outputs.push_back(pair); visiting.erase(pair.get()); return id;
      };
      for (auto const & pair : roots) (void)plan_pair(plan_pair, pair);
      for (auto const & native : native_roots) (void)plan_native(native);
      if (!reservations.empty()) {
        object_attempt_id attempt(ids_().hex()); auto owner = ids_().hex();
        std::sort(inputs.begin(), inputs.end(), [](auto const & a, auto const & b) { return a.index.hex() < b.index.hex(); });
        inputs.erase(std::unique(inputs.begin(), inputs.end()), inputs.end());
        auto op = operation(); catalog_.reserve(op, attempt, owner, inputs, reservations);
        for (auto const & native : native_outputs) {
          auto encoded = [&] {
            if constexpr (requires { typename Family::storage_type; }) return Family::storage_type::encode_native(*native->owned());
            else return encode_native_sections(*native->owned());
          }();
          auto receipt = encoded.seal(root(), planned_natives.at(native), attempt);
          op = operation(); catalog_.record_sealed(op, receipt);
        }
        auto native_id = [&](native_pointer const & native) {
          auto found = planned_natives.find(native); return found == planned_natives.end() ? natives_.at(native) : found->second;
        };
        auto pair_id = [&](pair_type const & pair) {
          auto found = planned_pairs.find(pair); return found == planned_pairs.end() ? nodes_.at(pair) : found->second;
        };
        for (auto const & pair : outputs) {
          auto id = pair_id(pair);
          auto encoded = encode_cola_sections(*pair->built(), id.native,
            pair->main_target() ? std::optional<blob_identity>(pair_id(pair->main_target())) : std::nullopt,
            pair->secondary_target() ? std::optional<object_id>(native_id(pair->secondary_target())) : std::nullopt);
          auto receipt = encoded.seal(root(), id.index, attempt);
          op = operation(); catalog_.record_sealed(op, receipt);
        }
        mapped_resolver loaded(root());
        std::vector<mapped_pointer> graphs;
        for (auto const & pair : roots) graphs.push_back(loaded.pair(pair_id(pair)));
        if (!graphs.empty()) {
          op = operation(); catalog_.template register_graphs<mapped_type>(op, graphs);
        }
      }
      // A newly sealed hidden native can arrive without any new pair output.
      // Its validated attestation still participates in the checkpoint's pins.
      for (auto const & [weak, id] : planned_pairs) nodes_.insert_or_assign(weak, id);
      for (auto const & [weak, id] : planned_natives) natives_.insert_or_assign(weak, id);
      auto primary = nodes_.at(source.query_root().head());
      catalog_auxiliary_roots retained;
      for (auto const & pair : roots) retained.pairs.push_back(nodes_.at(pair));
      for (auto const & native : native_roots) retained.natives.push_back(natives_.at(native));
      return {primary, catalog_detail::canonical_auxiliary(std::move(retained), primary)};
    }
  };
}
