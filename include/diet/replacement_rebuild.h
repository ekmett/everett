/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Rebuilds one replacement sort with a frozen scan and charged FIFO replay.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <diet/redundant_runtime.h>
#include <diet/typed_scan.h>
#include <deque>
#include <unordered_map>

namespace diet {
  namespace replacement_detail {
    template <class Family, class = void> struct clean_family {
      using type = Family;
      static constexpr bool enabled = false;
    };
    template <class Family> struct clean_family<Family, std::void_t<typename Family::storage_type::clean_storage_type>> {
      using type = typename Family::template rebind_storage<typename Family::storage_type::clean_storage_type>;
      static constexpr bool enabled = !std::is_same_v<Family, type>;
    };
  }
  // Trusted semantic checkpoint extension; the runtime frontier is stored
  // separately. Generation fields never authenticate the payload or its hash.
  template <class A = wrapping_fingerprint_algebra>
  struct replacement_metadata : typed_cola_metadata<A> {
    using base_type = typed_cola_metadata<A>;
    std::uint64_t clean_base = 0, mutations = 0;
    bool rebuilding = false;
    replacement_metadata() = default;
    replacement_metadata(base_type value, std::uint64_t b, std::uint64_t u, bool active)
      : base_type(std::move(value)), clean_base(b), mutations(u), rebuilding(active) {}
    void validate(std::uint64_t admissions) const {
      if (clean_base > admissions || mutations != admissions - clean_base ||
          this->live_count > admissions ||
          (clean_base > mutations && this->live_count < clean_base - mutations))
        throw std::invalid_argument("invalid replacement generation mass");
      auto trigger = clean_base / 4;
      if (!rebuilding) {
        if ((clean_base < 64 && (mutations || this->live_count != clean_base)) ||
            (clean_base >= 64 && mutations >= trigger))
          throw std::invalid_argument("inactive replacement generation passed its trigger");
      } else {
        // At freeze n_s <= b + floor(b/4); at most floor(n_s/8)
        // intervening admissions fit before handoff. Divide before adding to
        // avoid overflow in the upper bound for extreme metadata.
        auto extra = clean_base / 8 + trigger / 8 + (clean_base % 8 + trigger % 8) / 8;
        if (clean_base < 64 || mutations < trigger || mutations - trigger > extra)
          throw std::invalid_argument("invalid active replacement generation");
      }
    }
    std::vector<std::byte> encode() const requires std::same_as<typename A::element, std::uint64_t> {
      auto inner = base_type::encode();
      std::vector<std::byte> result(40 + inner.size());
      constexpr std::array<unsigned char, 8> magic{'D','I','E','T','.','R','B',0};
      for (unsigned i = 0; i != 8; ++i) result[i] = std::byte(magic[i]);
      std::array<std::uint64_t, 4> fields{1, clean_base, mutations, rebuilding ? 1u : 0u};
      for (unsigned n = 0; n != fields.size(); ++n)
        for (unsigned i = 0; i != 8; ++i) result[8 + n * 8 + i] = std::byte(fields[n] >> (i << 3));
      std::copy(inner.begin(), inner.end(), result.begin() + 40);
      return result;
    }
    static replacement_metadata decode(std::span<std::byte const> data)
      requires std::same_as<typename A::element, std::uint64_t> {
      constexpr std::array<unsigned char, 8> magic{'D','I','E','T','.','R','B',0};
      if (data.size() <= 56) throw std::invalid_argument("truncated replacement metadata");
      for (unsigned i = 0; i != 8; ++i)
        if (data[i] != std::byte(magic[i])) throw std::invalid_argument("replacement metadata signature mismatch");
      std::array<std::uint64_t, 4> fields{};
      for (unsigned n = 0; n != fields.size(); ++n)
        for (unsigned i = 0; i != 8; ++i)
          fields[n] |= std::uint64_t(std::to_integer<unsigned char>(data[8 + n * 8 + i])) << (i << 3);
      if (fields[0] != 1 || fields[3] > 1) throw std::invalid_argument("replacement metadata version or flags");
      return {base_type::decode(data.subspan(40)), fields[1], fields[2], fields[3] != 0};
    }
    bool operator==(replacement_metadata const &) const = default;
  };

  template <class P = string_policy, class A = wrapping_fingerprint_algebra,
    class Family = redundant_runtime_family<P>>
  struct replacement_cola : typed_cola<P, A, Family> {
    using base_type = typed_cola<P, A, Family>;
    using metadata_type = replacement_metadata<A>;
    using runtime_snapshot = typename Family::snapshot_type;
    replacement_cola(base_type value, std::uint64_t b = 0, std::uint64_t u = 0, bool active = false)
      : base_type(std::move(value)), metadata_(std::make_shared<metadata_type const>(base_type::metadata(), b, u, active)) {
      metadata_->validate(this->runtime().admissions());
    }
    metadata_type const & metadata() const & noexcept { return *metadata_; }
    static replacement_cola restore(runtime_snapshot data, metadata_type metadata, std::string_view schema) {
      metadata.validate(data.admissions());
      auto b = metadata.clean_base, u = metadata.mutations; auto active = metadata.rebuilding;
      return {base_type::restore(std::move(data), std::move(metadata), schema), b, u, active};
    }
  private:
    std::shared_ptr<metadata_type const> metadata_;
  };

