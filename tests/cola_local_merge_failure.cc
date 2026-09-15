/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/cola_local_merge.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
  // Only the selected public operation is observed/faulted. Stop faulting at
  // the selected allocation so unwinding and later assertions run normally.
  thread_local bool watch = false;
  thread_local std::ptrdiff_t fail_at = -1;
  thread_local std::size_t allocations = 0;
  void * allocate(std::size_t size) {
    if (watch) {
      auto ordinal = allocations++;
      if (fail_at >= 0 && ordinal == static_cast<std::size_t>(fail_at)) {
        watch = false;
        throw std::bad_alloc();
      }
    }
    if (auto result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
  }
}

void * operator new(std::size_t size) { return allocate(size); }
void * operator new[](std::size_t size) { return allocate(size); }
void operator delete(void * pointer) noexcept { std::free(pointer); }
void operator delete[](void * pointer) noexcept { std::free(pointer); }
void operator delete(void * pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void * pointer, std::size_t) noexcept { std::free(pointer); }

namespace {
  using namespace everett;
  void check(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F && operation) {
    bool caught = false;
    try { operation(); } catch (std::logic_error const &) { caught = true; }
    check(caught, "failed local job accepted reuse");
  }
  struct observation { std::size_t count; bool failed; };
  template <class F> observation observe(std::ptrdiff_t failure, F && operation) {
    allocations = 0; fail_at = failure; watch = true;
    bool failed = false;
    try { operation(); }
    catch (std::bad_alloc const &) { failed = true; }
    catch (...) { watch = false; throw; }
    watch = false;
    return {allocations, failed};
  }
  template <class P> std::vector<profile_record> records(unsigned stride, unsigned offset) {
    std::vector<profile_record> out;
    for (unsigned i = 0; i != 128; ++i) {
      auto n = i * stride + offset;
      std::string text(129, 'p');
      text.push_back(char(n >> 8)); text.push_back(char(n));
      auto key = bit_string::from_bytes(text);
      if constexpr (P::unit == profile_unit::bit) {
        key.bytes.push_back(std::byte((n & 7) << 5));
        key.bit_size += 3;
      }
      std::string value((1 + i % 3) * P::bits_per_unit, '0');
      for (std::size_t at = 0; at != value.size(); ++at) if ((at + n) % 3) value[at] = '1';
      out.push_back({std::move(key), bit_string::from_bits(value)});
    }
    return out;
  }
  template <class P> std::uint64_t fingerprint(profile_array<P> const & input) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (auto byte : input.bytes()) hash = (hash ^ std::to_integer<unsigned>(byte)) * 0x100000001b3ull;
    return hash;
  }
  template <class P> struct fixture {
    using node = cola_index<P>;
    using native = profile_array<P>;
    using job = cola_local_merge_job<P>;
    std::unique_ptr<job> work;
    std::weak_ptr<native const> older, newer, main_native, secondary;
    std::weak_ptr<node const> main;
    std::uint64_t older_hash = 0, newer_hash = 0;
    bool secondary_plan;

    explicit fixture(bool side) : secondary_plan(side) {
      auto a = std::make_shared<native const>(native::build(records<P>(2, 0)));
      auto b = std::make_shared<native const>(native::build(records<P>(3, 0)));
      auto m = std::make_shared<node const>(node::build(records<P>(5, 1)));
      auto s = std::make_shared<native const>(native::build(records<P>(7, 2)));
      older = a; newer = b; main = m; main_native = m->native_owner(); secondary = s;
      older_hash = fingerprint(*a); newer_hash = fingerprint(*b);
      auto plan = side ? cola_destination_plan<P>::for_secondary(m)
        : cola_destination_plan<P>::for_main(m, s);
      work = std::make_unique<job>(std::move(a), std::move(b), std::move(plan));
      // On return the job is the only strong owner of these inputs/plan roots.
    }
    void pins() const {
      check(!older.expired() && !newer.expired() && !main.expired() && !main_native.expired(),
            "local job released source or plan pins");
      check(secondary_plan ? secondary.expired() : !secondary.expired(), "destination secondary pin scope");
      check(fingerprint(*older.lock()) == older_hash && fingerprint(*newer.lock()) == newer_hash,
            "faulted operation modified source encoding");
    }
    void released() const {
      check(older.expired() && newer.expired() && main.expired() && main_native.expired() && secondary.expired(),
            "destroyed failed job retained owners");
    }
  };
  template <class P> void reach(cola_local_merge_job<P> & work, cola_merge_phase phase) {
    unsigned stages = 0;
    while (work.phase() != phase) {
      check(!work.done() && ++stages <= 3, "requested phase not reachable");
      while (!work.stage_done()) check(work.step(31) != 0, "stage failed to progress");
      work.finish_stage();
    }
  }
  template <class P> void complete(cola_local_merge_job<P> & work) {
    while (!work.done()) {
      while (!work.stage_done()) check(work.step(31) != 0, "successful control stalled");
      work.finish_stage();
    }
    auto result = work.finish();
    check(result.merged_native && result.merged_native->size() > 128 && result.main && result.carrier,
          "successful allocation control lost its result");
    check(result.carrier->main_target() == result.main && result.carrier->secondary_target() == result.secondary,
          "successful control has wrong carrier targets");
  }
  template <class P> void poisoned(fixture<P> & input) {
    auto & work = *input.work;
    check(work.failed() && !work.done() && !work.stage_done() && !work.finished(),
          "allocation failure did not poison local job");
    input.pins();
    rejects([&] { (void)work.step(0); });
    rejects([&] { (void)work.step(1000); });
    rejects([&] { work.finish_stage(); });
    rejects([&] { (void)work.finish(); });
    check(work.failed() && !work.done() && !work.finished(), "rejected reuse changed failure state");
    input.pins();
    input.work.reset();
    input.released();
  }
  template <class P> void finalization(bool secondary, cola_merge_phase phase) {
    std::size_t count = 0;
    {
      fixture<P> input(secondary);
      reach(*input.work, phase);
      while (!input.work->stage_done()) check(input.work->step(31) != 0, "finalization fixture stalled");
      auto observed = observe(-1, [&] { input.work->finish_stage(); });
      check(!observed.failed && input.work->phase() != phase, "normal finalization failed");
      count = observed.count;
      check(count > 1, "finalization fixture needs early and late allocations");
      complete(*input.work);
    }
    // Two structural cuts, not a sweep of every implementation allocation:
    // the first allocation and a late transition/output-owner allocation.
    for (auto cut : {std::size_t{0}, count - 1}) {
      fixture<P> input(secondary);
      reach(*input.work, phase);
      while (!input.work->stage_done()) check(input.work->step(31) != 0, "finalization fixture stalled");
      auto observed = observe(static_cast<std::ptrdiff_t>(cut), [&] { input.work->finish_stage(); });
      check(observed.failed && observed.count == cut + 1, "finalization allocation cut not reached");
      poisoned(input);
    }
  }
  template <class P> void growth(bool secondary, cola_merge_phase phase) {
    std::size_t count = 0;
    {
      fixture<P> input(secondary);
      reach(*input.work, phase);
      check(input.work->step(1) == 1 && !input.work->stage_done(), "growth fixture needs accepted prefix");
      std::uint64_t consumed = 0;
      auto observed = observe(-1, [&] { consumed = input.work->step(64); });
      check(!observed.failed && consumed > 1 && consumed <= 64 && input.work->phase() == phase,
            "normal growth changed phase or budget");
      count = observed.count;
      check(count > 1, "growth fixture needs early and late allocations");
      complete(*input.work);
    }
    for (auto cut : {std::size_t{0}, count - 1}) {
      fixture<P> input(secondary);
      reach(*input.work, phase);
      check(input.work->step(1) == 1, "growth prefix not accepted");
      auto observed = observe(static_cast<std::ptrdiff_t>(cut), [&] { (void)input.work->step(64); });
      check(observed.failed && observed.count == cut + 1, "growth allocation cut not reached");
      poisoned(input);
    }
  }
  template <class P> void policy() {
    for (auto phase : {cola_merge_phase::native_merge, cola_merge_phase::destination_index,
                       cola_merge_phase::carrier_index}) finalization<P>(false, phase);
    for (auto phase : {cola_merge_phase::destination_index, cola_merge_phase::carrier_index}) growth<P>(false, phase);
    for (auto phase : {cola_merge_phase::native_merge, cola_merge_phase::carrier_index}) finalization<P>(true, phase);
    growth<P>(true, cola_merge_phase::carrier_index);
  }
}

int main() try {
  policy<storage_policy<profile_unit::byte, variable_values, 3, exponential_golomb<0>, 16>>();
  policy<storage_policy<profile_unit::bit, variable_values, 3, golomb<3>, 15>>();
  std::cout << "COLA local finalization and growth allocation failures passed\n";
} catch (std::exception const & error) {
  watch = false;
  std::cerr << error.what() << '\n';
  return 1;
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Faults local COLA finalization and index growth while checking poison and ownership.
 */
