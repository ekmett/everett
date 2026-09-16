/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Seals runtime frontiers and restores named sessions through immutable mappings.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <everett/runtime_checkpoint.h>
#include <everett/runtime_graph_sealer.h>
#include <everett/runtime_registry.h>
#include <everett/sqlite_catalog.h>

#include <array>
#include <unordered_map>
#include <unordered_set>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/random.h>
#endif

namespace everett {
  // Physical and operation identities use OS entropy; semantic fingerprints
  // are unrelated. Catalog reservations and exclusive installation still
  // reject a collision instead of replacing an existing object.
  struct random_object_ids {
    object_id operator()() const {
#if defined(__APPLE__) || defined(__linux__)
      std::array<unsigned char, 16> bytes{};
      if (::getentropy(bytes.data(), bytes.size()))
        throw std::system_error(errno, std::generic_category(), "Everett identity entropy");
      constexpr char digits[] = "0123456789abcdef";
      std::string text(32, '0');
      for (std::size_t i = 0; i != bytes.size(); ++i) {
        text[i * 2] = digits[bytes[i] >> 4]; text[i * 2 + 1] = digits[bytes[i] & 15];
      }
      return object_id(std::move(text));
#else
      throw std::system_error(std::make_error_code(std::errc::operation_not_supported), "Everett identity entropy");
#endif
    }
  };

  template <class P, class Family = binary_runtime_family<P>> struct stored_runtime {
    catalog_session_head head;
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
      return runtime_store(catalog_type::create_sessions(root, identity, options, std::move(ops)), std::move(ids));
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
      auto head = catalog_.find_session(name);
      return head ? std::optional<stored_type>{restore(std::move(*head))} : std::nullopt;
    }
    std::optional<stored_type> find_save(std::string_view name) {
      require_active();
      auto head = catalog_.find_saved_session(name);
      return head ? std::optional<stored_type>{restore(std::move(*head))} : std::nullopt;
    }
    // Seal the complete visible/hidden frontier under construction owners.
    // This creates no named generation and grants no publication authority.
    void prepare(snapshot_type const & source) {
      require_active();
      try { (void)persist(source); }
      catch (...) { failed_ = true; throw; }
    }
    stored_type create_session(std::string_view name, snapshot_type const & source,
        std::span<std::byte const> semantic = {}) {
      require_active();
      try {
        auto id = persist(source);
        auto checkpoint = codec_type::encode(source, semantic,
          [&](pair_type const & pair) { return sealer().pair_id(pair); },
          [&](native_pointer const & native) { return sealer().native_id(native); });
        auto op = operation();
        return restore(catalog_.create_session(op, name, id.head, checkpoint, id.auxiliary), &source);
      } catch (...) { failed_ = true; throw; }
    }
    stored_type publish(catalog_session_head const & expected, snapshot_type const & source,
        std::span<std::byte const> semantic = {}) {
      require_active();
      try {
        auto id = persist(source);
        auto checkpoint = codec_type::encode(source, semantic,
          [&](pair_type const & pair) { return sealer().pair_id(pair); },
          [&](native_pointer const & native) { return sealer().native_id(native); });
        auto op = operation();
        auto published = catalog_.publish_session(op, expected, id.head, checkpoint, id.auxiliary);
        if (!published.published) throw std::runtime_error("named Everett session was advanced by another connection");
        return restore(std::move(published.head), &source);
      } catch (...) { failed_ = true; throw; }
    }
    stored_type fork(std::string_view name, catalog_session_head const & source) {
      require_active();
      try { auto op = operation(); return restore(catalog_.fork_session(op, name, source)); }
      catch (...) { failed_ = true; throw; }
    }
    void save(std::string_view name, catalog_session_head const & source) {
      require_active();
      try { auto op = operation(); catalog_.save_session(op, name, source); }
      catch (...) { failed_ = true; throw; }
    }
    std::string const & last_operation() const noexcept { return last_operation_; }

  private:
    using registry_type = runtime_store_detail::runtime_registry<node_type>;
    registry_type retained_;
    catalog_type catalog_;
    Ids ids_;
    bool failed_ = false;
    std::string last_operation_;

