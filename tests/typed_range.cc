/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks bounded multipass scans and observation-validated deletion sweeps.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/typed_scan.h>
#include <everett/replacement_rebuild.h>
#include <everett/sort_runtime.h>
#include <cassert>
#include <map>
#include <ranges>

namespace {
  using namespace everett;
  template <class F> void rejects(F && fn) {
    bool rejected = false;
    try { fn(); } catch (std::invalid_argument const &) { rejected = true; }
    assert(rejected);
  }
  template <class World> auto contents(World const & value) {
    std::map<std::string, std::string> result;
    for (auto row : range(value)) result.emplace(std::move(row.key), std::move(*row.value));
    return result;
  }
  template <class E> void verify() {
    E engine("range/1");
    std::map<std::string, std::string> expected;
    auto batch = E::batch();
    std::vector<std::string> keys{"", std::string("\0", 1), std::string("\0a", 2), "a", "aa", "ab", "abc", "b", "z", std::string("\xff", 1)};
    for (auto const & key : keys) { auto value = "value/" + key; batch.put(key, value); expected[key] = value; }
    engine.contribute(std::move(batch).finish());
    engine.contribute(E::put("aa", "newest")); expected["aa"] = "newest";
    engine.contribute(E::erase("ab")); expected.erase("ab");
    auto before = engine.snapshot();
    auto rows = range(before, std::string("a"), std::string("b"));
    static_assert(std::ranges::forward_range<decltype(rows)>);
    static_assert(std::ranges::viewable_range<decltype(rows)>);
    auto taken = range(before, std::string("a"), std::string("b")) | std::views::take(2);
    assert(std::ranges::distance(taken) == 2);
    auto filtered = range(before) | std::views::filter([](auto const & row) { return row.key == "aa"; });
    assert(std::ranges::distance(filtered) == 1);
    static_assert(std::forward_iterator<typename decltype(rows)::iterator>);
    assert(rows.step(0) == 0 && rows.consumed() == 0);
    auto first = rows.begin(), copy = first;
    assert(first == copy && first == rows.begin());
    auto retained = *first;
    assert(retained.key == "a");
    auto previous = first++;
    assert((*previous).key == "a" && (*first).key == "aa" && (*copy).key == "a");
    ++copy;
    assert(copy == first && retained.key == "a");
    ++copy;
    assert((*copy).key == "abc");
    assert(std::ranges::distance(rows) == 3);
    auto matched = std::ranges::find(rows, std::string("aa"), &decltype(rows)::row_type::key);
    assert(matched != rows.end() && (*matched).value == "newest");
    assert(range(before, std::string("a"), std::string("a")).begin() == std::default_sentinel);
    rejects([&] { (void)range(before, std::string("b"), std::string("a")); });
    auto prefix = range(before, std::nullopt, std::string("a"));
    assert(std::ranges::distance(prefix) == 3);
    auto suffix = range(before, std::string("z"));
    assert(std::ranges::distance(suffix) == 2);
    auto binary = range(before, std::string("\0", 1), std::string("\0b", 2));
    assert(std::ranges::distance(binary) == 2);
    auto removal = erase_range(before, std::string("a"), std::string("b"));
    assert(removal.records().size() == 3);
    engine.contribute(E::put("other", "independent")); expected["other"] = "independent";
    // A newly inserted key in the interval was never observed and survives.
    engine.contribute(E::put("ac", "unobserved")); expected["ac"] = "unobserved";
    engine.contribute(std::move(removal));
    expected.erase("a"); expected.erase("aa"); expected.erase("abc");
    assert(contents(engine.snapshot()) == expected && engine.snapshot().live_count() == expected.size());
    auto signature = std::uint64_t{0};
    for (auto const & [key, value] : expected) signature += sort_semantics<unsorted<std::optional<std::string>>>::hash_key(key) *
      sort_semantics<unsorted<std::optional<std::string>>>::hash_value(key, value);
    assert(engine.snapshot().signature() == signature);
    assert(contents(before).size() == 9 && (*previous).key == "a");
    auto stale = erase_range(engine.snapshot(), std::string("ac"), std::string("z"));
    engine.contribute(E::put("ac", "changed")); expected["ac"] = "changed";
    auto unchanged = engine.snapshot().metadata();
    rejects([&] { engine.contribute(std::move(stale)); });
    assert(!engine.failed() && engine.snapshot().metadata() == unchanged && contents(engine.snapshot()) == expected);
    auto empty = erase_range(engine.snapshot(), std::string("x"), std::string("y"));
    assert(empty.records().empty());
    engine.contribute(std::move(empty));
    auto all = erase_range(engine.snapshot());
    while (engine.pending()) engine.advance(1'000'000);
    engine.contribute(std::move(all));
    while (engine.pending()) engine.advance(1'000'000);
    assert(engine.snapshot().live_count() == 0 && engine.snapshot().signature() == 0 && contents(engine.snapshot()).empty());
  }
  // Hide the optimized query root to count every point query. Native scans
  // still expose ordinary immutable runs. Range selection/preflight/admission
  // must not invoke this fallback at all.
  template <class P> struct counted_family : binary_runtime_family<P> {
    using original = binary_runtime_family<P>;
    inline static unsigned queries = 0;
    struct snapshot_type {
      typename original::snapshot_type source;
      struct query_type {
        typename original::snapshot_type::query_type source;
        auto head() const { return source.head(); }
      };
      auto query_root() const { return query_type{source.query_root()}; }
      auto cursor_owned(bit_string && key) const { ++queries; return source.cursor_owned(std::move(key)); }
      auto runs() const { return source.runs(); }
      auto admissions() const { return source.admissions(); }
      bool same_layout(snapshot_type const & other) const { return source.same_layout(other.source); }
    };
    template <class Compose> struct runtime_type : original::template runtime_type<Compose> {
      using base = typename original::template runtime_type<Compose>;
      runtime_type() = default;
      static runtime_type from_snapshot(snapshot_type state) { return runtime_type(base::from_snapshot(std::move(state.source))); }
      snapshot_type snapshot() const { return {base::snapshot()}; }
      snapshot_type advance(std::uint64_t budget) { return {base::advance(budget)}; }
    private:
      explicit runtime_type(base value) : base(std::move(value)) {}
    };
  };
  void no_point_queries() {
    using family = counted_family<string_policy>;
    using engine_type = typed_engine<string_policy, wrapping_fingerprint_algebra, 256, family>;
    engine_type engine("counted-range/1");
    auto batch = engine_type::batch();
    for (unsigned i = 0; i != 48; ++i) batch.put("prefix/" + std::to_string(i), "v");
    engine.contribute(std::move(batch).finish());
    family::queries = 0;
    auto deletion = erase_range(engine.snapshot(), std::string("prefix/1"), std::string("prefix/4"));
    assert(family::queries == 0 && deletion.records().size() > 1);
    engine.contribute(std::move(deletion));
    assert(family::queries == 0);
  }
}
int main() {
  verify<everett::typed_engine<everett::string_policy>>();
  verify<everett::typed_engine<everett::storage_policy<>>>();
  verify<everett::typed_engine<everett::string_policy, everett::wrapping_fingerprint_algebra, 256,
    everett::sort_runtime_family<everett::string_policy>>>();
  verify<everett::replacement_rebuild_engine<everett::string_policy>>();
  verify<everett::replacement_rebuild_engine<everett::storage_policy<>>>();
  no_point_queries();
}