  struct replacement_rebuild_work {
    std::uint64_t mutations = 0, generations = 0;
    std::uint64_t foreground_charged = 0, candidate_charged = 0;
    std::uint64_t reserved = 0, granted = 0, committed = 0;
    std::uint64_t scan_records = 0, clean_rows = 0, replayed = 0;
    std::uint64_t maximum_replay = 0, maximum_handoff_mutations = 0;
    std::uint64_t tiny_generations = 0, tiny_indexes = 0, tiny_conversion_charged = 0;
  };
  struct replacement_rebuild_status {
    std::uint64_t clean_base = 0, mutations = 0;
    std::uint64_t frozen_live = 0, horizon = 0, admitted = 0, replayed = 0;
    std::uint64_t initial_bound = 0, quantum = 0, action_bound = 0;
    std::uint64_t committed = 0, credit = 0, queued = 0;
    std::uint64_t source_records = 0, clean_rows = 0;
    bool rebuilding = false, scanning = false;
  };

  // This initial executor supports one occupied replacement sort. A custom
  // sort supplies clean(key,state)->arrow; the default optional-string sort
  // already represents a clean state with the same replacement arrow.
  // Budgets are structural allowances, not byte counts or elapsed time.
  template <class P = string_policy, class A = wrapping_fingerprint_algebra,
    std::uint64_t DepthLimit = 256, class Family = redundant_runtime_family<P>>
  struct replacement_rebuild_engine {
    using policy_type = P;
    using sort_type = typed_detail::default_sort_t<P>;
    static_assert(!std::is_void_v<sort_type>, "replacement rebuild needs one occupied sort");
    using semantics = sort_semantics<sort_type>;
    using key_type = typed_detail::key_t<sort_type>;
    using state_type = typed_detail::state_t<sort_type>;
    using arrow_type = typed_detail::arrow_t<sort_type>;
    static_assert(typed_detail::replacement<sort_type> && std::is_same_v<state_type, arrow_type>,
      "replacement rebuild requires replacement state/arrow types");
    using engine_type = typed_engine<P, A, DepthLimit, Family>;
    using runtime_type = typename engine_type::runtime_type;
    static constexpr bool charged_service = engine_type::charged_service;
    static_assert(charged_service, "replacement rebuild requires charged redundant service");
    using runtime_family = Family;
    using metadata_type = replacement_metadata<A>;
    using typed_cola_type = typename engine_type::cola_type;
    using cola_type = replacement_cola<P, A, Family>;
    using contribution_type = typename engine_type::contribution_type;
    using scan_type = typed_scan<sort_type, typed_cola_type>;
    static_assert(DepthLimit >= 3);
    static constexpr std::uint64_t small_limit = 64;
    static constexpr std::uint64_t tiny_record_limit = 256;

    replacement_rebuild_engine() : foreground_(std::make_unique<engine_type>()), published_(foreground_->snapshot()) { work_.foreground_charged = foreground_->work().charged; }
    explicit replacement_rebuild_engine(std::string schema)
      : foreground_(std::make_unique<engine_type>(std::move(schema))), published_(foreground_->snapshot()) { work_.foreground_charged = foreground_->work().charged; }
    static replacement_rebuild_engine from_clean(typed_cola_type source) {
      if (source.runtime().admissions() != source.metadata().live_count)
        throw std::invalid_argument("replacement rebuild restore needs a clean admission mass");
      auto b = source.metadata().live_count;
      return from_snapshot(cola_type(std::move(source), b));
    }
    static replacement_rebuild_engine from_snapshot(cola_type source) {
      return replacement_rebuild_engine(std::move(source));
    }
    template <class Storage>
    static replacement_rebuild_engine from_clean(typed_cola_type source, Storage storage)
      requires requires { engine_type::from_snapshot(source, std::move(storage)); }
    {
      if (source.runtime().admissions() != source.metadata().live_count)
        throw std::invalid_argument("replacement rebuild restore needs a clean admission mass");
      auto b = source.metadata().live_count;
      return from_snapshot(cola_type(std::move(source), b), std::move(storage));
    }
    template <class Storage>
    static replacement_rebuild_engine from_snapshot(cola_type source, Storage storage)
      requires requires { engine_type::from_snapshot(source, std::move(storage)); }
    {
      return replacement_rebuild_engine(std::move(source), std::move(storage));
    }
    replacement_rebuild_engine(replacement_rebuild_engine const &) = delete;
    replacement_rebuild_engine & operator=(replacement_rebuild_engine const &) = delete;
    replacement_rebuild_engine(replacement_rebuild_engine &&) noexcept = default;
    replacement_rebuild_engine & operator=(replacement_rebuild_engine &&) noexcept = default;