    runtime_store(catalog_type catalog, Ids ids) : catalog_(std::move(catalog)), ids_(std::move(ids)) {}
    void require_active() const { if (failed()) throw std::logic_error("failed Everett runtime store; reopen it"); }
    std::string operation() { last_operation_ = ids_().hex(); return last_operation_; }
    auto sealer() { return runtime_store_detail::graph_sealer<P, Ids, Ops, Family>(catalog_, ids_, &last_operation_); }
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
          store.sealer().bind_native(value, id);
          found = natives.emplace(id.hex(), std::move(value)).first;
        }
        return found->second;
      }
      pair_type pair(blob_identity const & id) {
        if (auto found = pairs.find(id.index.hex()); found != pairs.end()) {
          if (found->second->mapped()->identity() != id)
            throw std::invalid_argument("checkpoint index has another native identity");
          return found->second;
        }
        if (restricted) {
          throw std::invalid_argument("checkpoint pair has no durable pin");
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
          store.sealer().bind_pair(result, p->identity());
        }
        return result;
      }
      bool seed_native(object_id const & id, native_pointer value) {
        auto [found, inserted] = natives.try_emplace(id.hex(), value);
        return inserted || found->second == value;
      }
      bool seed_root(pair_type value) {
        auto const & id = value->mapped()->identity();
        auto [found, inserted] = pairs.try_emplace(id.index.hex(), value);
        return inserted || found->second == value;
      }
    };
    struct roots {
      std::vector<pair_type> pairs;
      std::vector<typename registry_type::native_root> natives;
    };
    template <class Resolver> static std::optional<roots> retained_roots(catalog_session_head const & head, Resolver & loaded) {
      roots result;
      result.pairs.reserve(head.auxiliary.pairs.size() + 1);
      result.natives.reserve(head.auxiliary.natives.size());
      auto pair = loaded.pair(head.timeline.head);
      if (!pair) return std::nullopt;
      result.pairs.push_back(std::move(pair));
      for (auto const & id : head.auxiliary.pairs) {
        pair = loaded.pair(id);
        if (!pair) return std::nullopt;
        result.pairs.push_back(std::move(pair));
      }
      for (auto const & id : head.auxiliary.natives) {
        auto native = loaded.native(id);
        if (!native) return std::nullopt;
        result.natives.emplace_back(id, std::move(native));
      }
      return result;
    }
    struct pinned_resolver {
      registry_type const & retained;
      pair_type pair(blob_identity const & id) const {
        auto value = retained.pair(id);
        if (!value) throw std::invalid_argument("checkpoint pair has no durable pin");
        return value;
      }
      native_pointer native(object_id const & id) const {
        auto value = retained.native(id);
        if (!value) throw std::invalid_argument("checkpoint native has no durable pin");
        return value;
      }
    };
    stored_type decode_retained(catalog_session_head head) {
      pinned_resolver loaded{retained_};
      try {
        auto restored = codec_type::decode(head.checkpoint, head.timeline.head, loaded);
        return {std::move(head), std::move(restored.snapshot), std::move(restored.semantic)};
      } catch (...) {
        // A failed checkpoint never leaves an ambiguous authorization cache.
        retained_.clear(); throw;
      }
    }
    stored_type restore(catalog_session_head head, snapshot_type const * source = nullptr) {
      if (source) {
        resolver candidates(*this);
        auto graph = sealer();
        bool coherent = true;
        codec_type::collect(*source, [&](pair_type const & pair) {
            if (pair) coherent = candidates.seed_root(graph.mapped_pair(pair)) && coherent;
          }, [&](native_pointer const & native) {
            if (native) coherent = candidates.seed_native(graph.native_id(native), graph.mapped_native(native)) && coherent;
          });
        // Candidate roots provide owners, not authority. Only the roots in the
        // acknowledged catalog head acquire registry references. Shared live
        // owners stop at their local count; new edges are checked once.
        if (coherent) {
          candidates.restricted = true;
          auto selected = retained_roots(head, candidates);
          if (!selected) throw std::logic_error("published root has no prepared owner");
          if (retained_.replace(std::move(selected->pairs), std::move(selected->natives)))
            return decode_retained(std::move(head));
        }
      } else if (auto selected = retained_roots(head, retained_)) {
        if (retained_.replace(std::move(selected->pairs), std::move(selected->natives)))
          return decode_retained(std::move(head));
      }
      // An imported graph may contain distinct owners for the same physical
      // identity, even deep in a new prefix. Intern the entire incoming graph
      // afresh in that exceptional case. The old registry remains intact until
      // this new closure has been validated and decoded.
      resolver loaded(*this);
      auto selected = retained_roots(head, loaded);
      if (!selected) throw std::logic_error("durable root has no mapped owner");
      loaded.restricted = true;
      registry_type replacement;
      if (!replacement.replace(std::move(selected->pairs), std::move(selected->natives)))
        throw std::invalid_argument("mapped checkpoint has conflicting facade identities");
      pinned_resolver pinned{replacement};
      auto restored = codec_type::decode(head.checkpoint, head.timeline.head, pinned);
      retained_ = std::move(replacement);
      return {std::move(head), std::move(restored.snapshot), std::move(restored.semantic)};
    }
    struct persisted { blob_identity head; catalog_auxiliary_roots auxiliary; };
    persisted persist(snapshot_type const & source) {
      std::vector<pair_type> roots;
      std::vector<native_pointer> native_roots;
      std::unordered_set<node_type const *> seen_pairs;
      std::unordered_set<native_type const *> seen_natives;
      codec_type::collect(source, [&](pair_type value) {
          if (value && seen_pairs.insert(value.get()).second) roots.push_back(std::move(value));
        }, [&](native_pointer value) {
          if (value && seen_natives.insert(value.get()).second) native_roots.push_back(std::move(value));
        });
      auto graph = sealer();
      (void)graph.prepare_ready(roots, native_roots);
      // Each new owner resolves its immediate dependencies once. An existing
      // bound suffix stops this walk, irrespective of how many roots share it.
      // Finish each remaining root before starting another. The ready batch
      // has released every claim before this blocking dependency walk.
      for (auto const & pair : roots) (void)graph.ensure_pair(pair);
      for (auto const & native : native_roots) (void)graph.ensure_native(native);
      auto primary = graph.pair_id(source.query_root().head());
      catalog_auxiliary_roots retained;
      for (auto const & pair : roots) retained.pairs.push_back(graph.pair_id(pair));
      for (auto const & native : native_roots) retained.natives.push_back(graph.native_id(native));
      return {primary, catalog_detail::canonical_auxiliary(std::move(retained), primary)};
    }
  };
}
