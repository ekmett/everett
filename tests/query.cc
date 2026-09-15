/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/query.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
  using namespace everett;

  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F && action) {
    bool rejected = false;
    try { action(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "invalid query operation accepted");
  }

  // The oracle compares logical bits independently of the optimized key/LCP
  // primitives and never uses a fractional index to choose an expected record.
  int order(bit_view a, bit_view b) {
    for (std::uint64_t i = 0; i < std::min(a.size(), b.size()); ++i)
      if (a.at(i) != b.at(i)) return a.at(i) ? 1 : -1;
    return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
  }
  bool equal(bit_view a, bit_view b) { return order(a, b) == 0; }
  bit_string text(std::string const & value) { return bit_string::from_bytes(value); }
  void append_bit(bit_string & value, bool bit) {
    if (value.bit_size % 8 == 0) value.bytes.push_back(std::byte{0});
    if (bit) value.bytes.back() |= std::byte(1u << (7 - value.bit_size % 8));
    ++value.bit_size;
  }
  bit_string shifted(bit_view value, unsigned offset) {
    bit_string result;
    for (unsigned i = 0; i < offset; ++i) append_bit(result, true);
    for (std::uint64_t i = 0; i < value.size(); ++i) append_bit(result, value.at(i));
    for (unsigned i = 0; i < 5; ++i) append_bit(result, true);
    return result;
  }
  std::string number(unsigned i) {
    std::string result;
    result += char(i >> 8);
    result += char(i & 255);
    return result;
  }
  template <class P> bit_string value_for(unsigned seed) {
    auto units = P::value_width.value_or(seed % 17);
    bit_string result;
    for (std::uint64_t i = 0; i < units * P::bits_per_unit; ++i)
      append_bit(result, ((seed * 37 + i * 13) >> (i % 5)) & 1);
    return result;
  }
  template <class P> using pair_type = std::shared_ptr<profile_blob<P> const>;
  template <class P> using rows_type = std::vector<std::vector<profile_record>>;

  template <class P> pair_type<P> chain(rows_type<P> const & rows) {
    require(!rows.empty(), "chain fixture needs a terminal");
    auto tail = std::make_shared<profile_blob<P> const>(profile_blob<P>::build(rows.back()));
    std::vector<pair_type<P>> stages;
    for (auto i = rows.size() - 1; i; --i)
      stages.push_back(std::make_shared<profile_blob<P> const>(profile_blob<P>::build(rows[i - 1])));
    index_pipeline<P> pipeline(tail, std::move(stages));
    while (!pipeline.done()) {
      require(pipeline.step(0) == 0, "zero pipeline budget");
      require(pipeline.step(13) <= 13, "pipeline entry budget");
    }
    return pipeline.finish();
  }
  template <class P> std::uint64_t depth(pair_type<P> source) {
    std::uint64_t count = 0;
    for (; source; source = source->target()) ++count;
    return count;
  }
  template <class P> std::vector<query_match<P>> expected_matches(
      pair_type<P> source, rows_type<P> const & rows, bit_view query) {
    std::vector<query_match<P>> result;
    for (auto const & native : rows) {
      require(bool(source), "reference native chain is too short");
      for (std::size_t i = 0; i < native.size(); ++i)
        if (equal(native[i].key.view(), query))
          result.push_back({source, std::uint64_t(i), native[i].value});
      source = source->target();
    }
    require(!source, "reference native chain is too long");
    return result;
  }
  template <class P> void check_match(query_match<P> const & actual, query_match<P> const & expected) {
    require(actual.source == expected.source, "query emitted the wrong exact source pair");
    require(actual.ordinal == expected.ordinal, "query native ordinal mismatch");
    require(equal(actual.value.view(), expected.value.view()), "query value bits/length mismatch");
  }
  template <class P> std::vector<query_match<P>> drain(query_cursor<P> & cursor, std::uint64_t maximum_depth) {
    std::vector<query_match<P>> result;
    std::uint64_t visited = 0, calls = 0;
    while (!cursor.done()) {
      require(!cursor.failed(), "valid query cursor failed");
      bool ready = cursor.has_match(), ended = cursor.done();
      require(cursor.step(0) == 0 && cursor.has_match() == ready && cursor.done() == ended,
              "zero query budget changed traversal");
      if (ready) {
        require(cursor.step(100) == 0 && cursor.has_match(), "pending match must backpressure descent");
        result.push_back(cursor.take_match());
        require(!cursor.has_match(), "take did not consume one pending match");
      } else {
        auto budget = 1 + calls % 7;
        auto work = calls % 2 ? cursor.step(budget) : cursor.step();
        require(work > 0 && work <= (calls % 2 ? budget : 1), "query catalog budget/progress");
        visited += work;
        require(visited <= maximum_depth, "query revisited a catalog");
      }
      require(++calls <= 3 * maximum_depth + 3, "query failed to make bounded progress");
    }
    require(!cursor.has_match(), "done cursor retained an unconsumed match");
    rejects([&] { cursor.take_match(); });
    return result;
  }
  template <class P> void check_query(query_root<P> const & root, pair_type<P> original,
      rows_type<P> const & rows, bit_view query, bool copy_pending = false) {
    auto expected = expected_matches<P>(original, rows, query);
    auto cursor = root.cursor(query);
    if (copy_pending && !expected.empty()) {
      while (!cursor.has_match() && !cursor.done()) require(cursor.step() == 1, "first match progress");
      require(cursor.has_match() && !cursor.done(), "pending match must not be EOF");
      auto copied = cursor;
      auto moved = std::move(copied);
      auto actual = drain(moved, depth(root.head()));
      require(actual.size() == expected.size(), "copied cursor lost matches");
      for (std::size_t i = 0; i < actual.size(); ++i) check_match(actual[i], expected[i]);
      require(cursor.has_match(), "copied cursor consumed the original's pending match");
    }
    auto actual = drain(cursor, depth(root.head()));
    require(actual.size() == expected.size(), "query omitted or duplicated native segments");
    for (std::size_t i = 0; i < actual.size(); ++i) check_match(actual[i], expected[i]);
  }

  template <class P> std::vector<bit_string> keys() {
    std::vector<bit_string> result;
    for (auto const & key : std::vector<std::string>{"", std::string(1, '\0'), std::string("\0x", 2),
        "a", "aa", "ab", "abc", "abd", "b", "m", "z", std::string(1, char(128)), std::string(1, char(255))})
      result.push_back(text(key));
    for (unsigned i = 0; i < 97; ++i) {
      auto key = text("shared-prefix/" + number(i));
      if constexpr (P::unit == profile_unit::bit)
        for (unsigned j = 0; j < i % 7; ++j) append_bit(key, (i >> j) & 1);
      result.push_back(std::move(key));
    }
    for (unsigned i = 0; i < 9; ++i) result.push_back(text(std::string(192, 'a') + number(i)));
    if constexpr (P::unit == profile_unit::bit) {
      for (auto const * key : {"0", "00", "01", "1", "10", "101", "11", "1111111"})
        result.push_back(bit_string::from_bits(key));
    }
    std::sort(result.begin(), result.end(), [](auto const & a, auto const & b) { return order(a.view(), b.view()) < 0; });
    result.erase(std::unique(result.begin(), result.end(), [](auto const & a, auto const & b) { return equal(a.view(), b.view()); }), result.end());
    return result;
  }
  template <class P> void full_chain() {
    auto all = keys<P>();
    auto m = text("m");
    rows_type<P> rows(7);
    for (std::size_t layer = 0; layer < rows.size(); ++layer)
      for (std::size_t i = 0; i < all.size(); ++i)
        if (layer == 0 || layer + 1 == rows.size() || (i + layer) % (layer + 2) == 0 ||
            equal(all[i].view(), m.view()))
          rows[layer].push_back({all[i], value_for<P>(unsigned(103 * layer + i))});
    auto original = chain<P>(rows);
    require(original->virtual_size() > P::group_size, "large-head fixture is too small");
    query_root_builder<P> builder(original);
    require(!builder.done() && !builder.finished(), "large query root skipped preparation");
    rejects([&] { builder.finish(); });
    auto moved = std::move(builder);
    rejects([&] { builder.step(1); });
    rejects([&] { builder.finish(); });
    std::uint64_t calls = 0;
    while (!moved.done()) {
      require(moved.step(0) == 0, "zero root preparation budget");
      auto budget = 1 + calls % 11;
      auto work = moved.step(budget);
      require(work > 0 && work <= budget, "root preparation budget/progress");
      require(++calls < 100000, "root preparation failed to finish");
    }
    auto root = moved.finish();
    require(moved.finished() && moved.finish().head() == root.head(), "root finish is not stable");
    rejects([&] { moved.step(0); });
    require(root.head()->virtual_size() <= P::group_size, "prepared root exceeds one window");
    auto current = root.head();
    unsigned extra = 0;
    for (auto count = original->virtual_size(); count > P::group_size; ++extra)
      count = count / P::group_size + (count % P::group_size != 0);
    for (unsigned i = 0; i < extra; ++i) {
      require(current->native().size() == 0, "preparation added native data");
      current = current->target();
    }
    require(current == original, "preparation did not retain the exact original chain");
    for (auto const & query : all) check_query(root, original, rows, query.view());
    for (auto const * query : {"ac", "b", "ma", "none", "shared-prefix", "shared-prefix/"}) {
      auto key = text(query);
      check_query(root, original, rows, key.view(), std::string(query) == "b");
    }
    check_query(root, original, rows, m.view(), true);
    auto maximum = all.back();
    for (unsigned i = 0; i < P::bits_per_unit; ++i) append_bit(maximum, true);
    check_query(root, original, rows, maximum.view());
    auto after = root.cursor(maximum.view());
    std::uint64_t visited = 0;
    while (!after.done()) { visited += after.step(); require(!after.has_match(), "after-last query matched"); }
    require(visited == depth(root.head()), "catalog accounting missed an after-last descent");

    auto shifted_query = shifted(m.view(), 3);
    check_query(root, original, rows, shifted_query.view().subview(3, 8));
    if constexpr (P::unit == profile_unit::byte) {
      auto malformed = bit_string::from_bits("101");
      rejects([&] { root.cursor(malformed.view()); });
    }
  }

  template <class P> void equality_cut() {
    // The target samples m at ordinal2K. Two smaller borrowed samples and
    // K-3 smaller native keys place native m at K-1 and borrowed m at K.
    // A query routed into group1 must recover the native value before its cut.
    rows_type<P> rows(2);
    for (unsigned i = 0; i < 2 * P::group_size; ++i)
      rows[1].push_back({text("a" + number(i)), value_for<P>(100 + i)});
    rows[1].push_back({text("m"), value_for<P>(900)});
    rows[1].push_back({text("z"), value_for<P>(901)});
    for (unsigned i = 0; i < P::group_size - 3; ++i)
      rows[0].push_back({text("b" + number(i)), value_for<P>(200 + i)});
    rows[0].push_back({text("m"), value_for<P>(800)});
    auto source = chain<P>(rows);
    auto window = source->project(1);
    require(window.native_first == rows[0].size(), "tie fixture did not cross the native cut");
    require(source->false_borrow(window.borrowed_first), "equal sample lost its false-borrow flag");
    auto root = query_root<P>::build(source);
    for (auto const * query : {"", "l", "m", "ma", "n", "zz"}) {
      auto key = text(query);
      check_query(root, source, rows, key.view(), std::string(query) == "m");
    }
  }

  template <class P> void partial_context() {
    // A short query against a long boundary carries comparison state and the
    // actual full key length, without constructing the boundary's missing bytes.
    rows_type<P> rows(2);
    rows[0].push_back({text("c"), value_for<P>(1)});
    for (unsigned i = 0; i <= P::group_size; ++i)
      rows[1].push_back({text(std::string(192, 'a') + number(i)), value_for<P>(i + 2)});
    rows[1].push_back({text("b"), value_for<P>(800)});
    auto source = chain<P>(rows);
    auto query = text("b");
    auto window = source->search_window(0, profile_query_context<P>(query.view()));
    require(window.borrowed_predecessor.has_value(), "partial-context fixture has no route");
    auto const & context = *window.borrowed_predecessor;
    require(context.target_ordinal == P::group_size && context.comparison.order() < 0 &&
            context.comparison.common_bits() < context.comparison.full_units() * P::bits_per_unit,
            "partial-context fixture did not distinguish agreement and full length");
    auto root = query_root<P>::build(source);
    require(root.head() == source, "partial-context fixture exceeded its initial window");
    check_query(root, source, rows, query.view());
  }

  template <class P> void identities_and_invalid() {
    using blob = profile_blob<P>;
    rejects([] { query_root<P>::build({}); });
    rejects([] { query_root_builder<P> builder({}); });
    auto empty = std::make_shared<blob const>(blob::build({}));
    auto root = query_root<P>::build(empty);
    require(root.head() == empty && root.cursor({}).done(), "empty root identity/EOF");
    auto anything = text("anything");
    auto empty_cursor = root.cursor(anything.view());
    require(empty_cursor.done() && empty_cursor.step() == 0, "empty catalog was searched");
    if constexpr (P::unit == profile_unit::byte) {
      auto malformed = bit_string::from_bits("1");
      rejects([&] { root.cursor(malformed.view()); });
    }

    rows_type<P> rows{{{text("m"), value_for<P>(7)}}};
    auto source = chain<P>(rows);
    query_root_builder<P> small(source);
    require(small.done() && small.step(3) == 0, "bounded root should need no sampler");
    auto moved = std::move(small);
    rejects([&] { small.step(0); });
    rejects([&] { small.finish(); });
    auto ready = moved.finish();
    auto m = text("m");
    require(ready.head() == source, "bounded root changed source identity");
    check_query(ready, source, rows, {});
    check_query(ready, source, rows, m.view());
    auto shifted_query = shifted(m.view(), 3);
    check_query(ready, source, rows, shifted_query.view().subview(3, 8));
    auto moved_root = std::move(ready);
    rejects([&] { ready.cursor(m.view()); });
    check_query(moved_root, source, rows, m.view());

    std::vector<bit_string> samples{text("m")};
    auto unbound = std::make_shared<blob const>(blob::build({}, samples));
    rejects([&] { query_root<P>::build(unbound); });

    // A low-level builder checks sample extent when binding. Mutating an alias
    // afterward demonstrates that preparation also validates the linked shapes.
    auto mutable_tail = std::make_shared<blob>(blob::build(rows[0]));
    auto blank = std::make_shared<blob const>(blob::build({}));
    index_pipeline<P> pipeline(mutable_tail, {blank});
    while (!pipeline.done()) pipeline.step(5);
    auto malformed = pipeline.finish();
    std::vector<profile_record> replacement;
    for (unsigned i = 0; i < P::group_size + 1; ++i)
      replacement.push_back({text("a" + number(i)), value_for<P>(i)});
    *mutable_tail = blob::build(replacement);
    rejects([&] { query_root<P>::build(malformed); });

    // Public assignment can form a shape-consistent empty cycle. Break it
    // before checking the exception so the test never retains a cycle itself.
    auto cycle = std::make_shared<blob>(blob::build({}));
    index_builder<P> cyclic(*cycle);
    cyclic.close_input();
    *cycle = cyclic.finish(cycle);
    bool rejected = false;
    try { (void)query_root<P>::build(cycle); } catch (std::exception const &) { rejected = true; }
    *cycle = blob::build({});
    require(rejected, "cyclic query chain accepted");
  }

  template <class P> void ownership() {
    std::optional<query_cursor<P>> cursor;
    std::weak_ptr<profile_blob<P> const> first, tail;
    rows_type<P> rows(3);
    for (unsigned i = 0; i < rows.size(); ++i)
      rows[i].push_back({text("m"), value_for<P>(400 + i)});
    {
      auto source = chain<P>(rows);
      first = source;
      tail = source->target()->target();
      auto root = query_root<P>::build(source);
      auto m = text("m");
      auto local_query = shifted(m.view(), 3);
      cursor.emplace(root, local_query.view().subview(3, 8));
    }
    require(!first.expired() && !tail.expired(), "cursor lost owner/query storage before stepping");
    std::optional<query_match<P>> retained;
    unsigned emitted = 0;
    while (!cursor->done()) {
      if (!cursor->has_match()) { require(cursor->step() == 1, "owned cursor progress"); continue; }
      auto match = cursor->take_match();
      require(equal(match.value.view(), rows[emitted][0].value.view()), "owned query/value changed after input destruction");
      if (!emitted) retained = std::move(match);
      ++emitted;
    }
    require(emitted == rows.size(), "owned cursor lost remaining targets");
    cursor.reset();
    require(retained && !first.expired() && !tail.expired(), "returned match did not retain its exact source");
    retained->source.reset();
    require(first.expired() && tail.expired(), "completed cursor or result leaked its source pins");
    require(equal(retained->value.view(), rows[0][0].value.view()), "returned value was not independently owned");

    // A visited prefix without a result can be reclaimed immediately; the
    // cursor only needs the unvisited suffix and any pending match's source.
    rows_type<P> suffix_rows{{{text("a"), value_for<P>(1)}}, {{text("m"), value_for<P>(2)}}};
    {
      auto source = chain<P>(suffix_rows);
      first = source;
      tail = source->target();
      auto root = query_root<P>::build(source);
      auto key = text("m");
      cursor.emplace(root, key.view());
    }
    require(cursor->step() == 1 && !cursor->has_match(), "prefix miss did not retain its route");
    require(first.expired() && !tail.expired(), "visited prefix retained or unvisited suffix lost");
    require(cursor->step() == 1 && cursor->has_match() && !cursor->done(), "terminal pending-match state");
    require(!tail.expired(), "pending match lost its source pin");
    auto final_match = cursor->take_match();
    require(cursor->done() && !tail.expired(), "returned terminal match lost its source pin");
    cursor.reset();
    final_match.source.reset();
    require(tail.expired(), "terminal match source remained pinned after release");
  }

  template <class P> void exercise() {
    static_assert(std::is_same_v<typename query_root<P>::policy_type, P>);
    identities_and_invalid<P>();
    equality_cut<P>();
    partial_context<P>();
    ownership<P>();
    full_chain<P>();
  }
  template <std::uint64_t K> void groups() {
    exercise<storage_policy<profile_unit::byte, variable_values, K>>();
    exercise<storage_policy<profile_unit::bit, variable_values, K>>();
  }
}

