/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Executes charged encoded COLA carries behind immutable queryable snapshots.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <diet/cola_query.h>
#include <diet/mapped_cola.h>
#include <diet/native_merge.h>
#include <diet/runtime_seal.h>

#include <bit>
#include <functional>
#include <limits>
#include <stdexcept>
#include <memory>
#include <optional>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

namespace diet {
  template <class P> struct cola_runtime_native {
    using policy_type = P;
    static std::shared_ptr<cola_runtime_native const> from_owned(profile_array<P> value) {
      return std::shared_ptr<cola_runtime_native const>(new cola_runtime_native(
        std::make_shared<profile_array<P> const>(std::move(value))));
    }
    static std::shared_ptr<cola_runtime_native const> from_mapped(std::shared_ptr<mapped_native<P> const> value) {
      if (!value) error_detail::raise<std::invalid_argument>("null mapped runtime native");
      return std::shared_ptr<cola_runtime_native const>(new cola_runtime_native(std::move(value)));
    }
    profile_view<P> view() const { return owned_ ? owned_->view() : mapped_->view(); }
    std::uint64_t size() const { return view().size(); }
    std::shared_ptr<profile_array<P> const> owned() const noexcept { return owned_; }
    std::shared_ptr<mapped_native<P> const> mapped() const noexcept { return mapped_; }
  private:
    template <class, class, class, class> friend struct runtime_store;
    template <class, class, class, class> friend struct runtime_store_detail::graph_sealer;
    catalog_bindings<native_binding<mapped_native<P>>> bindings_;
    std::shared_ptr<profile_array<P> const> owned_;
    std::shared_ptr<mapped_native<P> const> mapped_;
    explicit cola_runtime_native(std::shared_ptr<profile_array<P> const> value) : owned_(std::move(value)) {}
    explicit cola_runtime_native(std::shared_ptr<mapped_native<P> const> value) : mapped_(std::move(value)) {}
  };

  // A facade over an existing mapped pair or new owning index. Wrapping a
  // mapped main chain reads metadata only; payload validation stays explicit.
  template <class P> struct cola_runtime_node {
    using policy_type = P;
    using native_type = cola_runtime_native<P>;
    using native_pointer = std::shared_ptr<native_type const>;
    using pair_type = std::shared_ptr<cola_runtime_node const>;
    using built_type = cola_index<P, native_type, cola_runtime_node>;
    static pair_type from_built(built_type value) {
      return pair_type(new cola_runtime_node(std::make_shared<built_type const>(std::move(value))));
    }
    static pair_type from_mapped(std::shared_ptr<mapped_cola_blob<P> const> head) {
      if (!head) error_detail::raise<std::invalid_argument>("null mapped runtime root");
      std::vector<std::shared_ptr<mapped_cola_blob<P> const>> nodes;
      std::unordered_set<mapped_cola_blob<P> const *> seen;
      for (auto p = std::move(head); p; p = p->main_target()) {
        if (!seen.insert(p.get()).second || p->secondary_target())
          error_detail::raise<std::invalid_argument>("runtime restore requires an acyclic main-only graph");
        nodes.push_back(p);
      }
      pair_type result;
      for (auto i = nodes.rbegin(); i != nodes.rend(); ++i)
        result = pair_type(new cola_runtime_node(*i, std::move(result)));
      return result;
    }
    static pair_type from_mapped_parts(std::shared_ptr<mapped_cola_blob<P> const> source,
        native_pointer native, pair_type main, native_pointer secondary = {}) {
      if (!source || !native || native->mapped() != source->native_object() || secondary || source->secondary_target() ||
          bool(main) != bool(source->main_target()) || (main && main->mapped() != source->main_target()))
        error_detail::raise<std::invalid_argument>("runtime mapped parts do not retain exact targets");
      return pair_type(new cola_runtime_node(std::move(source), std::move(native), std::move(main)));
    }
    cola_index_view<P> view() const { return built_ ? built_->view() : mapped_->view(); }
    native_pointer native_owner() const noexcept { return native_; }
    native_type const & native() const & noexcept { return *native_; }
    native_type const & native() const && = delete;
    pair_type main_target() const noexcept { return main_; }
    native_pointer secondary_target() const noexcept { return {}; }
    std::uint64_t virtual_size() const { return view().virtual_size(); }
    std::uint64_t group_count() const { return view().group_count(); }
    std::uint64_t depth() const noexcept { return depth_; }
    std::shared_ptr<built_type const> built() const noexcept { return built_; }
    std::shared_ptr<mapped_cola_blob<P> const> mapped() const noexcept { return mapped_; }
  private:
    template <class, class, class, class> friend struct runtime_store;
    template <class, class, class, class> friend struct runtime_store_detail::graph_sealer;
    catalog_bindings<pair_binding<mapped_cola_blob<P>>> bindings_;
    native_pointer native_;
    pair_type main_;
    std::shared_ptr<built_type const> built_;
    std::shared_ptr<mapped_cola_blob<P> const> mapped_;
    std::uint64_t depth_;
    explicit cola_runtime_node(std::shared_ptr<built_type const> value)
      : native_(value->native_owner()), main_(value->main_target()), built_(std::move(value)),
        depth_(profile_detail::add(main_ ? main_->depth() : 0, 1)) {
      if (built_->secondary_target()) error_detail::raise<std::invalid_argument>("runtime node has a secondary");
    }
    cola_runtime_node(std::shared_ptr<mapped_cola_blob<P> const> value, pair_type main)
      : native_(native_type::from_mapped(value->native_object())), main_(std::move(main)), mapped_(std::move(value)),
        depth_(profile_detail::add(main_ ? main_->depth() : 0, 1)) {}
    cola_runtime_node(std::shared_ptr<mapped_cola_blob<P> const> value, native_pointer native, pair_type main)
      : native_(std::move(native)), main_(std::move(main)), mapped_(std::move(value)),
        depth_(profile_detail::add(main_ ? main_->depth() : 0, 1)) {}
  };

