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

namespace diet {
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
    static_assert(engine_type::charged_service, "replacement rebuild requires charged redundant service");
    using runtime_family = Family;
    using metadata_type = replacement_metadata<A>;
    using typed_cola_type = typename engine_type::cola_type;
    using cola_type = replacement_cola<P, A, Family>;
    using contribution_type = typename engine_type::contribution_type;
    using scan_type = typed_scan<sort_type, typed_cola_type>;
    static_assert(DepthLimit >= 3);
    static constexpr std::uint64_t small_limit = 64;

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
    replacement_rebuild_engine(replacement_rebuild_engine const &) = delete;
    replacement_rebuild_engine & operator=(replacement_rebuild_engine const &) = delete;
    replacement_rebuild_engine(replacement_rebuild_engine &&) noexcept = default;
    replacement_rebuild_engine & operator=(replacement_rebuild_engine &&) noexcept = default;

    cola_type snapshot() const { active(); return published_; }
    bool failed() const noexcept { return failed_; }
    bool pending() const noexcept { return foreground_ && (job_ || foreground_->pending()); }
    bool admission_ready() const noexcept { return foreground_ && !failed_ && !recovering_ && foreground_->admission_ready(); }
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
    static tap_reservation reservation(contribution_type const & input) {
      auto quote = engine_type::reservation(input);
      auto h = std::min<std::uint64_t>(64, DepthLimit - 3);
      auto g = action_bound(h), c = runtime_type::local_charge_bound;
      constexpr std::uint64_t runs = 128, scan = 32, setup = runs * (scan + 8) + 32;
      auto large = add(add(mul(22, g), mul(mul(10, add(c, 32)), add(h, 1))), setup + 26 * scan + 2);
      // Below the small threshold the valid generation has at most 256
      // physical occurrences. L <= 64 and bit_width(L) <= 7.
      auto small = add(setup + 321 * scan, add(mul(130, action_bound(7)), mul(512, add(c, 32))));
      auto query = mul(mul(64, add(DepthLimit, 1)), add(add(P::group_size, P::codec_block_size), 16));
      auto extra = add(add(std::max(large, small), query), runs * 8 + 32);
      quote.work = add(quote.work, mul(input.records().size(), extra));
      return quote;
    }

    // Validation and allocation of the input's detached replay records precede
    // mutation. Intermediate per-key cuts stay private until the entire batch
    // and its owed service succeeds.
    cola_type contribute(contribution_type input) {
      writable();
      if (!admission_ready()) throw std::logic_error("replacement foreground needs recovery service");
      if (input.base() && input.base()->metadata().schema_id != published_.metadata().schema_id)
        throw std::invalid_argument("rebuild contribution uses another schema");
      if (published_.runtime().query_root().head()->depth() > DepthLimit ||
          (input.base() && input.base()->runtime().query_root().head()->depth() > DepthLimit))
        throw std::length_error("replacement query exceeds depth allowance");
      std::vector<mutation> entries; entries.reserve(input.records().size());
      for (auto const & record : input.records()) {
        engine_type::key_transport::dispatch(record.key.view(), [&]<class S>(std::type_identity<S>, auto const & key) {
          static_assert(std::is_same_v<S, sort_type>);
          auto before = published_.template get<S>(key);
          if (input.base() && before != input.base()->template get<S>(key))
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
      } catch (...) { failed_ = true; throw; }
    }
    std::optional<cola_type> advance(std::uint64_t budget) {
      writable(); if (!budget || !pending()) return {};
      auto before = published_;
      try {
        if (job_ && foreground_->admission_ready()) { grant(budget); service(); }
        else foreground_advance(budget);
        published_ = publication();
      } catch (...) { failed_ = true; throw; }
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
      if (published_.runtime().query_root().head()->depth() > DepthLimit)
        throw std::length_error("restored replacement query exceeds depth allowance");
      work_.foreground_charged = foreground_->work().charged;
      if (published_.metadata().rebuilding) { recovering_ = true; start(true); }
    }
    cola_type publication() const { return {foreground_->snapshot(), base_, mutations_, bool(job_)}; }
    void active() const { if (!foreground_) throw std::logic_error("moved-from replacement rebuild engine"); }
    void writable() const { active(); if (failed_) throw std::logic_error("failed replacement rebuild engine"); }
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
      if (job_) {
        require(job_->admitted < job_->horizon, "rebuild deadline exhausted before admission");
        job_->queue.push_back(entry);
      }
      auto prior = foreground_->work().charged;
      try { foreground_->contribute(engine_type::template change<sort_type>(entry.key, entry.arrow)); }
      catch (...) { work_.foreground_charged = add(work_.foreground_charged, foreground_->work().charged - prior); throw; }
      work_.foreground_charged = add(work_.foreground_charged, foreground_->work().charged - prior);
      work_.mutations = entry.ordinal; mutations_ = add(mutations_, 1);
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
    void service() {
      while (job_) {
        auto cost = price(); if (job_->credit < cost) break;
        auto limit = add(job_->bound, mul(job_->admitted, job_->action));
        require(job_->committed <= limit && cost <= limit - job_->committed, "rebuild exceeded its reserved work bound");
        job_->credit -= cost; job_->committed += cost; work_.committed = add(work_.committed, cost);
        auto & j = *job_;
        if (!j.candidate) {
          j.candidate = std::make_unique<engine_type>(j.frozen.metadata().schema_id);
          work_.candidate_charged = add(work_.candidate_charged, j.candidate->work().charged);
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
