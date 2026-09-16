/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Opens durable named typed sessions with mutable commands and immutable saved worlds.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <everett/multiverse.h>
#include <everett/active_engine.h>
#include <everett/runtime_store.h>
#include <everett/private_construction.h>
#include <everett/redundant_checkpoint.h>
#include <everett/sort_runtime_store.h>
#include <everett/typed_world.h>
#include <everett/typed_scan.h>

#include <concepts>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace everett {
  struct connection_options {
    // Empty selects Core's default. Custom registries require an explicit,
    // stable identity for their ordering, codecs, hashing and semantics.
    std::string schema_id{};
    bool create_if_missing = true;
    // Omitted limits use this Core's quote for 1024 record-equivalents.
    // Explicit limits are honored exactly, including a zero work/byte limit.
    std::optional<session_limits> limits{};
  };

  namespace connection_detail { struct logical_state {}; }
  template <class Core> struct transaction;

  struct transaction_conflict : std::runtime_error {
    transaction_conflict() : std::runtime_error("transaction base is no longer current") {}
  };

  template <class World> struct stored_world : World {
    stored_world(World value, catalog_session_head head,
        std::shared_ptr<connection_detail::logical_state const> logical = std::make_shared<connection_detail::logical_state>())
      : World(std::move(value)), head_(std::make_shared<catalog_session_head const>(std::move(head))), logical_(std::move(logical)) {}
    catalog_session_head const & head() const & noexcept { return *head_; }
    catalog_session_head const & head() const && = delete;
    auto logical_identity() const noexcept { return logical_; }
  private:
    std::shared_ptr<catalog_session_head const> head_;
    std::shared_ptr<connection_detail::logical_state const> logical_;
  };

  namespace connection_detail {
    template <class Core> auto restore_checkpoint(typename Core::runtime_family::snapshot_type runtime,
        std::span<std::byte const> semantic, std::string_view schema) {
      if constexpr (requires { Core::restore_checkpoint(std::move(runtime), semantic, schema); })
        return Core::restore_checkpoint(std::move(runtime), semantic, schema);
      else {
        auto metadata = Core::metadata_type::decode(semantic);
        return Core::world_type::restore(std::move(runtime), std::move(metadata), schema);
      }
    }
  }

  // Single-threaded durable Engine for session. Core owns interpretation and
  // private merge continuations; the published snapshot is always mmap-backed.
  template <class Core = active_engine<>, class Ids = random_object_ids> struct persistent_engine {
    using core_type = Core;
    using family_type = typename Core::runtime_family;
    using policy_type = typename Core::policy_type;
    using typed_world_type = typename Core::world_type;
    using world_type = stored_world<typed_world_type>;
    using ordinary_contribution = typename Core::contribution_type;
    struct prepared_transaction {
      std::shared_ptr<private_construction<policy_type>> scope;
      world_type base;
      std::unique_ptr<Core> core;
      prepared_transaction(std::unique_ptr<Core> candidate, world_type original,
          std::shared_ptr<private_construction<policy_type>> lease)
        : scope(std::move(lease)), base(std::move(original)), core(std::move(candidate)) {}
    };
    // The temporary borrowed form lets session reserve capacity before it copies
    // or moves an ordinary command. Only an owning form enters the worker queue.
    struct contribution_type {
      struct borrowed { ordinary_contribution const * copy; ordinary_contribution * move; };
      using value_type = std::variant<ordinary_contribution, prepared_transaction, borrowed>;
      value_type value;
      contribution_type(ordinary_contribution input) : value(std::move(input)) {}
      contribution_type(prepared_transaction input) : value(std::move(input)) {}
      explicit contribution_type(borrowed input) : value(input) {}
      contribution_type(contribution_type const &) = delete;
      contribution_type & operator=(contribution_type const &) = delete;
      contribution_type(contribution_type && other) : value(take(other)) {}
      contribution_type & operator=(contribution_type &&) = delete;
      template <class C> requires std::constructible_from<ordinary_contribution, C &&>
      static contribution_type borrow(C && input) {
        if constexpr (std::is_rvalue_reference_v<C &&> && !std::is_const_v<std::remove_reference_t<C>>)
          return contribution_type(borrowed{&input, &input});
        else return contribution_type(borrowed{&input, nullptr});
      }
    private:
      static auto take(contribution_type & other) -> value_type {
        if (auto p = std::get_if<borrowed>(&other.value)) {
          if (p->move) return value_type(std::in_place_index<0>, std::move(*p->move));
          if constexpr (std::copy_constructible<ordinary_contribution>)
            return value_type(std::in_place_index<0>, *p->copy);
          else throw std::logic_error("move-only command requires an rvalue");
        }
        return std::move(other.value);
      }
    };
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
        if (!options.create_if_missing) throw std::out_of_range("unknown named Everett session");
        found.emplace(store.create_session(name, initial.runtime(), initial.metadata().encode()));
      }
      auto current = restore(std::move(*found), schema);
      auto core = [&] {
        if constexpr (requires { family_type::open_storage(store.root(), schema); })
          return Core::from_snapshot(current, family_type::open_storage(store.root(), schema));
        else if constexpr (requires { family_type::open_storage(store.root()); })
          return Core::from_snapshot(current, family_type::open_storage(store.root()));
        else return Core::from_snapshot(current);
      }();
      return persistent_engine(std::move(store), std::move(core), std::move(current), std::move(schema));
    }
    persistent_engine(persistent_engine const &) = delete;
    persistent_engine & operator=(persistent_engine const &) = delete;
    persistent_engine(persistent_engine &&) noexcept(std::is_nothrow_move_constructible_v<store_type> &&
      std::is_nothrow_move_constructible_v<Core> && std::is_nothrow_move_constructible_v<world_type>) = default;
    persistent_engine & operator=(persistent_engine &&) = delete;

    template <class C> requires std::same_as<std::remove_cvref_t<C>, ordinary_contribution> &&
      std::constructible_from<ordinary_contribution, C &&>
    static contribution_type borrow_contribution(C && input) {
      return contribution_type::borrow(std::forward<C>(input));
    }
    static session_reservation reservation(contribution_type const & input) {
      if (auto ordinary = std::get_if<ordinary_contribution>(&input.value)) return Core::reservation(*ordinary);
      if (auto borrowed = std::get_if<typename contribution_type::borrowed>(&input.value))
        return Core::reservation(*borrowed->copy);
      // Private execution has already paid its structural work. The queue still
      // reserves one contribution until publication is acknowledged.
      return {};
    }
    world_type snapshot() const { return current_; }
    bool failed() const noexcept { return failed_ || core_->failed() || store_.failed(); }
    bool pending() const noexcept { return core_->pending(); }
    bool admission_ready() const noexcept { return !failed() && core_->admission_ready(); }
    std::string const & last_operation() const noexcept {
      if (catalog_failure_) {
        try { std::rethrow_exception(catalog_failure_); }
        catch (catalog_error const & error) { return error.operation; }
        catch (...) {}
      }
      return store_.last_operation();
    }

    world_type contribute(contribution_type input) {
      require_active();
      if (auto prepared = std::get_if<prepared_transaction>(&input.value))
        return install(std::move(*prepared));
      if (!std::holds_alternative<ordinary_contribution>(input.value))
        throw std::logic_error("borrowed contribution escaped admission");
      return contribute_ordinary(std::move(std::get<ordinary_contribution>(input.value)));
    }
    world_type contribute_ordinary(ordinary_contribution input) {
      require_active();
      // Only empty input needs this comparison. A pending Core can own a
      // different physical layout from the last mapped durable publication.
      std::optional<typed_world_type> before;
      if constexpr (requires { input.records().empty(); })
        if (input.records().empty()) before.emplace(core_->snapshot());
      // Core's healthy failure contract means no logical update was admitted.
      // Encoding, old-value validation and an absent deletion can reject just
      // this ticket; publication failures may never take that path.
      auto updated = [&] {
        try { return core_->contribute(std::move(input)); }
        catch (...) { remember_failure(); if (core_->failed()) poison(); throw; }
      }();
      try {
        if (before && updated.runtime().same_layout(before->runtime()) && updated.metadata() == before->metadata())
          return current_;
        return publish(std::move(updated), true);
      } catch (...) { remember_failure(); poison(); throw; }
    }
    std::optional<world_type> advance(std::uint64_t budget) {
      require_active();
      try {
        auto updated = core_->advance(budget);
        if (!updated) return std::nullopt;
        return publish(std::move(*updated), false);
      } catch (...) { remember_failure(); poison(); throw; }
    }

  private:
    store_type store_;
    std::shared_ptr<private_construction<policy_type>> core_scope_;
    std::unique_ptr<Core> core_;
    world_type current_;
    std::string schema_;
    std::exception_ptr catalog_failure_;
    bool failed_ = false;

    persistent_engine(store_type store, Core core, world_type current, std::string schema)
      : store_(std::move(store)), core_(std::make_unique<Core>(std::move(core))), current_(std::move(current)), schema_(std::move(schema)) {}
    static store_type open_store(std::filesystem::path const & root, bool create, Ids ids) {
      if (!std::filesystem::is_directory(root)) throw std::invalid_argument("Everett backing directory must already exist");
      if (std::filesystem::exists(root / "catalog.sqlite3") || !create)
        return store_type::open(root, std::move(ids));
      return store_type::create(root, std::move(ids));
    }
    static world_type restore(typename store_type::stored_type saved, std::string_view schema,
        std::shared_ptr<connection_detail::logical_state const> logical = std::make_shared<connection_detail::logical_state>()) {
      auto world = connection_detail::restore_checkpoint<Core>(std::move(saved.snapshot), saved.semantic, schema);
      return {std::move(world), std::move(saved.head), std::move(logical)};
    }
    world_type publish(typed_world_type updated, bool mutation) {
      auto saved = store_.publish(current_.head(), updated.runtime(), updated.metadata().encode());
      auto mapped = restore(std::move(saved), schema_, mutation ?
        std::make_shared<connection_detail::logical_state>() : current_.logical_identity());
      // Keep partial builders alive until their carry completes. Restarting the
      // Core on every equivalent publication would discard paid private work.
      if (!core_->pending()) {
        if constexpr (requires { core_->rebase(mapped); }) core_->rebase(mapped);
        else core_ = std::make_unique<Core>(Core::from_snapshot(mapped));
      }
      current_ = std::move(mapped);
      catalog_failure_ = nullptr;
      return current_;
    }
    world_type install(prepared_transaction input) {
      if (input.base.logical_identity() != current_.logical_identity()) throw transaction_conflict();
      if (!input.core) return current_;
      try {
        auto updated = input.core->snapshot();
        auto saved = store_.publish(current_.head(), updated.runtime(), updated.metadata().encode());
        auto mapped = restore(std::move(saved), schema_);
        if (!input.core->pending()) {
          if constexpr (requires { input.core->rebase(mapped); }) input.core->rebase(mapped);
          else input.core = std::make_unique<Core>(Core::from_snapshot(mapped));
        }
        // mapped owns the acknowledged graph before either old executor or its
        // construction lease is released. Keep the candidate's private jobs.
        core_ = std::move(input.core);
        core_scope_ = std::move(input.scope);
        current_ = std::move(mapped);
        catalog_failure_ = nullptr;
        return current_;
      } catch (...) { remember_failure(); poison(); throw; }
    }
    void remember_failure() noexcept {
      // A streamed Core has its own catalog connection. Keep its exact error
      // alive without copying the operation string while handling a failure.
      try { throw; }
      catch (catalog_error const &) { catalog_failure_ = std::current_exception(); }
      catch (...) { catalog_failure_ = nullptr; }
    }
    void require_active() const { if (failed()) throw std::logic_error("failed persistent Everett engine; reconnect it"); }
    void poison() noexcept {
      failed_ = true;
      if constexpr (requires { { core_->poison() } noexcept; }) core_->poison();
    }
  };

  // Ordinary callers use the mutable connection; snapshots retain the same
  // conditional update API as typed_world. Catalog operations below open their
  // own connection and never touch the worker's SQLite handle.
  template <class Core = active_engine<>> struct connection {
    using core_type = Core;
    using policy_type = typename Core::policy_type;
    using engine_type = persistent_engine<Core>;
    using session_type = session<engine_type>;
    using world_type = typename engine_type::world_type;
    using contribution_type = typename Core::contribution_type;
    using transaction_type = transaction<Core>;
    using ticket = typename session_type::ticket;
    using publication_type = typename session_type::snapshot_type;
    using store_type = runtime_store<policy_type, random_object_ids, sqlite_catalog_ops, typename Core::runtime_family>;

    connection(std::filesystem::path const & root, std::string_view name, connection_options options = {})
      : root_(std::filesystem::canonical(root)), options_(checked_options(std::move(options))),
        session_(std::make_shared<session_type>(engine_type::connect(root_, name, options_), *options_.limits)) {}
    connection(connection const &) = delete;
    connection & operator=(connection const &) = delete;
    connection(connection &&) = delete;
    connection & operator=(connection &&) = delete;

    ~connection() { session_->shutdown(); }
    transaction_type begin();
    world_type snapshot() const { return session_->snapshot()->world; }
    session_limits limits() const noexcept { return *options_.limits; }
    publication_type publication() const noexcept { return session_->snapshot(); }
    std::filesystem::path const & root() const & noexcept { return root_; }
    std::filesystem::path const & root() const && = delete;
    template <class S = typed_detail::default_sort_t<policy_type>> auto range(
        std::optional<typed_detail::key_t<S>> lo = {}, std::optional<typed_detail::key_t<S>> hi = {}) const {
      return everett::range<S>(snapshot(), std::move(lo), std::move(hi));
    }
    template <class S = typed_detail::default_sort_t<policy_type>> world_type erase_range(
        std::optional<typed_detail::key_t<S>> lo = {}, std::optional<typed_detail::key_t<S>> hi = {})
      requires typed_detail::replacement<S> {
      return apply(everett::erase_range<S>(snapshot(), std::move(lo), std::move(hi)));
    }
    template <class S = typed_detail::default_sort_t<policy_type>> ticket erase_range_async(
        std::optional<typed_detail::key_t<S>> lo = {}, std::optional<typed_detail::key_t<S>> hi = {})
      requires typed_detail::replacement<S> {
      return submit(everett::erase_range<S>(snapshot(), std::move(lo), std::move(hi)));
    }
    template <class S = typed_detail::default_sort_t<policy_type>> auto get(typed_detail::key_t<S> const & key) const {
      return session_->snapshot()->world.template get<S>(key);
    }
    template <class S = typed_detail::default_sort_t<policy_type>> world_type put(
        typed_detail::key_t<S> const & key, typed_detail::state_t<S> const & value)
      requires typed_detail::replacement<S> { return apply(Core::template put<S>(key, value)); }
    template <class S = typed_detail::default_sort_t<policy_type>> world_type erase(typed_detail::key_t<S> const & key)
      requires typed_detail::replacement<S> { return apply(Core::template erase<S>(key)); }
    template <class S = typed_detail::default_sort_t<policy_type>> world_type change(
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
    std::optional<ticket> try_submit(C && value) { return session_->try_submit(engine_type::contribution_type::borrow(std::forward<C>(value))); }
    template <class C> requires std::same_as<std::remove_cvref_t<C>, contribution_type>
    ticket submit(C && value) { return session_->submit(engine_type::contribution_type::borrow(std::forward<C>(value))); }
    template <class C> requires std::same_as<std::remove_cvref_t<C>, contribution_type>
    world_type apply(C && value) { return submit(std::forward<C>(value)).get()->world; }
    bool cancel(ticket const & value) { return session_->cancel(value); }
    session_reservation outstanding() const { return session_->outstanding(); }
    std::uint64_t pending_count() const { return session_->pending_count(); }
    std::exception_ptr failure() const { return session_->failure(); }
    void close() { session_->close(); }
    void shutdown() { session_->shutdown(); }

    void save(std::string_view name) const { save(name, snapshot()); }
    void save(std::string_view name, world_type const & state) const {
      auto storage = store_type::open(root_);
      storage.save(name, state.head());
    }
    std::optional<world_type> load(std::string_view name) const {
      auto storage = store_type::open(root_);
      auto saved = storage.find_save(name);
      if (!saved) return std::nullopt;
      auto world = connection_detail::restore_checkpoint<Core>(std::move(saved->snapshot), saved->semantic,
        session_->snapshot()->world.metadata().schema_id);
      return world_type(std::move(world), std::move(saved->head));
    }
    connection fork(std::string_view name) const { return fork(name, snapshot()); }
    connection fork(std::string_view name, world_type const & state) const {
      auto storage = store_type::open(root_);
      (void)storage.fork(name, state.head());
      auto options = options_;
      options.create_if_missing = false;
      options.schema_id = state.metadata().schema_id;
      return connection(root_, name, std::move(options));
    }
  private:
    static connection_options checked_options(connection_options options) {
      if (!options.limits) {
        std::uint64_t work = 128'000'000;
        if constexpr (requires { Core::reservation_work(std::uint64_t{}); }) work = Core::reservation_work(1024);
        options.limits = session_limits{work, 64 * 1024 * 1024, 64, 4096};
      }
      if (!options.limits->contributions || !options.limits->maintenance_budget)
        throw std::invalid_argument("connection needs a positive contribution limit and maintenance budget");
      return options;
    }
    std::filesystem::path root_;
    connection_options options_;
    std::shared_ptr<session_type> session_;
    friend transaction_type;
  };

  template <class Core = active_engine<>> connection<Core> connect(
      std::filesystem::path const & root, std::string_view name, connection_options options = {}) {
    return connection<Core>(root, name, std::move(options));
  }

  template <class P> std::size_t multiverse<P>::recover_transactions() const {
    return private_construction<P>::recover(root());
  }

  template <class P> auto multiverse<P>::connect(std::string_view name) const -> session {
    return session(root(), name);
  }
  template <class P> auto multiverse<P>::connect(std::string_view name, connection_options const & options) const -> session {
    return session(root(), name, options);
  }
}

#include <everett/transaction.h>
