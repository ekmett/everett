/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Opens durable named typed taps with mutable commands and immutable saved colas.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <diet/fridge.h>
#include <diet/runtime_store.h>
#include <diet/redundant_checkpoint.h>
#include <diet/typed_cola.h>

#include <concepts>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace diet {
  struct connection_options {
    // Empty selects Core's default. Custom registries require an explicit,
    // stable identity for their ordering, codecs, hashing and semantics.
    std::string schema_id{};
    bool create_if_missing = true;
    tap_limits limits{128'000'000, 64 * 1024 * 1024, 64, 4096};
  };

  template <class Cola> struct stored_cola : Cola {
    stored_cola(Cola value, catalog_tap_head head) : Cola(std::move(value)), head_(std::move(head)) {}
    catalog_tap_head const & head() const & noexcept { return head_; }
    catalog_tap_head const & head() const && = delete;
  private:
    catalog_tap_head head_;
  };

  // Single-threaded durable Engine for tap. Core owns interpretation and
  // private merge continuations; the published snapshot is always mmap-backed.
  template <class Core = typed_engine<>, class Ids = random_object_ids> struct persistent_engine {
    using core_type = Core;
    using policy_type = typename Core::policy_type;
    using typed_cola_type = typename Core::cola_type;
    using cola_type = stored_cola<typed_cola_type>;
    using contribution_type = typename Core::contribution_type;
    using metadata_type = typename Core::metadata_type;
    using store_type = runtime_store<policy_type, Ids, sqlite_catalog_ops, typename Core::runtime_family>;

    // The directory must already exist durably. Catalog creation is exclusive;
    // a competing creator can fail this call, but no existing file is formatted.
    static persistent_engine connect(std::filesystem::path const & root, std::string_view name,
        connection_options const & options = {}, Ids ids = {}) {
      catalog_detail::name(name);
      Core seed = options.schema_id.empty() ? Core{} : Core(options.schema_id);
      auto initial = seed.snapshot();
      auto schema = initial.metadata().schema_id;
      auto store = open_store(root, options.create_if_missing, std::move(ids));
      auto found = store.find(name);
      if (!found) {
        if (!options.create_if_missing) throw std::out_of_range("unknown named Diet tap");
        found.emplace(store.create_tap(name, initial.runtime(), initial.metadata().encode()));
      }
      auto current = restore(std::move(*found), schema);
      auto core = Core::from_snapshot(current);
      return persistent_engine(std::move(store), std::move(core), std::move(current), std::move(schema));
    }
    persistent_engine(persistent_engine const &) = delete;
    persistent_engine & operator=(persistent_engine const &) = delete;
    persistent_engine(persistent_engine &&) noexcept(std::is_nothrow_move_constructible_v<store_type> &&
      std::is_nothrow_move_constructible_v<Core> && std::is_nothrow_move_constructible_v<cola_type>) = default;
    persistent_engine & operator=(persistent_engine &&) = delete;

    static tap_reservation reservation(contribution_type const & input) { return Core::reservation(input); }
    cola_type snapshot() const { return current_; }
    bool failed() const noexcept { return failed_ || core_.failed() || store_.failed(); }
    bool pending() const noexcept { return core_.pending(); }
    bool admission_ready() const noexcept { return !failed() && core_.admission_ready(); }
    std::string const & last_operation() const noexcept { return store_.last_operation(); }

    cola_type contribute(contribution_type input) {
      require_active();
      // Core's healthy failure contract means no logical update was admitted.
      // Encoding, old-value validation and an absent deletion can reject just
      // this ticket; publication failures may never take that path.
      auto updated = [&] {
        try { return core_.contribute(std::move(input)); }
        catch (...) { if (core_.failed()) failed_ = true; throw; }
      }();
      try { return publish(std::move(updated)); }
      catch (...) { failed_ = true; throw; }
    }
    std::optional<cola_type> advance(std::uint64_t budget) {
      require_active();
      try {
        auto updated = core_.advance(budget);
        if (!updated) return std::nullopt;
        return publish(std::move(*updated));
      } catch (...) { failed_ = true; throw; }
    }

  private:
    store_type store_;
    Core core_;
    cola_type current_;
    std::string schema_;
    bool failed_ = false;

    persistent_engine(store_type store, Core core, cola_type current, std::string schema)
      : store_(std::move(store)), core_(std::move(core)), current_(std::move(current)), schema_(std::move(schema)) {}
    static store_type open_store(std::filesystem::path const & root, bool create, Ids ids) {
      if (!std::filesystem::is_directory(root)) throw std::invalid_argument("Diet backing directory must already exist");
      if (std::filesystem::exists(root / "catalog.sqlite3") || !create)
        return store_type::open(root, std::move(ids));
      return store_type::create(root, std::move(ids));
    }
    static cola_type restore(typename store_type::stored_type saved, std::string_view schema) {
      auto metadata = metadata_type::decode(saved.semantic);
      auto cola = typed_cola_type::restore(std::move(saved.snapshot), std::move(metadata), schema);
      return {std::move(cola), std::move(saved.head)};
    }
    cola_type publish(typed_cola_type updated) {
      auto saved = store_.publish(current_.head(), updated.runtime(), updated.metadata().encode());
      auto mapped = restore(std::move(saved), schema_);
      // Keep partial builders alive until their carry completes. Restarting the
      // Core on every equivalent publication would discard paid private work.
      if (!core_.pending()) core_ = Core::from_snapshot(mapped);
      current_ = std::move(mapped);
      return current_;
    }
    void require_active() const { if (failed()) throw std::logic_error("failed persistent Diet engine; reconnect it"); }
  };

  // Ordinary callers use the mutable connection; snapshots retain the same
  // conditional update API as typed_cola. Catalog operations below open their
  // own connection and never touch the worker's SQLite handle.
  template <class Core = typed_engine<>> struct connection {
    using core_type = Core;
    using policy_type = typename Core::policy_type;
    using engine_type = persistent_engine<Core>;
    using tap_type = tap<engine_type>;
    using cola_type = typename engine_type::cola_type;
    using contribution_type = typename engine_type::contribution_type;
    using ticket = typename tap_type::ticket;
    using publication_type = typename tap_type::snapshot_type;
    using store_type = runtime_store<policy_type, random_object_ids, sqlite_catalog_ops, typename Core::runtime_family>;

    connection(std::filesystem::path const & root, std::string_view name, connection_options options = {})
      : root_(std::filesystem::canonical(root)), options_(checked_options(std::move(options))),
        tap_(engine_type::connect(root_, name, options_), options_.limits) {}
    connection(connection const &) = delete;
    connection & operator=(connection const &) = delete;
    connection(connection &&) = delete;
    connection & operator=(connection &&) = delete;

    cola_type snapshot() const { return tap_.snapshot()->cola; }
    publication_type publication() const noexcept { return tap_.snapshot(); }
    std::filesystem::path const & root() const & noexcept { return root_; }
    std::filesystem::path const & root() const && = delete;
    template <class S = typed_detail::default_sort_t<policy_type>> auto get(typed_detail::key_t<S> const & key) const {
      return tap_.snapshot()->cola.template get<S>(key);
    }
    template <class S = typed_detail::default_sort_t<policy_type>> cola_type put(
        typed_detail::key_t<S> const & key, typed_detail::state_t<S> const & value)
      requires typed_detail::replacement<S> { return apply(Core::template put<S>(key, value)); }
    template <class S = typed_detail::default_sort_t<policy_type>> cola_type erase(typed_detail::key_t<S> const & key)
      requires typed_detail::replacement<S> { return apply(Core::template erase<S>(key)); }
    template <class S = typed_detail::default_sort_t<policy_type>> cola_type change(
        typed_detail::key_t<S> const & key, typed_detail::arrow_t<S> const & arrow) {
      return apply(Core::template change<S>(key, arrow));
    }
    template <class S = typed_detail::default_sort_t<policy_type>> ticket put_async(
        typed_detail::key_t<S> const & key, typed_detail::state_t<S> const & value)
      requires typed_detail::replacement<S> { return submit(Core::template put<S>(key, value)); }
    template <class S = typed_detail::default_sort_t<policy_type>> ticket erase_async(typed_detail::key_t<S> const & key)
      requires typed_detail::replacement<S> { return submit(Core::template erase<S>(key)); }
    template <class S = typed_detail::default_sort_t<policy_type>> ticket change_async(
        typed_detail::key_t<S> const & key, typed_detail::arrow_t<S> const & arrow) {
      return submit(Core::template change<S>(key, arrow));
    }

    template <class C> requires std::same_as<std::remove_cvref_t<C>, contribution_type>
    std::optional<ticket> try_submit(C && value) { return tap_.try_submit(std::forward<C>(value)); }
    template <class C> requires std::same_as<std::remove_cvref_t<C>, contribution_type>
    ticket submit(C && value) { return tap_.submit(std::forward<C>(value)); }
    template <class C> requires std::same_as<std::remove_cvref_t<C>, contribution_type>
    cola_type apply(C && value) { return tap_.apply(std::forward<C>(value))->cola; }
    bool cancel(ticket const & value) { return tap_.cancel(value); }
    tap_reservation outstanding() const { return tap_.outstanding(); }
    std::uint64_t pending_count() const { return tap_.pending_count(); }
    std::exception_ptr failure() const { return tap_.failure(); }
    void close() { tap_.close(); }
    void shutdown() { tap_.shutdown(); }

    void save(std::string_view name) const { save(name, snapshot()); }
    void save(std::string_view name, cola_type const & state) const {
      auto storage = store_type::open(root_);
      storage.save(name, state.head());
    }
    std::optional<cola_type> load(std::string_view name) const {
      auto storage = store_type::open(root_);
      auto saved = storage.find_save(name);
      if (!saved) return std::nullopt;
      auto metadata = Core::metadata_type::decode(saved->semantic);
      auto cola = Core::cola_type::restore(std::move(saved->snapshot), std::move(metadata),
        tap_.snapshot()->cola.metadata().schema_id);
      return cola_type(std::move(cola), std::move(saved->head));
    }
    connection fork(std::string_view name) const { return fork(name, snapshot()); }
    connection fork(std::string_view name, cola_type const & state) const {
      auto storage = store_type::open(root_);
      (void)storage.fork(name, state.head());
      auto options = options_;
      options.create_if_missing = false;
      options.schema_id = state.metadata().schema_id;
      return connection(root_, name, std::move(options));
    }
  private:
    static connection_options checked_options(connection_options options) {
      if (!options.limits.contributions || !options.limits.maintenance_budget)
        throw std::invalid_argument("connection needs a positive contribution limit and maintenance budget");
      return options;
    }
    std::filesystem::path root_;
    connection_options options_;
    tap_type tap_;
  };

  template <class Core = typed_engine<>> connection<Core> connect(
      std::filesystem::path const & root, std::string_view name, connection_options options = {}) {
    return connection<Core>(root, name, std::move(options));
  }

  template <class P> auto fridge<P>::connect(std::string_view name) const -> tap {
    return tap(root(), name);
  }
  template <class P> auto fridge<P>::connect(std::string_view name, connection_options const & options) const -> tap {
    return tap(root(), name, options);
  }
}
