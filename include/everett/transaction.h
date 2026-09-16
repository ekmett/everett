/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Private typed nurseries, persistent read snapshots and checked durable commit.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <everett/connection.h>
#include <everett/nursery_map.h>

namespace everett {
  namespace transaction_detail {
    template <class S> inline constexpr bool state_is_arrow =
      std::same_as<S, unsorted<std::optional<std::string>>>;
    struct key_less {
      bool operator()(bit_string const & a, bit_string const & b) const { return compare_bits(a.view(), b.view()) < 0; }
    };
  }

  // A transaction is single-owner mutable state. Frozen snapshots and branches
  // retain immutable nursery roots, staged files and the original commit base.
  template <class Core> struct transaction {
    using policy_type = typename Core::policy_type;
    using family_type = typename Core::runtime_family;
    using transport = typed_detail::transport_t<policy_type, family_type>;
    using world_type = typename Core::world_type;
    using connection_type = connection<Core>;
    using engine_type = typename connection_type::engine_type;
    using session_type = typename connection_type::session_type;
    using stored_type = typename connection_type::world_type;
    using ticket = typename connection_type::ticket;
    struct nursery_value {
      bit_string arrow;
      std::optional<bit_string> original;
    };
    using nursery_type = nursery_map<bit_string, nursery_value, transaction_detail::key_less>;
    using scope_type = private_construction<policy_type>;
    using store_type = typename connection_type::store_type;

  private:
    struct origin {
      std::filesystem::path root;
      std::weak_ptr<session_type> session;
      stored_type base;
      std::uint64_t maintenance_budget;
    };
    struct stage {
      std::shared_ptr<scope_type> scope;
      world_type world;
      bool changed;
      stage(world_type state, std::shared_ptr<scope_type> lease, bool modified)
        : scope(std::move(lease)), world(std::move(state)), changed(modified) {}
    };

  public:
    struct snapshot_type {
      template <class S = typed_detail::default_sort_t<policy_type>> auto get(typed_detail::key_t<S> const & key) const {
        auto encoded = transport::template encode<S>(key);
        auto entry = nursery_.find(encoded);
        if (!entry) return stage_->world.template get<S>(key);
        auto before = [&] {
          if constexpr (typed_detail::replacement<S>) return sort_semantics<S>::initial(key);
          else return stage_->world.template get<S>(key);
        }();
        return sort_semantics<S>::apply(key, std::move(before), typed_detail::value<policy_type, S>(entry->arrow.view()));
      }
      std::size_t buffered_keys() const noexcept { return nursery_.size(); }
      transaction branch() const { return transaction(*this); }
    private:
      std::shared_ptr<origin const> origin_;
      std::shared_ptr<stage const> stage_;
      typename nursery_type::snapshot_type nursery_;
      snapshot_type(std::shared_ptr<origin const> source, std::shared_ptr<stage const> base,
          typename nursery_type::snapshot_type values)
        : origin_(std::move(source)), stage_(std::move(base)), nursery_(std::move(values)) {}
      friend transaction;
    };

    transaction(transaction const &) = delete;
    transaction & operator=(transaction const &) = delete;
    transaction(transaction &&) = default;
    transaction & operator=(transaction &&) = delete;

    snapshot_type snapshot() {
      require_active();
      return {origin_, stage_, nursery_->freeze()};
    }
    template <class S = typed_detail::default_sort_t<policy_type>> auto get(typed_detail::key_t<S> const & key) const {
      require_active();
      auto encoded = transport::template encode<S>(key);
      auto entry = nursery_->find(encoded);
      if (!entry) return stage_->world.template get<S>(key);
      auto before = [&] {
        if constexpr (typed_detail::replacement<S>) return sort_semantics<S>::initial(key);
        else return stage_->world.template get<S>(key);
      }();
      return sort_semantics<S>::apply(key, std::move(before), typed_detail::value<policy_type, S>(entry->arrow.view()));
    }
    template <class S = typed_detail::default_sort_t<policy_type>> void change(
        typed_detail::key_t<S> const & key, typed_detail::arrow_t<S> const & arrow) {
      require_active();
      using semantics = sort_semantics<S>;
      auto encoded = transport::template encode<S>(key);
      auto previous = nursery_->find(encoded);
      auto base = [&] {
        if constexpr (transaction_detail::state_is_arrow<S>) {
          if (previous) return semantics::apply(key, semantics::initial(key),
            typed_detail::value<policy_type, S>(previous->original->view()));
        }
        return stage_->world.template get<S>(key);
      }();
      auto before = previous ? semantics::apply(key, base,
        typed_detail::value<policy_type, S>(previous->arrow.view())) : base;
      auto after = semantics::apply(key, before, arrow);
      if constexpr (typed_detail::replacement<S>) {
        if (!semantics::present(key, before) && !semantics::present(key, after))
          throw std::invalid_argument("transaction deletion targets an absent key");
      }
      // Encoding and composition finish before the mutable nursery is touched.
      auto combined = [&] {
        if constexpr (typed_detail::replacement<S>) return arrow;
        else if (previous) return semantics::compose(key,
          typed_detail::value<policy_type, S>(previous->arrow.view()), arrow);
        else return arrow;
      }();
      auto value = typed_detail::value<policy_type, S>(combined);
      if constexpr (typed_detail::replacement<S>) {
        if (after == base) { nursery_->erase(encoded); return; }
      }
      std::optional<bit_string> original;
      if constexpr (transaction_detail::state_is_arrow<S>) original.emplace(typed_detail::value<policy_type, S>(base));
      nursery_->insert_or_assign(std::move(encoded), nursery_value{std::move(value), std::move(original)});
    }
    template <class S = typed_detail::default_sort_t<policy_type>> void put(
        typed_detail::key_t<S> const & key, typed_detail::state_t<S> const & value)
      requires typed_detail::replacement<S> { change<S>(key, value); }
    template <class S = typed_detail::default_sort_t<policy_type>> void erase(typed_detail::key_t<S> const & key)
      requires typed_detail::replacement<S> { change<S>(key, sort_semantics<S>::erase(key)); }

    // Empty flushes allocate no Core, catalog scope or output file. A nonempty
    // flush completes typed validation and seals a private checkpoint only.
    snapshot_type flush() {
      require_active();
      if (nursery_->empty()) return snapshot();
      auto batch = Core::batch();
      nursery_->for_each([&](bit_string const & key, nursery_value const & value) {
        transport::dispatch(key.view(), [&]<class S>(std::type_identity<S>, auto const & decoded) {
          batch.template change<S>(decoded, typed_detail::value<policy_type, S>(value.arrow.view()));
        });
      });
      auto input = std::move(batch).finish();
      nursery_type empty;
      try {
        ensure_core();
        while (!core_->admission_ready()) (void)core_->advance(origin_->maintenance_budget);
        auto updated = core_->contribute(std::move(input));
        store_->prepare(updated.runtime());
        auto next = std::make_shared<stage const>(stage{std::move(updated), scope_, true});
        // Preserve the old stage and frozen roots if any preceding operation
        // fails; after this point replacement of both local roots is noexcept.
        *nursery_ = std::move(empty);
        stage_ = std::move(next);
      } catch (...) { failed_ = true; throw; }
      return snapshot();
    }
    ticket commit_async() {
      require_active();
      auto target = origin_->session.lock();
      if (!target) throw session_closed();
      (void)flush();
      if (stage_->changed && !core_) ensure_core(); // branch of a flushed snapshot
      typename engine_type::prepared_transaction prepared{std::move(core_), origin_->base, scope_};
      typename engine_type::contribution_type command(std::move(prepared));
      // Submission owns the candidate from here; every failure leaves the
      // original named world unchanged locally, with uncertain SQL outcomes
      // reported through the same ticket contract as ordinary contributions.
      closed_ = true;
      auto result = target->submit(std::move(command));
      release();
      return result;
    }
    stored_type commit() { return commit_async().get()->world; }
    void abort() noexcept { closed_ = true; release(); }
    bool failed() const noexcept { return failed_ || (nursery_ && nursery_->failed()); }

  private:
    std::shared_ptr<origin const> origin_;
    std::shared_ptr<stage const> stage_;
    std::optional<nursery_type> nursery_{std::in_place};
    // Destruction order keeps the scope alive until both SQLite connections
    // and every private builder have been released.
    std::shared_ptr<scope_type> scope_;
    std::unique_ptr<store_type> store_;
    std::unique_ptr<Core> core_;
    bool closed_ = false, failed_ = false;

    explicit transaction(connection_type & connection)
      : origin_(std::make_shared<origin const>(origin{connection.root_, connection.session_,
          connection.snapshot(), connection.options_.limits->maintenance_budget})),
        stage_(std::make_shared<stage const>(stage{origin_->base, {}, false})) {}
    explicit transaction(snapshot_type const & saved)
      : origin_(saved.origin_), stage_(saved.stage_), nursery_(nursery_type::thaw(saved.nursery_)), scope_(stage_->scope) {}
    void ensure_core() {
      if (core_) return;
      if (!scope_) scope_ = scope_type::create(origin_->root);
      if (!store_) store_ = std::make_unique<store_type>(store_type::open(origin_->root, {}, scope_->options()));
      auto const & schema = stage_->world.metadata().schema_id;
      if constexpr (requires { family_type::open_storage(origin_->root, schema, scope_->options()); })
        core_ = std::make_unique<Core>(Core::from_snapshot(stage_->world,
          family_type::open_storage(origin_->root, schema, scope_->options())));
      else {
        static_assert(!requires { family_type::open_storage(origin_->root); },
          "streaming transaction storage must accept private catalog options");
        core_ = std::make_unique<Core>(Core::from_snapshot(stage_->world));
      }
    }
    void require_active() const {
      if (closed_ || !origin_) throw std::logic_error("closed Everett transaction");
      if (failed()) throw std::logic_error("failed Everett transaction; abort it");
    }
    void release() noexcept {
      core_.reset(); store_.reset(); scope_.reset(); stage_.reset(); origin_.reset();
      nursery_.reset();
    }
    friend connection_type;
  };

  template <class Core> auto connection<Core>::begin() -> transaction_type { return transaction_type(*this); }
}