int main() try {
  groups<3>(); groups<7>(); groups<15>(); groups<31>();
  exercise<storage_policy<profile_unit::byte, fixed_values<0>, 15>>();
  exercise<storage_policy<profile_unit::byte, fixed_values<3>, 7>>();
  exercise<storage_policy<profile_unit::bit, fixed_values<0>, 3>>();
  exercise<storage_policy<profile_unit::bit, fixed_values<5>, 31>>();
  exercise<storage_policy<profile_unit::bit, variable_values, 7, golomb<3>>>();
  exercise<storage_policy<profile_unit::bit, fixed_values<3>, 15, exponential_golomb<3>>>();
  exercise<storage_policy<profile_unit::byte, variable_values, 3, exponential_golomb<0>, 16>>();
  exercise<storage_policy<profile_unit::bit, variable_values, 7, exponential_golomb<0>, 1>>();
  exercise<storage_policy<profile_unit::byte, fixed_values<3>, 15, exponential_golomb<0>, 16>>();
  exercise<storage_policy<profile_unit::bit, fixed_values<5>, 31, golomb<3>, 16>>();
  exercise<storage_policy<profile_unit::byte, variable_values, 7, exponential_golomb<0>, 64>>();
  exercise<storage_policy<profile_unit::bit, variable_values, 3, exponential_golomb<3>, 64>>();
  std::cout << "Prepared query roots, ordered exact-chain matches, ownership and budgets passed\n";
} catch (std::exception const & error) {
  std::cerr << error.what() << '\n';
  return 1;
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests complete prepared queries against independent native-array oracles.
 */