  struct cola_runtime_interval {
    std::uint64_t first = 0, last = 0;
    std::uint64_t mass() const noexcept { return last - first; }
    bool operator==(cola_runtime_interval const &) const = default;
  };
  template <class P> struct cola_runtime_run {
    std::uint64_t first = 0, last = 0;
    std::shared_ptr<cola_runtime_node<P> const> node;
    std::uint64_t mass() const noexcept { return last - first; }
    auto native_owner() const noexcept { return node->native_owner(); }
  };
  template <class P, class Compose> struct cola_runtime;

  // All owners are immutable. Runs are oldest first, whereas cursor matches
  // are newest first. Intervals count admissions even when keys coalesce.
  template <class P> struct cola_runtime_snapshot {
    using policy_type = P;
    using node_type = cola_runtime_node<P>;
    using pair_type = typename node_type::pair_type;
    using query_type = cola_query_root<P, node_type>;
    using run_type = cola_runtime_run<P>;
    std::uint64_t admissions() const noexcept { return state_->admissions; }
    bool settled() const noexcept {
      auto const & r = state_->runs;
      return r.size() < 2 || r[r.size() - 2].mass() != r.back().mass();
    }
    std::span<run_type const> runs() const & noexcept { return state_->runs; }
    std::span<run_type const> runs() const && = delete;
    query_type const & query_root() const & noexcept { return state_->query; }
    query_type const & query_root() const && = delete;
    auto cursor(bit_view key) const { return state_->query.cursor(key); }
    bool same_layout(cola_runtime_snapshot const & other) const noexcept { return state_ == other.state_; }

    // Restore the frontier, not a mutable continuation. The supplied mapped or
    // built graph must already be trusted/admitted. Only metadata is inspected.
    // Empty routing nodes are omitted from the oldest-first interval list.
    static cola_runtime_snapshot restore(pair_type head, std::span<cola_runtime_interval const> intervals) {
      auto query = query_type::adopt_prepared(std::move(head));
      std::vector<pair_type> sources;
      for (auto p = query.head(); p; p = p->main_target())
        if (p->native_owner()->size()) sources.push_back(p);
      if (sources.size() != intervals.size())
        error_detail::raise<std::invalid_argument>("runtime interval count differs from query graph");
      std::vector<run_type> runs;
      std::uint64_t next = 0, previous = 0;
      for (std::size_t i = 0; i != intervals.size(); ++i) {
        auto span = intervals[i];
        if (span.first != next || span.last <= span.first || !std::has_single_bit(span.mass()) ||
            (i && span.mass() >= previous && !(i + 1 == intervals.size() && previous == 1 && span.mass() == 1)))
          error_detail::raise<std::invalid_argument>("invalid runtime admission frontier");
        auto source = sources[sources.size() - i - 1];
        if (source->native_owner()->size() > span.mass())
          error_detail::raise<std::invalid_argument>("runtime native exceeds admission mass");
        runs.push_back({span.first, span.last, std::move(source)});
        next = span.last; previous = span.mass();
      }
      return make(std::move(query), std::move(runs), next);
    }
  private:
    template <class, class> friend struct cola_runtime;
    struct state {
      query_type query;
      std::vector<run_type> runs;
      std::uint64_t admissions;
    };
    std::shared_ptr<state const> state_;
    explicit cola_runtime_snapshot(std::shared_ptr<state const> value) : state_(std::move(value)) {}
    static cola_runtime_snapshot make(query_type query, std::vector<run_type> runs, std::uint64_t admissions) {
      return cola_runtime_snapshot(std::make_shared<state const>(state{
        std::move(query), std::move(runs), admissions}));
    }
  };