    cola_type snapshot() const { active(); return published_; }
    bool failed() const noexcept {
      return failed_ || (foreground_ && foreground_->failed()) ||
        (job_ && job_->candidate && job_->candidate->failed());
    }
    // A shared storage context is serialized with both private executors.
    // Poison it too when an outer durable publication becomes uncertain.
    void poison() noexcept {
      failed_ = true;
      if (foreground_) foreground_->poison();
      if (job_ && job_->candidate) job_->candidate->poison();
    }
    auto storage() const requires requires(engine_type const & core) { core.storage(); } {
      active();
      return foreground_->storage();
    }
    // Replace only a settled, equivalent physical graph. The concrete storage
    // context survives publication, including after a candidate handoff.
    void rebase(cola_type state) {
      writable();
      if (pending() || state.metadata() != published_.metadata() ||
          state.runtime().admissions() != published_.runtime().admissions())
        throw std::invalid_argument("replacement rebase requires a settled equivalent snapshot");
      try {
        foreground_->rebase(state);
        work_.foreground_charged = add(work_.foreground_charged, foreground_->work().charged);
        published_ = std::move(state);
      } catch (...) { poison(); throw; }
    }
    bool pending() const noexcept { return foreground_ && (job_ || foreground_->pending()); }
    bool admission_ready() const noexcept { return foreground_ && !failed() && !recovering_ && foreground_->admission_ready(); }
    replacement_rebuild_work work() const noexcept { return work_; }
    replacement_rebuild_status status() const {
      active(); replacement_rebuild_status out; out.clean_base = base_; out.mutations = mutations_;
      if (job_) {
        auto const & j = *job_; out.frozen_live = j.live; out.horizon = j.horizon;
        out.admitted = j.admitted; out.replayed = j.replayed; out.initial_bound = j.bound;
        out.quantum = j.quantum; out.action_bound = j.action; out.committed = j.committed;
        out.credit = j.credit; out.queued = j.queue.size(); out.source_records = j.source_records; out.clean_rows = j.rows; out.rebuilding = true; out.scanning = j.building;
      }
      return out;
    }
    static auto batch() { return engine_type::batch(); }
    template <class S = sort_type> requires std::same_as<S, sort_type>
    static contribution_type put(key_type const & key, state_type const & value) {
      return engine_type::template put<sort_type>(key, value);
    }
    template <class S = sort_type> requires std::same_as<S, sort_type>
    static contribution_type erase(key_type const & key) { return engine_type::template erase<sort_type>(key); }

    template <class S = sort_type> requires std::same_as<S, sort_type>
    static contribution_type change(key_type const & key, arrow_type const & arrow) {
      return engine_type::template change<S>(key, arrow);
    }
    // Accepted input allowance only. Recovery of already published history is
    // separately serviced behind admission_ready(), before a tap claims input.
    static std::uint64_t reservation_work(std::uint64_t records) {
      auto h = std::min<std::uint64_t>(64, DepthLimit - 3);
      auto g = action_bound(h), c = runtime_type::local_charge_bound;
      constexpr std::uint64_t runs = 128, scan = 32, setup = runs * (scan + 8) + 32;
      auto large = add(add(mul(22, g), mul(mul(10, add(c, 32)), add(h, 1))), setup + 26 * scan + 2);
      // Below the small threshold the valid generation has at most 256
      // physical occurrences. L <= 64 and bit_width(L) <= 7.
      auto small = add(setup + 321 * scan, add(mul(130, action_bound(7)), mul(512, add(c, 32))));
      auto query = mul(mul(64, add(DepthLimit, 1)), add(add(P::group_size, P::codec_block_size), 16));
      auto extra = add(add(std::max(large, small), query), runs * 8 + 32);
      if constexpr (replacement_detail::clean_family<Family>::enabled) extra = add(extra, tiny_conversion_bound());
      return add(engine_type::reservation_work(records), mul(records, extra));
    }
    static tap_reservation reservation(contribution_type const & input) {
      auto quote = engine_type::reservation(input);
      quote.work = reservation_work(input.records().size());
      return quote;
    }

