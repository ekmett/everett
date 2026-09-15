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

#include <diet/cola_runtime.h>
#include <diet/sqlite_catalog.h>

#include <array>
#include <map>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/random.h>
#endif

namespace diet {
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

  struct runtime_checkpoint {
    std::vector<cola_runtime_interval> intervals;
    std::vector<std::byte> semantic;
  };

  namespace runtime_store_detail {
    inline constexpr std::array<std::byte, 8> magic{
      std::byte{'D'}, std::byte{'I'}, std::byte{'E'}, std::byte{'T'},
      std::byte{'R'}, std::byte{'T'}, std::byte{0}, std::byte{1}};

    template <class P> std::vector<std::byte> checkpoint(cola_runtime_snapshot<P> const & source,
        std::span<std::byte const> semantic) {
      std::vector<std::byte> out(magic.begin(), magic.end());
      catalog_detail::number(out, source.runs().size());
      for (auto const & run : source.runs()) {
        catalog_detail::number(out, run.first); catalog_detail::number(out, run.last);
      }
      catalog_detail::binary(out, semantic);
      return out;
    }
    inline runtime_checkpoint checkpoint(std::span<std::byte const> encoded) {
      if (encoded.size() < magic.size() || !std::equal(magic.begin(), magic.end(), encoded.begin()))
        throw std::invalid_argument("unsupported Diet runtime checkpoint");
      catalog_detail::outcome_reader input{encoded.subspan(magic.size())};
      auto count = input.number();
      if (count > 65 || count > input.data.size() / 16)
        throw std::invalid_argument("invalid Diet runtime interval count");
      runtime_checkpoint result;
      result.intervals.reserve(static_cast<std::size_t>(count));
      for (std::uint64_t i = 0; i != count; ++i) {
        auto first = input.number(), last = input.number();
        result.intervals.push_back({first, last});
      }
      auto size = input.number();
      if (size != input.data.size()) throw std::invalid_argument("invalid Diet runtime semantic extent");
      result.semantic.assign(input.data.begin(), input.data.end());
      return result;
    }
  }

  template <class P> struct stored_runtime {
    catalog_tap_head head;
    cola_runtime_snapshot<P> snapshot;
    std::vector<std::byte> semantic;
  };

  // Single-owner adapter over a trusted, existing, durable directory. All
  // persisted roots remain catalog-pinned. Private failures disable this
  // adapter; reopening selects only complete root/checkpoint publications.
  template <class P, class Ids = random_object_ids, class Ops = sqlite_catalog_ops> struct runtime_store {
    using policy_type = P;
    using snapshot_type = cola_runtime_snapshot<P>;
    using stored_type = stored_runtime<P>;
    using node_type = cola_runtime_node<P>;
    using native_type = cola_runtime_native<P>;
    using pair_type = typename node_type::pair_type;
    using native_pointer = typename node_type::native_pointer;
    using catalog_type = sqlite_catalog<P, Ops>;

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
        auto checkpoint = runtime_store_detail::checkpoint(source, semantic);
        auto op = operation();
        return restore(catalog_.create_tap(op, name, id, checkpoint));
      } catch (...) { failed_ = true; throw; }
    }
    stored_type publish(catalog_tap_head const & expected, snapshot_type const & source,
        std::span<std::byte const> semantic = {}) {
      require_active();
      try {
        auto id = persist(source);
        auto checkpoint = runtime_store_detail::checkpoint(source, semantic);
        auto op = operation();
        auto published = catalog_.publish_tap(op, expected, id, checkpoint);
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
    cache<node_type, blob_identity> nodes_;
    cache<native_type, object_id> natives_;
    bool failed_ = false;
    std::string last_operation_;

    runtime_store(catalog_type catalog, Ids ids) : catalog_(std::move(catalog)), ids_(std::move(ids)) {}
    void require_active() const { if (failed()) throw std::logic_error("failed Diet runtime store; reopen it"); }
    std::string operation() { last_operation_ = ids_().hex(); return last_operation_; }
    template <class T> static void prune(T & table) {
      for (auto i = table.begin(); i != table.end();) {
        if (i->first.expired()) i = table.erase(i); else ++i;
      }
    }
    stored_type restore(catalog_tap_head head) {
      auto checkpoint = runtime_store_detail::checkpoint(head.checkpoint);
      auto graph = open_mapped_cola_query<P>(root(), head.timeline.head);
      auto node = node_type::from_mapped(graph.head());
      auto snapshot = snapshot_type::restore(node, checkpoint.intervals);
      prune(nodes_); prune(natives_);
      for (auto current = node; current; current = current->main_target()) {
        auto const & ids = current->mapped()->identity();
        nodes_.insert_or_assign(current, ids);
        natives_.insert_or_assign(current->native_owner(), ids.native);
      }
      return {std::move(head), std::move(snapshot), std::move(checkpoint.semantic)};
    }
    blob_identity persist(snapshot_type const & source) {
      prune(nodes_); prune(natives_);
      struct output { pair_type node; blob_identity ids; bool native; };
      std::vector<output> outputs;
      cache<native_type, object_id> planned;
      std::vector<catalog_object_reservation> reservations;
      std::vector<blob_identity> inputs;
      std::optional<blob_identity> target;
      for (auto node = source.query_root().head(); node; node = node->main_target()) {
        if (auto known = nodes_.find(node); known != nodes_.end()) {
          target = known->second; inputs.push_back(*target); break;
        }
        if (!node->built())
          throw std::invalid_argument("mapped runtime snapshot was not opened by this store");
        auto native = node->native_owner();
        auto old = natives_.find(native), pending = planned.find(native);
        bool fresh = old == natives_.end() && pending == planned.end();
        auto id = old != natives_.end() ? old->second : pending != planned.end() ? pending->second : ids_();
        if (fresh) {
          if (!native->owned()) throw std::invalid_argument("mapped native was not opened by this store");
          planned.emplace(native, id); reservations.push_back({id, file_kind::native_blob});
        }
        blob_identity pair{id, ids_()};
        reservations.push_back({pair.index, file_kind::fractional_index});
        outputs.push_back({node, std::move(pair), fresh});
      }
      if (outputs.empty()) {
        if (!target) throw std::logic_error("runtime has no prepared root");
        return *target;
      }
      object_attempt_id attempt(ids_().hex());
      auto owner = ids_().hex();
      auto reserve_op = operation();
      catalog_.reserve(reserve_op, attempt, owner, inputs, reservations);
      // Native owners may be shared by several new indexes; seal each exactly
      // once before sealing indexes in dependency order.
      for (auto const & output : outputs) if (output.native) {
        auto encoded = encode_native_sections(*output.node->native_owner()->owned());
        auto receipt = encoded.seal(root(), output.ids.native, attempt);
        auto op = operation(); catalog_.record_sealed(op, receipt);
      }
      for (auto i = outputs.rbegin(); i != outputs.rend(); ++i) {
        auto encoded = encode_cola_sections(*i->node->built(), i->ids.native, target);
        auto receipt = encoded.seal(root(), i->ids.index, attempt);
        auto op = operation(); catalog_.record_sealed(op, receipt);
        target = i->ids;
      }
      auto graph = open_mapped_cola_query<P>(root(), *target);
      auto op = operation(); catalog_.register_chain(op, graph);
      for (auto const & output : outputs) {
        nodes_.insert_or_assign(output.node, output.ids);
        natives_.insert_or_assign(output.node->native_owner(), output.ids.native);
      }
      return *target;
    }
  };
}
