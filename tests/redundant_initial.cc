/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks sorted initial frontiers, paid carries, snapshots and failed construction.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/redundant_runtime.h>

#include <iostream>
#include <string>

namespace {
  using namespace everett;
  void check(bool value, char const * message) { if (!value) throw std::runtime_error(message); }
  template <class E = std::exception, class F> void rejects(F && action) {
    bool caught = false;
    try { action(); } catch (E const &) { caught = true; }
    check(caught, "expected rejection");
  }
  bit_string number(unsigned value) {
    return bit_string::from_bytes(std::string{char(value >> 8), char(value)});
  }
  profile_record row(unsigned key, unsigned value) { return {number(key), number(value)}; }
  std::vector<profile_record> input(unsigned count) {
    std::vector<profile_record> result;
    for (unsigned i = 0; i != count; ++i) result.push_back(row(i, i ^ 0x5a5a));
    return result;
  }
  std::string bits(bit_view value) {
    std::string out;
    for (std::uint64_t i = 0; i != value.size(); ++i) out += value.at(i) ? '1' : '0';
    return out;
  }
  template <class Snapshot> auto values(Snapshot const & state, bit_view key) {
    auto cursor = state.cursor(key);
    std::vector<std::string> result;
    for (unsigned steps = 0; !cursor.done(); ++steps) {
      check(steps < 10000, "query stalled");
      if (!cursor.has_match()) cursor.step(1);
      if (cursor.has_match()) {
        auto match = cursor.take_match(); result.push_back(bits(match.value.view()));
      }
    }
    return result;
  }
  template <class Runtime> std::uint64_t allowance(std::uint64_t count) {
    // The existing typed_engine reservation with its default depth limit.
    return count * (2 * Runtime::local_charge_bound + 16 * 256 + 512 +
      Runtime::local_charge_bound * 8 * (Runtime::maximum_levels + 2));
  }
  template <class Runtime> void drain(Runtime & runtime) {
    for (unsigned calls = 0; runtime.pending(); ++calls) {
      check(calls < 1000000, "service stalled");
      runtime.advance(std::max<std::uint64_t>(runtime.next_service_cost(), 1));
    }
  }
  template <class Snapshot> void validate(Snapshot const & state) {
    auto restored = Snapshot::restore(state.frontier(), state.query_root().head());
    check(restored.admissions() == state.admissions(), "restored mass mismatch");
    std::uint64_t next = 0;
    for (auto const & run : state.runs()) {
      check(run->first == next && run->last > next, "chronology gap or reversal");
      next = run->last;
    }
    check(next == state.admissions(), "omitted history");
  }
  template <class P> void initial_sizes() {
    using runtime_type = redundant_runtime<P>;
    for (unsigned count = 2; count <= 4096; count <<= 1) {
      runtime_type runtime;
      auto empty = runtime.snapshot();
      auto records = input(count);
      auto before = runtime.work();
      auto budget = allowance<runtime_type>(count);
      check(runtime.try_initialize_sorted(records, budget, 256), "initial batch declined");
      check(runtime.admission_ready() && !runtime.pending() && !runtime.recovering() &&
        !runtime.service_due() && !runtime.credit(), "initialization left debt");
      auto initial = runtime.snapshot();
      validate(initial);
      check(initial.admissions() == count && runtime.work().admissions == count, "initial mass mismatch");
      auto work = runtime.work();
      check(work.charged - before.charged <= budget && work.granted - before.granted == budget, "initial allowance exceeded");
      check(work.charged == work.native_work + work.index_work + work.carrier_work + work.metadata_work + work.root_work,
        "structural charge sum mismatch");
      check(work.merges == static_cast<unsigned>(std::bit_width(count) - 1), "executed a nongeometric merge history");
      check(work.native_inputs == 2 * count - 2 && work.native_outputs == 3 * count - 2,
        "initialization retained singleton merge work");
      check(work.max_job_charge_per_mass <= runtime_type::local_charge_bound, "local job ceiling exceeded");
      for (auto const & record : records)
        check(values(initial, record.key.view()) == std::vector{bits(record.value.view())}, "initial value mismatch");
      check(values(empty, records.front().key.view()).empty() && !empty.admissions(), "empty snapshot mutated");

      // Restart the full hidden closure, then continue ordinary chronological
      // updates through several lower-index visibility transitions.
      auto resumed = runtime_type::from_snapshot(initial);
      check(!resumed.pending() && resumed.admission_ready(), "settled initialization restored recovery debt");
      for (unsigned i = 0; i != 40; ++i) {
        auto record = row(i % count, 0x8000 + i);
        resumed.contribute(record);
        auto found = values(resumed.snapshot(), record.key.view());
        check(!found.empty() && found.front() == bits(record.value.view()), "continued update chronology mismatch");
      }
      drain(resumed); validate(resumed.snapshot());
      check(resumed.snapshot().admissions() == count + 40, "continued admissions lost");
      for (unsigned i : {0u, count / 2, count - 1})
        check(values(initial, records[i].key.view()) == std::vector{bits(records[i].value.view())}, "initial snapshot mutated");
    }
  }
  template <class P> void refusals() {
    using runtime_type = redundant_runtime<P>;
    runtime_type runtime;
    auto original = runtime.snapshot();
    auto before = runtime.work();
    auto records = input(8);
    auto budget = allowance<runtime_type>(8);
    for (std::size_t n : {0u, 1u, 3u, 6u})
      check(!runtime.try_initialize_sorted(std::span(records).first(n), budget, 256), "accepted ineligible count");
    check(!runtime.try_initialize_sorted(records, 0, 256), "accepted unfunded initialization");
    check(!runtime.try_initialize_sorted(records, budget, 3), "accepted insufficient depth");
    check(runtime.snapshot().same_layout(original) && runtime.work().charged == before.charged &&
      runtime.work().granted == before.granted && !runtime.failed(), "refusal changed executor");
    auto bad = records;
    std::swap(bad[0], bad[1]);
    rejects<std::invalid_argument>([&] { runtime.try_initialize_sorted(bad, budget, 256); });
    bad = records; bad[1].key = bad[0].key;
    rejects<std::invalid_argument>([&] { runtime.try_initialize_sorted(bad, budget, 256); });
    if constexpr (P::bits_per_unit == 8) {
      bad = records; bad.back().value = bit_string::from_bits("1");
      rejects<std::invalid_argument>([&] { runtime.try_initialize_sorted(bad, budget, 256); });
    }
    check(!runtime.failed() && runtime.snapshot().same_layout(original) && runtime.work().charged == before.charged,
      "input preflight poisoned or charged executor");

    // An unrooted but previously allocated identity space is not a fresh seed.
    auto frontier = original.frontier(); frontier.next_identity = 2;
    auto old = runtime_type::snapshot_type::restore(std::move(frontier), original.query_root().head());
    auto history = runtime_type::from_snapshot(old);
    check(!history.try_initialize_sorted(records, budget, 256), "initialized historical empty frontier");
    check(runtime.try_initialize_sorted(records, budget, 4), "exact initial depth was rejected");
    auto initialized = runtime.snapshot(); before = runtime.work();
    check(!runtime.try_initialize_sorted(records, budget, 256) && runtime.snapshot().same_layout(initialized) &&
      runtime.work().charged == before.charged, "nonempty initializer changed executor");
    runtime_type pending;
    pending.try_contribute(records[0]); pending.try_contribute(records[1]);
    auto owed = pending.snapshot(); auto due = pending.service_due();
    check(!pending.try_initialize_sorted(records, budget, 256) && pending.snapshot().same_layout(owed) &&
      pending.service_due() == due, "initialization bypassed service debt");
    auto moved = std::move(pending);
    rejects<std::logic_error>([&] { pending.try_initialize_sorted(records, budget, 256); });
    check(moved.snapshot().same_layout(owed), "moved initializer changed transferred state");
  }
  template <class P> void partial_keys() {
    using runtime_type = redundant_runtime<P>;
    std::vector<profile_record> records;
    for (auto key : {"", "0", "00", "000", "001", "01", "1", "11"})
      records.push_back({bit_string::from_bits(key), bit_string::from_bits(std::string(key) + '1')});
    runtime_type runtime;
    check(runtime.try_initialize_sorted(records, allowance<runtime_type>(records.size()), 256), "partial-bit initialization declined");
    validate(runtime.snapshot());
    for (auto const & record : records)
      check(values(runtime.snapshot(), record.key.view()) == std::vector{bits(record.value.view())}, "partial-bit initial lookup");
  }
  struct append {
    bit_string operator()(bit_view, bit_view older, bit_view newer) const {
      return bit_string::from_bits(bits(older) + bits(newer));
    }
  };
  template <class P> void composition() {
    using runtime_type = redundant_runtime<P, append>;
    runtime_type runtime;
    auto records = input(64);
    check(runtime.try_initialize_sorted(records, allowance<runtime_type>(64), 256), "composing initial batch declined");
    std::vector<std::string> oracle;
    for (auto const & record : records) oracle.push_back(bits(record.value.view()));
    for (unsigned n = 0; n != 192; ++n) {
      auto index = n % 64;
      auto record = row(index, n);
      oracle[index] += bits(record.value.view()); runtime.contribute(record);
      auto found = values(runtime.snapshot(), record.key.view());
      std::string actual;
      for (auto i = found.rbegin(); i != found.rend(); ++i) actual += *i;
      check(actual == oracle[index], "noncommutative post-initialization chronology");
    }
    drain(runtime); validate(runtime.snapshot());
  }
  struct fault_state { unsigned calls = 0, fail = 0; bool poisoned = false; };
  template <class P> struct fault_storage : profile_runtime_storage<P> {
    using base = profile_runtime_storage<P>;
    using native_pointer = std::shared_ptr<typename base::native_type const>;
    std::shared_ptr<fault_state> state;
    void touch() {
      if (++state->calls == state->fail) throw std::runtime_error("injected construction failure");
    }
    void poison() noexcept { state->poisoned = true; }
    auto sorted_native(std::span<profile_record const> records) { touch(); return base::sorted_native(records); }
    template <class Node> auto make_index(native_pointer native, typename Node::pair_type main = {}, native_pointer secondary = {}) {
      touch(); return base::template make_index<Node>(std::move(native), std::move(main), std::move(secondary));
    }
    template <class Node> auto finish_index(typename base::template index_type<Node> & index) {
      touch(); return base::template finish_index<Node>(index);
    }
    template <class Compose> auto make_merge(native_pointer older, native_pointer newer, Compose compose) {
      touch(); return base::template make_merge<Compose>(std::move(older), std::move(newer), std::move(compose));
    }
    template <class Merge> auto finish_merge(Merge & merge) { touch(); return base::finish_merge(merge); }
  };
  template <class P> void failures() {
    using runtime_type = redundant_runtime<P, replace_native_value, fault_storage<P>>;
    auto records = input(8); auto budget = allowance<runtime_type>(8);
    unsigned failures = 0;
    for (unsigned cut = 1; cut != 100; ++cut) {
      auto state = std::make_shared<fault_state>(fault_state{0, cut, false});
      fault_storage<P> storage; storage.state = state;
      runtime_type runtime(storage); auto empty = runtime.snapshot();
      bool completed = false;
      try { completed = runtime.try_initialize_sorted(records, budget, 256); }
      catch (std::runtime_error const &) {
        ++failures;
        check(runtime.failed() && state->poisoned && runtime.snapshot().same_layout(empty), "failed initialization published");
        check(!runtime.snapshot().admissions() && values(empty, records[0].key.view()).empty(), "failed initializer changed empty snapshot");
        rejects<std::logic_error>([&] { runtime.try_initialize_sorted(records, budget, 256); });
        check(runtime.work().charged <= runtime.work().granted, "failed initializer overcharged");
      }
      if (completed) {
        check(!runtime.failed() && !state->poisoned && cut == state->calls + 1, "failure sweep omitted a factory event");
        break;
      }
    }
    check(failures >= 20, "failure sweep omitted construction or carry events");
  }
}

int main() {
  try {
    using bit = everett::storage_policy<everett::tip<everett::encoded_sort<everett::bit_encoding<>>>, 3, everett::golomb<3>, 5>;
    using byte = everett::storage_policy<everett::tip<everett::encoded_sort<everett::byte_encoding<>>>, 15, everett::exponential_golomb<0>, 4>;
    initial_sizes<bit>(); initial_sizes<byte>();
    refusals<bit>(); refusals<byte>(); composition<bit>(); composition<byte>();
    partial_keys<bit>(); failures<bit>(); failures<byte>();
    std::cout << "redundant initial construction tests passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