    // Complete validation precedes mutation. The per-key fallback also prepares
    // detached replay records before execution. Intermediate cuts stay private
    // until the entire batch and its owed service succeeds.
    cola_type contribute(contribution_type input) {
      writable();
      if (!admission_ready()) throw std::logic_error("replacement foreground needs recovery service");
      if (input.base() && input.base()->metadata().schema_id != published_.metadata().schema_id)
        throw std::invalid_argument("rebuild contribution uses another schema");
      if (published_.runtime().query_root().head()->depth() > DepthLimit ||
          (input.base() && input.base()->runtime().query_root().head()->depth() > DepthLimit))
        throw std::length_error("replacement query exceeds depth allowance");
      if (input.records().empty()) return published_;
      // Initial unique string replacements are already clean arrows. Share
      // the typed preflight and avoid constructing detached per-key replay
      // entries when the runtime can initialize a prefix and admit the tail
      // before publishing the complete pristine batch.
      if constexpr (std::is_same_v<sort_type, unsorted<std::optional<std::string>>> &&
          requires(runtime_type & runtime, std::span<profile_record const> records) {
            runtime.try_initialize_sorted(records, std::uint64_t{}, DepthLimit);
          }) {
        auto count = input.records().size();
        if (count >= 2 && !base_ && !mutations_ &&
            !job_ && !recovering_ && !foreground_->pending() && !mass(published_) &&
            !work_.mutations && !work_.generations) {
          auto metadata = foreground_->prepare(input);
          auto accepted = add(work_.mutations, count);
          auto prior = foreground_->work().charged;
          try {
            if (foreground_->initialize(input, metadata)) {
              work_.foreground_charged = add(work_.foreground_charged, foreground_->work().charged - prior);
              prior = foreground_->work().charged;
              base_ = count;
              work_.mutations = accepted;
              published_ = publication();
              return published_;
            }
          } catch (...) {
            work_.foreground_charged = add(work_.foreground_charged, foreground_->work().charged - prior);
            poison();
            throw;
          }
        }
      }
      std::vector<mutation> entries; entries.reserve(input.records().size());
      for (auto const & record : input.records()) {
        engine_type::key_transport::dispatch(record.key.view(), [&]<class S>(std::type_identity<S>, auto const & key) {
          static_assert(std::is_same_v<S, sort_type>);
          auto before = published_.template get_encoded<S>(key, record.key);
          if (input.base() && before != input.base()->template get_encoded<S>(key, record.key))
            throw std::invalid_argument("stale rebuilt key value");
          auto arrow = typed_detail::value<P, S>(record.value.view());
          auto after = semantics::apply(key, before, arrow);
          if (!semantics::present(key, before) && !semantics::present(key, after))
            throw std::invalid_argument("deleting absent rebuilt key");
          entries.push_back({key, std::move(before), std::move(after), std::move(arrow), 0});
        });
      }
      try {
        for (auto & entry : entries) apply(std::move(entry));
        published_ = publication();
        return published_;
      } catch (...) { poison(); throw; }
    }
    std::optional<cola_type> advance(std::uint64_t budget) {
      writable(); if (!budget || !pending()) return {};
      auto before = published_;
      try {
        if (job_ && foreground_->admission_ready()) { grant(budget); service(); }
        else foreground_advance(budget);
        published_ = publication();
      } catch (...) { poison(); throw; }
      if (before.runtime().same_layout(published_.runtime()) && before.metadata() == published_.metadata()) return {};
      return published_;
    }

  private:
    struct mutation {
      key_type key;
      state_type before, after;
      arrow_type arrow;
      std::uint64_t ordinal;
    };
    struct rebuild {
      typed_cola_type frozen;
      std::unique_ptr<scan_type> scan;
      std::unique_ptr<engine_type> candidate;
      std::deque<mutation> queue;
      std::uint64_t live = 0, horizon = 1, frozen_ordinal = 0;
      std::uint64_t admitted = 0, replayed = 0, rows = 0, source_records = 0;
      std::uint64_t bound = 0, quantum = 0, action = 0, scan_price = 0, setup = 0;
      std::uint64_t committed = 0, credit = 0;
      bool building = true;
      bool tiny = false;
      explicit rebuild(typed_cola_type value) : frozen(std::move(value)) {}
    };
    std::unique_ptr<engine_type> foreground_;
    cola_type published_;
    std::unique_ptr<rebuild> job_;
    std::uint64_t base_ = 0, mutations_ = 0;
    replacement_rebuild_work work_;
    bool failed_ = false, recovering_ = false;

