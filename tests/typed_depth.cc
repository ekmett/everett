/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks typed root-depth limits at execution and imported publication boundaries.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/redundant_runtime.h>
#include <everett/typed_world.h>

#include <cassert>

namespace {
  using namespace everett;
  using P = string_policy;
  template <class Family, std::uint64_t Limit>
  using engine = typed_engine<P, wrapping_fingerprint_algebra, Limit, Family>;

  template <class F> void over_limit(F && action) {
    bool rejected = false;
    try { action(); } catch (std::length_error const &) { rejected = true; }
    assert(rejected);
  }
  template <class Family> void admission_boundary(bool two) {
    using E = engine<Family, 1>;
    E active;
    auto old = active.snapshot();
    auto before = active.work().charged;
    auto input = E::batch(); input.put("a", "one");
    if (two) input.put("b", "two");
    over_limit([&] { active.contribute(std::move(input).finish()); });
    auto after = active.snapshot();
    assert(active.failed() && active.work().charged > before);
    assert(after.runtime().same_layout(old.runtime()) && after.metadata() == old.metadata());
    assert(!old.get("a") && !old.get("b"));
    bool refused = false;
    try { active.contribute(E::put("retry", "forbidden")); }
    catch (std::logic_error const &) { refused = true; }
    assert(refused);
  }

  template <class Family> auto padded(typename Family::snapshot_type const & source) {
    using Node = typename Family::node_type;
    using Native = typename Family::native_type;
    using Snapshot = typename Family::snapshot_type;
    auto empty = Native::from_owned(profile_array<P>::build({}));
    cola_index_builder<P, Native, Node> builder(empty, source.query_root().head());
    while (!builder.done()) builder.step(64);
    auto head = Node::from_built(builder.finish());
    if constexpr (requires { source.frontier(); }) return Snapshot::restore(source.frontier(), head);
    else {
      std::vector<cola_runtime_interval> intervals;
      for (auto const & run : source.runs()) intervals.push_back({run.first, run.last});
      return Snapshot::restore(head, intervals);
    }
  }

  template <class Family, std::uint64_t Limit> void imported_boundary() {
    using E = engine<Family, Limit>;
    E active;
    auto old = active.contribute(E::put("a", "one"));
    assert(!active.pending() && old.runtime().query_root().head()->depth() == Limit);
    auto deeper = padded<Family>(old.runtime());
    assert(deeper.query_root().head()->depth() == Limit + 1);
    auto imported = E::world_type::restore(deeper, old.metadata(), old.metadata().schema_id);
    assert(imported.get("a") == "one");
    over_limit([&] { (void)E::from_snapshot(imported); });
    if constexpr (requires { active.storage(); })
      over_limit([&] { (void)E::from_snapshot(imported, active.storage()); });
    auto before = active.work().charged;
    over_limit([&] { active.rebase(imported); });
    auto retained = active.snapshot();
    assert(!active.failed() && active.work().charged == before &&
      retained.runtime().same_layout(old.runtime()) && retained.get("a") == "one");
    auto restored = E::from_snapshot(old);
    assert(restored.snapshot().get("a") == "one");
  }

  // An equivalent backend publication may add a valid empty routing parent.
  // Current carries usually preserve or reduce depth; the typed Family
  // contract must also reject a backend that legitimately grows its root.
  template <class Compose> struct expanding_runtime : cola_runtime<P, Compose> {
    using base_type = cola_runtime<P, Compose>;
    using snapshot_type = typename base_type::snapshot_type;
    using base_type::base_type;
    static expanding_runtime from_snapshot(snapshot_type state, Compose compose = {}) {
      return expanding_runtime(base_type::from_snapshot(std::move(state), std::move(compose)));
    }
    bool try_initialize_sorted(std::span<profile_record const> records, std::uint64_t, std::uint64_t) {
      base_type::contribute(records, 0);
      return true; // Deliberately leaves depth enforcement to the typed boundary.
    }
    bool pending() const noexcept { return base_type::pending(); }
    bool failed() const noexcept { return poisoned || base_type::failed(); }
    void poison() noexcept { poisoned = true; }
    snapshot_type advance(std::uint64_t budget) {
      if (!budget) return this->snapshot();
      return padded<binary_runtime_family<P>>(this->snapshot());
    }
  private:
    bool poisoned = false;
    explicit expanding_runtime(base_type source) : base_type(std::move(source)) {}
  };
  struct expanding_family : binary_runtime_family<P> {
    template <class Compose> using runtime_type = expanding_runtime<Compose>;
  };
  void advance_boundary() {
    engine<binary_runtime_family<P>, 2> source;
    auto old = source.contribute(decltype(source)::put("a", "one"));
    using E = engine<expanding_family, 2>;
    auto admitted = E::world_type::restore(old.runtime(), old.metadata(), old.metadata().schema_id);
    auto active = E::from_snapshot(admitted);
    assert(!active.advance(0));
    over_limit([&] { active.advance(1); });
    auto retained = active.snapshot();
    assert(active.failed() && retained.runtime().same_layout(admitted.runtime()) &&
      retained.metadata() == admitted.metadata() && retained.get("a") == "one");
  }

  void initialization_boundary() {
    using E = engine<expanding_family, 2>;
    E active;
    auto old = active.snapshot();
    auto input = E::batch(); input.put("a", "one").put("b", "two");
    over_limit([&] { active.contribute(std::move(input).finish()); });
    auto retained = active.snapshot();
    assert(active.failed() && retained.runtime().same_layout(old.runtime()) && !retained.live_count());
  }

  void retained_nonempty() {
    using E = engine<redundant_runtime_family<P>, 1>;
    E active;
    auto first = active.contribute(E::put("a", "one"));
    over_limit([&] { active.contribute(E::put("b", "two")); });
    auto retained = active.snapshot();
    assert(active.failed() && retained.runtime().same_layout(first.runtime()));
    assert(retained.get("a") == "one" && !retained.get("b"));
  }
}
int main() {
  admission_boundary<binary_runtime_family<P>>(false);
  admission_boundary<redundant_runtime_family<P>>(true);
  imported_boundary<binary_runtime_family<P>, 2>();
  imported_boundary<redundant_runtime_family<P>, 1>();
  retained_nonempty(); advance_boundary(); initialization_boundary();
}