  // Structural charges are allowances for executed/attempted operations, not
  // CPU cycles or a byte/latency bound. Counters also report completed records.
  struct cola_runtime_work {
    std::uint64_t charged = 0, granted = 0;
    std::uint64_t setup = 0, native_allowance = 0;
    std::uint64_t index_occurrences = 0, target_scan_allowance = 0, navigation_allowance = 0;
    std::uint64_t directory_entries = 0, publication = 0;
    std::uint64_t native_input_records = 0, native_output_records = 0;
    std::uint64_t native_merges = 0, indexes = 0, carriers = 0, admitted_records = 0;
  };

  // Low-level ACTIVE encoded executor. Compose is supplied by the active
  // registry handler; it is not the cola's semantic type. Existing FC records
  // are accepted here, not arbitrary per-sort physical grammars.
  //
  // An admission is immediately queryable. One private binary carry chain may
  // remain. The next admission drains that chain first, providing conservative
  // backpressure. Extra service changes only equivalent immutable layouts.
  // Credits pay real operations; finalization requires its whole structural
  // allowance and executes atomically, so single-call latency is not bounded.
  // Exceptions during execution poison the executor while retaining its last
  // published snapshot and private input pins. Invalid input preflight does not.
  template <class P, class Compose = replace_native_value> struct cola_runtime {
    using policy_type = P;
    using snapshot_type = cola_runtime_snapshot<P>;
    using node_type = cola_runtime_node<P>;
    using native_type = cola_runtime_native<P>;
    using native_pointer = typename node_type::native_pointer;
    using pair_type = typename node_type::pair_type;
    using run_type = cola_runtime_run<P>;
    using query_type = typename snapshot_type::query_type;
    explicit cola_runtime(Compose compose = {}) : execution_(std::make_unique<execution>(std::move(compose))) {}
    static cola_runtime from_snapshot(snapshot_type source, Compose compose = {}) {
      return cola_runtime(std::make_unique<execution>(std::move(source), std::move(compose)));
    }
    cola_runtime(cola_runtime const &) = delete;
    cola_runtime & operator=(cola_runtime const &) = delete;
    cola_runtime(cola_runtime &&) noexcept = default;
    cola_runtime & operator=(cola_runtime &&) noexcept = default;
    snapshot_type snapshot() const { return active().published; }
    bool pending() const noexcept { return execution_ && bool(execution_->job); }
    bool admission_ready() const noexcept { return execution_ && !execution_->failed && !execution_->job; }
    std::uint64_t next_service_cost() const { auto const & e = active(); return e.job ? e.price() : 0; }
    // Metadata-only structural cost of a ready singleton admission, excluding
    // optional service. This does not bound key bytes, callbacks or latency.
    std::uint64_t admission_cost() const {
      auto const & e = active();
      if (e.job) error_detail::raise<std::logic_error>("runtime admission requires service");
      auto r = e.published.runs();
      auto head = e.published.query_root().head();
      auto borrowed = head->group_count();
      auto n = execution::add(9, profile_detail::multiply(execution::add(borrowed, 1), P::group_size + 6));
      n = execution::add(n, execution::add(execution::blocks(borrowed), 5));
      n = execution::add(n, execution::add(execution::add(r.size(), head->depth()), 3));
      if (!r.empty() && r.back().mass() == 1) n = execution::add(n, execution::add(r.size(), 2));
      return n;
    }
    // Refusal is a no-op. Old debt is never silently charged as admission work.
    std::optional<snapshot_type> try_contribute(profile_record const & record, std::uint64_t service_budget = 0) {
      auto & e = writable();
      if (e.job) return std::nullopt;
      return contribute(record, service_budget);
    }
    bool failed() const noexcept { return execution_ && execution_->failed; }
    cola_runtime_work work() const { return active().work; }
    std::uint64_t credit() const { return active().credit; }
    snapshot_type advance(std::uint64_t budget) {
      auto & e = writable();
      if (!budget || !e.job) return e.published;
      try { e.grant(budget); e.serve(); }
      catch (...) { e.failed = true; throw; }
      return e.published;
    }
    snapshot_type contribute(profile_record const & record, std::uint64_t service_budget = 64) {
      return contribute(std::span<profile_record const>(&record, 1), service_budget);
    }
    // A batch is nondecreasing by encoded key; equal keys are chronological
    // separate admissions. Preflight precedes any work; a later execution
    // failure rolls the entire batch's visible state back and poisons this handle.
    snapshot_type contribute(std::span<profile_record const> records, std::uint64_t service_budget = 64) {
      auto & e = writable();
      (void)profile_detail::add(e.published.admissions(), records.size());
      bit_view previous;
      for (std::size_t i = 0; i != records.size(); ++i) {
        auto key = records[i].key.view(), value = records[i].value.view();
        if ((key.size() & (P::bits_per_unit - 1)) || (value.size() & (P::bits_per_unit - 1)) ||
            (P::value_width && (value.size() >> P::unit_shift) != *P::value_width) ||
            (i && compare_bits(previous, key) > 0))
          error_detail::raise<std::invalid_argument>("invalid encoded runtime contribution");
        previous = key;
      }
      auto before = e.published;
      try {
        for (auto const & record : records) {
          e.drain();
          e.admit(record);
          if (service_budget && e.job) { e.grant(service_budget); e.serve(); }
        }
      } catch (...) { e.published = std::move(before); e.failed = true; throw; }
      return e.published;
    }
  private:
    using merge_compose = std::conditional_t<std::is_same_v<Compose, replace_native_value>,
                                              Compose, std::reference_wrapper<Compose>>;
    using merge_type = native_merge_builder<P, native_type, merge_compose>;
    using index_type = cola_index_builder<P, native_type, node_type>;
    enum class phase { merge_start, merge_step, merge_finish, index_start, index_step, index_finish, publish };
    struct continuation {
      std::vector<run_type> prefix;
      run_type older;
      native_pointer carry;
      std::uint64_t first, last;
      pair_type target, head, result_node;
      bool carrier = false;
      phase stage = phase::merge_start;
      std::unique_ptr<merge_type> merge;
      std::unique_ptr<index_type> index;
      explicit continuation(snapshot_type const & source) : prefix(source.runs().begin(), source.runs().end()) {
        auto newest = prefix.back(); prefix.pop_back();
        carry = newest.native_owner(); first = newest.first; last = newest.last;
        older = prefix.back(); prefix.pop_back();
        target = older.node->main_target();
      }
    };
    struct execution {
      Compose compose;
      native_pointer empty;
      snapshot_type published;
      std::unique_ptr<continuation> job;
      cola_runtime_work work;
      std::uint64_t credit = 0;
      bool failed = false;
      static native_pointer empty_native() { return native_type::from_owned(profile_array<P>::build({})); }
      static snapshot_type initial(native_pointer native) {
        index_type builder(std::move(native));
        auto node = node_type::from_built(builder.finish());
        return snapshot_type::make(query_type::adopt_prepared(std::move(node)), {}, 0);
      }
      explicit execution(Compose value)
        : compose(std::move(value)), empty(empty_native()), published(initial(empty)) {
        direct_setup(2); direct_directory(3); direct_publication(2);
      }
      execution(snapshot_type source, Compose value)
        : compose(std::move(value)), empty(empty_native()), published(std::move(source)) {
        direct_setup(1); direct_directory(1);
        if (!published.settled()) start_job();
      }
      static std::uint64_t add(std::uint64_t a, std::uint64_t b) { return profile_detail::add(a, b); }
      static std::uint64_t blocks(std::uint64_t n) { return n / P::codec_block_size + (n % P::codec_block_size != 0); }
      void grant(std::uint64_t amount) {
        auto next = add(credit, amount), granted = add(work.granted, amount);
        credit = next; work.granted = granted;
      }
      void direct(std::uint64_t amount, std::uint64_t cola_runtime_work::* field) {
        auto next = work;
        next.charged = add(next.charged, amount); next.granted = add(next.granted, amount);
        next.*field = add(next.*field, amount); work = next;
      }
      void direct_setup(std::uint64_t n) { direct(n, &cola_runtime_work::setup); }
      void direct_directory(std::uint64_t n) { direct(n, &cola_runtime_work::directory_entries); }
      void direct_publication(std::uint64_t n) { direct(n, &cola_runtime_work::publication); }
      merge_compose merger() {
        if constexpr (std::is_same_v<Compose, replace_native_value>) return compose;
        else return std::ref(compose);
      }
      // Drop only empty routing ancestors. Never skip an actual native run.
      static pair_type near(pair_type target, std::uint64_t mass) {
        auto limit = mass > std::numeric_limits<std::uint64_t>::max() / P::group_size ?
          std::numeric_limits<std::uint64_t>::max() : mass * P::group_size;
        while (target && !target->native_owner()->size() && target->main_target() &&
               target->main_target()->virtual_size() <= limit) target = target->main_target();
        return target;
      }
      pair_type immediate_index(native_pointer native, pair_type target) {
        auto borrowed = target ? target->group_count() : 0;
        direct_setup(5);
        index_type builder(std::move(native), std::move(target));
        while (!builder.done()) {
          direct(1, &cola_runtime_work::index_occurrences);
          direct(P::group_size, &cola_runtime_work::target_scan_allowance);
          direct(5, &cola_runtime_work::navigation_allowance);
          builder.step(1);
        }
        direct_directory(add(blocks(borrowed), 2));
        direct(3, &cola_runtime_work::navigation_allowance);
        auto result = builder.finish();
        work.indexes = add(work.indexes, 1);
        return node_type::from_built(std::move(result));
      }
      void start_job() {
        direct_setup(add(published.runs().size(), 1));
        job = std::make_unique<continuation>(published);
      }
      void admit(profile_record const & record) {
        direct_setup(1);
        profile_native_writer<P> writer;
        direct(1, &cola_runtime_work::native_allowance);
        writer.append(record);
        direct_directory(2);
        auto native = native_type::from_owned(writer.finish());
        work.native_output_records = add(work.native_output_records, 1);
        auto head = immediate_index(native, published.query_root().head());
        auto runs = std::vector<run_type>(published.runs().begin(), published.runs().end());
        auto next = add(published.admissions(), 1);
        runs.push_back({published.admissions(), next, head});
        direct_publication(add(add(runs.size(), head->depth()), 1));
        published = snapshot_type::make(query_type::adopt_prepared(head), std::move(runs), next);
        work.admitted_records = add(work.admitted_records, 1);
        if (!published.settled()) start_job();
      }
      std::uint64_t price() const {
        auto const & j = *job;
        switch (j.stage) {
          case phase::merge_start: return 3;
          case phase::merge_step: return 3;
          case phase::merge_finish: return add(blocks(j.merge->progress().keys), 2);
          case phase::index_start: return 5;
          case phase::index_step: return add(P::group_size, 6);
          case phase::index_finish: return add(blocks(j.target ? j.target->group_count() : 0), 5);
          case phase::publish: return add(add(j.prefix.size(), j.head->depth()), 2);
        }
        error_detail::raise<std::logic_error>("invalid runtime stage");
      }
      void charge(std::uint64_t amount) {
        auto next = work;
        next.charged = add(next.charged, amount);
        switch (job->stage) {
          case phase::merge_start: case phase::index_start: next.setup = add(next.setup, amount); break;
          case phase::merge_step: next.native_allowance = add(next.native_allowance, amount); break;
          case phase::merge_finish: next.directory_entries = add(next.directory_entries, amount - 1);
            next.setup = add(next.setup, 1); break;
          case phase::index_step:
            next.index_occurrences = add(next.index_occurrences, 1);
            next.target_scan_allowance = add(next.target_scan_allowance, P::group_size);
            next.navigation_allowance = add(next.navigation_allowance, 5); break;
          case phase::index_finish: next.directory_entries = add(next.directory_entries, amount - 3);
            next.navigation_allowance = add(next.navigation_allowance, 3); break;
          case phase::publish: next.publication = add(next.publication, amount); break;
        }
        work = next; credit -= amount;
      }
      void perform() {
        auto & j = *job;
        switch (j.stage) {
          case phase::merge_start:
            j.merge = std::make_unique<merge_type>(j.older.native_owner(), j.carry, merger());
            j.stage = j.merge->done() ? phase::merge_finish : phase::merge_step;
            break;
          case phase::merge_step: {
            auto done = j.merge->step(1);
            work.native_input_records = add(work.native_input_records, done.input_records);
            work.native_output_records = add(work.native_output_records, done.keys);
            if (j.merge->done()) j.stage = phase::merge_finish;
            break;
          }
          case phase::merge_finish:
            j.carry = native_type::from_owned(j.merge->finish()); j.merge.reset();
            work.native_merges = add(work.native_merges, 1);
            j.first = j.older.first;
            if (!j.prefix.empty() && j.prefix.back().mass() == j.last - j.first) {
              j.older = j.prefix.back(); j.prefix.pop_back(); j.target = j.older.node->main_target();
              j.stage = phase::merge_start;
            } else {
              j.target = near(std::move(j.target), j.last - j.first);
              j.stage = phase::index_start;
            }
            break;
          case phase::index_start:
            j.index = std::make_unique<index_type>(j.carrier ? empty : j.carry, j.target);
            j.stage = j.index->done() ? phase::index_finish : phase::index_step;
            break;
          case phase::index_step:
            j.index->step(1);
            if (j.index->done()) j.stage = phase::index_finish;
            break;
          case phase::index_finish:
            j.head = node_type::from_built(j.index->finish()); j.index.reset();
            work.indexes = add(work.indexes, 1);
            if (j.carrier) work.carriers = add(work.carriers, 1);
            else j.result_node = j.head;
            if (j.head->virtual_size() > P::group_size) {
              j.target = j.head; j.carrier = true; j.stage = phase::index_start;
            } else j.stage = phase::publish;
            break;
          case phase::publish: {
            j.prefix.push_back({j.first, j.last, j.result_node});
            auto result = snapshot_type::make(query_type::adopt_prepared(j.head), std::move(j.prefix), j.last);
            published = std::move(result); job.reset(); credit = 0;
            break;
          }
        }
      }
      void serve() {
        while (job) {
          auto amount = price();
          if (amount > credit) break;
          charge(amount); perform();
        }
      }
      void drain() {
        while (job) {
          auto amount = price();
          if (credit < amount) grant(amount - credit);
          serve();
        }
      }
    };
    std::unique_ptr<execution> execution_;
    explicit cola_runtime(std::unique_ptr<execution> value) : execution_(std::move(value)) {}
    execution const & active() const {
      if (!execution_) error_detail::raise<std::logic_error>("moved-from COLA runtime");
      return *execution_;
    }
    execution & writable() {
      (void)active();
      if (execution_->failed) error_detail::raise<std::logic_error>("failed COLA runtime");
      return *execution_;
    }
  };

  template <class P> struct binary_runtime_family {
    using policy_type = P;
    using snapshot_type = cola_runtime_snapshot<P>;
    using node_type = cola_runtime_node<P>;
    using native_type = cola_runtime_native<P>;
    template <class Compose> using runtime_type = cola_runtime<P, Compose>;
  };
}