    explicit replacement_rebuild_engine(cola_type value)
      : foreground_(std::make_unique<engine_type>(engine_type::from_snapshot(value))), published_(std::move(value)),
        base_(published_.metadata().clean_base), mutations_(published_.metadata().mutations) {
      initialize_restore();
    }
    template <class Storage>
    replacement_rebuild_engine(cola_type value, Storage storage)
      : foreground_(std::make_unique<engine_type>(engine_type::from_snapshot(value, std::move(storage)))),
        published_(std::move(value)), base_(published_.metadata().clean_base),
        mutations_(published_.metadata().mutations) {
      initialize_restore();
    }
    void initialize_restore() {
      if (published_.runtime().query_root().head()->depth() > DepthLimit)
        throw std::length_error("restored replacement query exceeds depth allowance");
      work_.foreground_charged = foreground_->work().charged;
      if (published_.metadata().rebuilding) { recovering_ = true; start(true); }
    }
    cola_type publication() const { return {foreground_->snapshot(), base_, mutations_, bool(job_)}; }
    void active() const { if (!foreground_) throw std::logic_error("moved-from replacement rebuild engine"); }
    void writable() const { active(); if (failed()) throw std::logic_error("failed replacement rebuild engine"); }
    static std::uint64_t mass(typed_cola_type const & state) { return state.runtime().admissions(); }
    static std::uint64_t add(std::uint64_t a, std::uint64_t b) { return profile_detail::add(a, b); }
    static std::uint64_t mul(std::uint64_t a, std::uint64_t b) { return profile_detail::multiply(a, b); }
    static std::uint64_t ceil(std::uint64_t a, std::uint64_t b) { return a / b + (a % b != 0); }
    static void require(bool value, char const * message) { if (!value) throw std::logic_error(message); }
    static arrow_type clean(key_type const & key, state_type const & state) {
      if constexpr (requires { semantics::clean(key, state); }) return semantics::clean(key, state);
      else {
        static_assert(std::is_same_v<sort_type, unsorted<std::optional<std::string>>>,
          "custom replacement sort must define clean(key,state)");
        return state;
      }
    }
    void foreground_advance(std::uint64_t budget) {
      auto prior = foreground_->work().charged;
      try { foreground_->advance(budget); }
      catch (...) { work_.foreground_charged = add(work_.foreground_charged, foreground_->work().charged - prior); throw; }
      work_.foreground_charged = add(work_.foreground_charged, foreground_->work().charged - prior);
    }
    void apply(mutation entry) {
      require(foreground_->admission_ready(), "replacement foreground exhausted its admission service");
      entry.ordinal = add(work_.mutations, 1);
      // A fresh replacement in a clean string table adds one live row and
      // one physical admission, without introducing history to collect.
      // Extend that clean base; pending ordinary carries still receive their
      // normal admission service. Once history exists, every write funds it.
      bool extends_clean = false;
      if constexpr (std::is_same_v<sort_type, unsorted<std::optional<std::string>>>)
        extends_clean = !job_ && !mutations_ && !entry.before && bool(entry.after);
      if (job_) {
        require(job_->admitted < job_->horizon, "rebuild deadline exhausted before admission");
        job_->queue.push_back(entry);
      }
      auto prior = foreground_->work().charged;
      try { foreground_->contribute(engine_type::template change<sort_type>(entry.key, entry.arrow)); }
      catch (...) { work_.foreground_charged = add(work_.foreground_charged, foreground_->work().charged - prior); throw; }
      work_.foreground_charged = add(work_.foreground_charged, foreground_->work().charged - prior);
      work_.mutations = entry.ordinal;
      if (extends_clean) {
        base_ = add(base_, 1);
        require(mass(foreground_->snapshot()) == base_, "extended clean generation mass mismatch");
        return;
      }
      mutations_ = add(mutations_, 1);
      require(mass(foreground_->snapshot()) == add(base_, mutations_), "foreground generation mass mismatch");
      if (job_) {
        ++job_->admitted;
        work_.reserved = add(work_.reserved, job_->action);
        work_.maximum_replay = std::max<std::uint64_t>(work_.maximum_replay, job_->queue.size());
        grant(add(job_->quantum, job_->action)); service();
        require(!job_ || job_->admitted < job_->horizon, "rebuild missed funded handoff horizon");
      } else {
        auto live = foreground_->snapshot().metadata().live_count;
        bool small = base_ < small_limit || live < small_limit;
        if (small || mutations_ >= base_ / 4) {
          start(small);
          if (small) { grant(job_->bound); service(); require(!job_, "small rebuild did not finish"); }
        }
      }
    }
    static std::uint64_t action_bound(std::uint64_t height) {
      auto depth = add(height, 3), c = runtime_type::local_charge_bound;
      auto ready = add(add(mul(2, c), mul(16, depth)), 512);
      auto query = mul(mul(64, add(depth, 1)), add(add(P::group_size, P::codec_block_size), 16));
      auto service = mul(mul(8, c), add(height, 2));
      return add(add(ready, service), add(query, add(mul(8, height), 16)));
    }
    void start(bool small) {
      auto source = foreground_->snapshot();
      auto freeze = add(mul(source.runtime().runs().size(), 8), 32);
      work_.reserved = add(work_.reserved, freeze);
      work_.granted = add(work_.granted, freeze);
      work_.committed = add(work_.committed, freeze);
      auto next = std::make_unique<rebuild>(std::move(source));
      next->live = next->frozen.metadata().live_count;
      next->horizon = small ? 1 : next->live / 8;
      require(next->horizon, "empty rebuilding horizon");
      next->frozen_ordinal = work_.mutations;
      auto limit = add(next->live, small ? 0 : next->horizon);
      auto height = std::bit_width(limit); auto depth = add(height, 3);
      if (depth > DepthLimit) throw std::length_error("rebuild candidate exceeds supported depth");
      auto c = runtime_type::local_charge_bound;
      next->action = action_bound(height);
      std::uint64_t physical = 0;
      auto runs = next->frozen.runtime().runs();
      for (auto const & run : runs) physical = add(physical, run->native->size());
      next->source_records = physical;
      if constexpr (replacement_detail::clean_family<Family>::enabled)
        next->tiny = small && next->live <= small_limit && physical <= tiny_record_limit;
      next->scan_price = add(mul(2, std::bit_width(runs.size())), 16);
      next->setup = add(mul(runs.size(), add(next->scan_price, 8)), 32);
      auto r = add(next->setup, mul(add(add(physical, next->live), 1), next->scan_price));
      r = add(r, mul(next->live, next->action));
      // All maintenance through the largest possible replay cut is reserved,
      // including a large carry first triggered by an intervening mutation.
      r = add(r, mul(mul(add(c, 32), limit), add(height, 1)));
      r = add(r, mul(limit, next->action)); // Nested grant fragments at settlement.
      r = add(r, mul(next->horizon, next->action)); // Outer atomic-action fragments.
      next->bound = add(r, next->action); // Final metadata check and ownership handoff.
      if (next->tiny) next->bound = add(next->bound, tiny_conversion_bound());
      next->quantum = ceil(next->bound, next->horizon);
      work_.reserved = add(work_.reserved, next->bound);
      job_ = std::move(next);
    }
    void grant(std::uint64_t amount) {
      auto credit = add(job_->credit, amount), total = add(work_.granted, amount);
      job_->credit = credit; work_.granted = total;
    }
    std::uint64_t price() const {
      auto const & j = *job_;
      if (j.tiny) return j.bound;
      if (!j.candidate) return j.setup;
      if (j.building && !j.scan->has_row() && !j.scan->done()) return j.scan_price;
      return j.action;
    }
    void candidate_work(auto && operation) {
      auto before = job_->candidate->work().charged;
      try { operation(); }
      catch (...) {
        work_.candidate_charged = add(work_.candidate_charged, job_->candidate->work().charged - before);
        throw;
      }
      work_.candidate_charged = add(work_.candidate_charged, job_->candidate->work().charged - before);
    }
    // A settled candidate with at most 64 admissions has at most seven levels.
    // Checked restore confines every object route to the three slots per
    // level. Their pairs, at most one carrier per level, and the prepared
    // head chain use fewer than 8*(height+1) distinct pairs. Each pair has at most 2*64+4 occurrences:
    // V <= 64 + ceil(V/K) + ceil(64/K), with K >= 3. Charge an index allowance
    // including both directory streams, plus object/level admission metadata.
    static std::uint64_t tiny_conversion_bound() {
      auto pairs = mul(8, add(std::bit_width(small_limit), 1));
      return add(mul(pairs, add(32, mul(add(mul(2, small_limit), 4), add(P::group_size, 8)))), 1024);
    }
    template <class Source> auto convert_tiny(Source const & source) {
      using source_pair = typename Source::query_type::pair_type;
      using source_node = std::remove_const_t<typename source_pair::element_type>;
      using source_object = typename Source::object_pointer::element_type;
      using target_node = typename Family::node_type;
      using target_pair = typename target_node::pair_type;
      using target_object = typename Family::snapshot_type::object_pointer;
      static_assert(std::is_same_v<typename source_node::native_type, typename Family::native_type>);
      auto const & old = source.frontier();
      require(old.admissions <= small_limit && old.levels.size() <= std::bit_width(small_limit) && !old.service_due,
        "tiny conversion exceeds its bounded settled frontier");
      std::uint64_t charged = 0;
      auto charge = [&](std::uint64_t amount) {
        charged = add(charged, amount);
        require(charged <= tiny_conversion_bound(), "tiny conversion exceeded its allowance");
        work_.tiny_conversion_charged = add(work_.tiny_conversion_charged, amount);
        work_.candidate_charged = add(work_.candidate_charged, amount);
      };
      charge(add(32, mul(old.levels.size(), 16)));
      std::unordered_map<source_node const *, target_pair> pairs;
      auto copy_pair = [&](auto && self, source_pair const & value) -> target_pair {
        if (!value) return {};
        if (auto found = pairs.find(value.get()); found != pairs.end()) return found->second;
        auto main = self(self, value->main_target());
        require(pairs.size() < 8 * (std::bit_width(small_limit) + 1), "too many tiny conversion pairs");
        auto secondary = value->secondary_target();
        auto a = main ? ceil(main->virtual_size(), P::group_size) : 0;
        auto b = secondary ? ceil(secondary->size(), P::group_size) : 0;
        auto n = add(value->native_owner()->size(), add(a, b));
        require(n <= 2 * small_limit + 4, "tiny pair exceeds its occurrence allowance");
        charge(add(add(16, mul(n, P::group_size + 6)), add(ceil(a, P::codec_block_size), ceil(b, P::codec_block_size))));
        cola_index_builder<P, typename Family::native_type, target_node> builder(value->native_owner(), main, secondary);
        while (!builder.done()) builder.step(1);
        auto result = target_node::from_built(builder.finish());
        work_.tiny_indexes = add(work_.tiny_indexes, 1);
        pairs.emplace(value.get(), result); return result;
      };
      std::unordered_map<source_object const *, target_object> objects;
      auto copy_object = [&](auto && self, typename Source::object_pointer const & value) -> target_object {
        if (!value) return {};
        if (auto found = objects.find(value.get()); found != objects.end()) return found->second;
        typename Family::routes_type next{self(self, value->next.main), self(self, value->next.secondary)};
        require(objects.size() < 3 * old.levels.size(), "too many tiny conversion objects");
        charge(16);
        auto result = std::make_shared<typename Family::object_type const>(typename Family::object_type{
          value->identity, value->first, value->last, value->level, value->native,
          copy_pair(copy_pair, value->pair), std::move(next)});
        objects.emplace(value.get(), result); return result;
      };
      auto copy_route = [&](auto const & route) {
        return typename Family::routes_type{copy_object(copy_object, route.main), copy_object(copy_object, route.secondary)};
      };
      typename Family::frontier_type out;
      out.admissions = old.admissions; out.next_identity = old.next_identity; out.service_due = old.service_due;
      out.levels.resize(old.levels.size());
      for (std::size_t i = 0; i != old.levels.size(); ++i) {
        auto const & before = old.levels[i]; auto & after = out.levels[i];
        require(!before.job, "tiny conversion requires completed jobs");
        unsigned active = 0;
        for (std::size_t j = 0; j != before.slots.size(); ++j) {
          auto const & slot = before.slots[j]; active += slot.state == redundant_slot_state::active;
          after.slots[j] = {slot.state, copy_object(copy_object, slot.object), copy_route(slot.route),
            copy_pair(copy_pair, slot.carrier), slot.ever_visible};
        }
        require(active < 2, "tiny conversion requires a settled level");
        after.last_destination = before.last_destination; after.last_destination_visible = before.last_destination_visible;
      }
      out.root = copy_route(old.root);
      auto head = copy_pair(copy_pair, source.query_root().head());
      auto result = Family::snapshot_type::restore(std::move(out), std::move(head));
      return result;
    }
    void tiny_rebuild() requires replacement_detail::clean_family<Family>::enabled {
      using clean_family = typename replacement_detail::clean_family<Family>::type;
      using clean_engine = typed_engine<P, A, DepthLimit, clean_family>;
      auto & j = *job_;
      require(j.tiny && j.live <= small_limit && j.source_records <= tiny_record_limit &&
        j.queue.empty() && !j.admitted, "tiny rebuild escaped its bounded eager path");
      clean_engine candidate(j.frozen.metadata().schema_id);
      work_.candidate_charged = add(work_.candidate_charged, candidate.work().charged);
      auto execute = [&](auto && operation) {
        auto before = candidate.work().charged;
        try { operation(); }
        catch (...) { work_.candidate_charged = add(work_.candidate_charged, candidate.work().charged - before); throw; }
        work_.candidate_charged = add(work_.candidate_charged, candidate.work().charged - before);
      };
      scan_type rows(j.frozen);
      while (!rows.done()) {
        if (!rows.has_row()) work_.scan_records = add(work_.scan_records, rows.step(1));
        if (!rows.has_row()) continue;
        auto row = rows.take_row();
        auto arrow = clean(row.key, row.value);
        require(semantics::apply(row.key, semantics::initial(row.key), arrow) == row.value, "invalid clean replacement arrow");
        while (!candidate.admission_ready()) execute([&] { candidate.advance(j.action); });
        execute([&] { candidate.contribute(clean_engine::template change<sort_type>(row.key, arrow)); });
        ++j.rows; work_.clean_rows = add(work_.clean_rows, 1);
        require(j.rows <= j.live, "tiny scan exceeds frozen live count");
      }
      auto scanned = candidate.snapshot();
      require(rows.consumed() == j.source_records && j.rows == j.live &&
        scanned.runtime().admissions() == j.live && scanned.metadata() == j.frozen.metadata(),
        "tiny rebuild differs from frozen source");
      while (candidate.pending()) execute([&] { candidate.advance(j.action); });
      auto clean = candidate.snapshot();
      auto runtime = convert_tiny(clean.runtime());
      auto state = typed_cola_type::restore(std::move(runtime), clean.metadata(), j.frozen.metadata().schema_id);
      require(state.metadata() == foreground_->snapshot().metadata() && state.runtime().admissions() == j.live,
        "tiny handoff differs from foreground");
      auto replacement = engine_type::from_snapshot(std::move(state), foreground_->storage());
      work_.candidate_charged = add(work_.candidate_charged, replacement.work().charged);
      base_ = j.live; mutations_ = 0;
      work_.generations = add(work_.generations, 1); work_.tiny_generations = add(work_.tiny_generations, 1);
      foreground_ = std::make_unique<engine_type>(std::move(replacement)); job_.reset(); recovering_ = false;
    }
    void service() {
      while (job_) {
        auto cost = price(); if (job_->credit < cost) break;
        auto limit = add(job_->bound, mul(job_->admitted, job_->action));
        require(job_->committed <= limit && cost <= limit - job_->committed, "rebuild exceeded its reserved work bound");
        job_->credit -= cost; job_->committed += cost; work_.committed = add(work_.committed, cost);
        auto & j = *job_;
        if constexpr (replacement_detail::clean_family<Family>::enabled) {
          if (j.tiny) { tiny_rebuild(); continue; }
        }
        if (!j.candidate) {
          auto seed = std::make_unique<engine_type>(j.frozen.metadata().schema_id);
          work_.candidate_charged = add(work_.candidate_charged, seed->work().charged);
          if constexpr (requires { foreground_->storage(); }) {
            // The empty seed does not execute native work. Its initialization
            // and context-aware restoration both fit the setup's 32-unit margin.
            j.candidate = std::make_unique<engine_type>(
              engine_type::from_snapshot(seed->snapshot(), foreground_->storage()));
            work_.candidate_charged = add(work_.candidate_charged, j.candidate->work().charged);
          } else j.candidate = std::move(seed);
          j.scan = std::make_unique<scan_type>(j.frozen);
        } else if (j.building) {
          if (j.scan->has_row()) {
            if (!j.candidate->admission_ready()) candidate_work([&]{ j.candidate->advance(j.action); });
            else {
              auto row = j.scan->take_row();
              auto arrow = clean(row.key, row.value);
              require(semantics::apply(row.key, semantics::initial(row.key), arrow) == row.value, "invalid clean replacement arrow");
              candidate_work([&]{ j.candidate->contribute(engine_type::template change<sort_type>(row.key, arrow)); });
              ++j.rows; work_.clean_rows = add(work_.clean_rows, 1);
            }
          } else if (!j.scan->done()) work_.scan_records = add(work_.scan_records, j.scan->step(1));
          else {
            require(j.scan->consumed() == j.source_records && j.rows == j.live && mass(j.candidate->snapshot()) == j.live &&
              j.candidate->snapshot().metadata() == j.frozen.metadata(), "clean rebuild differs from frozen source");
            j.scan.reset(); j.building = false;
          }
        } else if (!j.queue.empty()) {
          if (!j.candidate->admission_ready()) candidate_work([&]{ j.candidate->advance(j.action); });
          else {
            auto const & entry = j.queue.front();
            require(entry.ordinal == add(add(j.frozen_ordinal, j.replayed), 1), "rebuild replay order changed");
            require(j.candidate->snapshot().template get<sort_type>(entry.key) == entry.before, "rebuild replay old value mismatch");
            candidate_work([&]{ j.candidate->contribute(engine_type::template change<sort_type>(entry.key, entry.arrow)); });
            require(j.candidate->snapshot().template get<sort_type>(entry.key) == entry.after, "rebuild replay new value mismatch");
            j.queue.pop_front(); ++j.replayed; work_.replayed = add(work_.replayed, 1);
          }
        } else if (j.candidate->pending()) candidate_work([&]{ j.candidate->advance(j.action); });
        else {
          auto state = j.candidate->snapshot();
          require(j.replayed == j.admitted && state.runtime().admissions() == add(j.live, j.replayed) &&
            state.metadata() == foreground_->snapshot().metadata(), "rebuild handoff differs from foreground");
          base_ = j.live; mutations_ = j.replayed;
          work_.maximum_handoff_mutations = std::max(work_.maximum_handoff_mutations, j.admitted);
          work_.generations = add(work_.generations, 1);
          foreground_ = std::move(j.candidate); job_.reset(); recovering_ = false;
        }
      }
    }
  };
}
