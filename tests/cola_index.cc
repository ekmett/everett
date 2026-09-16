/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/cola_index.h>
#include <everett/cola_query.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
  using namespace everett;

  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F && action) {
    bool rejected = false;
    try { action(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "invalid COLA operation accepted");
  }

  // Read the original logical bits directly. Expected ordering, LCP, samples,
  // ranks and routes do not use the implementation's comparison/navigation.
  bool bit(bit_view value, std::uint64_t i) {
    auto position = value.offset() + i;
    return (std::to_integer<unsigned>(value.storage()[position / 8]) >> (7 - position % 8)) & 1;
  }
  struct comparison { std::uint64_t common; int order; };
  comparison compare(bit_view a, bit_view b) {
    std::uint64_t i = 0;
    for (; i < std::min(a.size(), b.size()); ++i)
      if (bit(a, i) != bit(b, i)) return {i, bit(a, i) ? 1 : -1};
    return {i, a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0};
  }
  bool equal(bit_view a, bit_view b) { return !compare(a, b).order; }
  void append_bit(bit_string & value, bool one) {
    if (!(value.bit_size % 8)) value.bytes.push_back(std::byte{0});
    if (one) value.bytes.back() |= std::byte(1u << (7 - value.bit_size % 8));
    ++value.bit_size;
  }
  bit_string text(std::string const & value) { return bit_string::from_bytes(value); }
  std::string number(unsigned i) {
    return std::string{char(i >> 16), char(i >> 8), char(i)};
  }
  template <class P> bit_string value_for(unsigned seed) {
    bit_string value;
    auto units = P::value_width.value_or(seed % 19);
    for (std::uint64_t i = 0; i < units * P::bits_per_unit; ++i)
      append_bit(value, ((seed * 29 + i * 11) >> (i % 5)) & 1);
    return value;
  }
  void sort_keys(std::vector<bit_string> & keys) {
    std::sort(keys.begin(), keys.end(), [](auto const & a, auto const & b) {
      return compare(a.view(), b.view()).order < 0;
    });
    keys.erase(std::unique(keys.begin(), keys.end(), [](auto const & a, auto const & b) {
      return equal(a.view(), b.view());
    }), keys.end());
  }
  template <class P> std::vector<profile_record> rows(std::vector<bit_string> keys, unsigned salt) {
    sort_keys(keys);
    std::vector<profile_record> result;
    for (std::size_t i = 0; i != keys.size(); ++i)
      result.push_back({std::move(keys[i]), value_for<P>(salt + unsigned(i))});
    return result;
  }
  template <class Range, class Key>
  std::size_t upper(Range const & range, bit_view query, Key key) {
    std::size_t first = 0, last = range.size();
    while (first != last) {
      auto middle = first + (last - first) / 2;
      if (compare(key(range[middle]), query).order <= 0) first = middle + 1;
      else last = middle;
    }
    return first;
  }
  std::optional<std::uint64_t> find(std::span<profile_record const> rows, bit_view query) {
    auto end = upper(rows, query, [](auto const & row) { return row.key.view(); });
    if (end && equal(rows[end - 1].key.view(), query)) return end - 1;
    return std::nullopt;
  }
  template <class P> void check_context(profile_query_context<P> const & actual,
                                       bit_view key, bit_view query) {
    auto expected = compare(key, query);
    require(equal(actual.query(), query), "comparison lost the owned query");
    require(actual.common_bits() == expected.common && actual.order() == expected.order,
            "comparison differs from original-key LCP/direction");
    require(!actual.full_units() || *actual.full_units() == key.size() / P::bits_per_unit,
            "known comparison length is wrong");
    if (!expected.order) require(actual.full_units() == query.size() / P::bits_per_unit,
                                 "equality must establish the full length");
  }

  struct occurrence {
    bit_string key;
    unsigned origin;
    std::uint64_t ordinal;
  };
  template <class P> struct model {
    using pair_type = typename cola_index<P>::pair_type;
    using native_pointer = typename cola_index<P>::native_pointer;
    std::vector<profile_record> native_rows, secondary_rows;
    std::shared_ptr<model const> main;
    native_pointer native, secondary;
    pair_type encoded;
    std::array<std::vector<bit_string>, 2> samples;
    std::vector<occurrence> catalog;
  };

  template <class P> std::shared_ptr<model<P> const> make_node(
      std::vector<profile_record> native_rows, std::shared_ptr<model<P> const> main = {},
      std::vector<profile_record> secondary_rows = {}, bool explicit_secondary = false) {
    auto result = std::make_shared<model<P>>();
    result->native_rows = std::move(native_rows);
    result->secondary_rows = std::move(secondary_rows);
    result->main = std::move(main);
    result->native = std::make_shared<profile_array<P> const>(profile_array<P>::build(result->native_rows));
    if (!result->secondary_rows.empty() || explicit_secondary)
      result->secondary = std::make_shared<profile_array<P> const>(profile_array<P>::build(result->secondary_rows));
    if (result->main)
      for (std::size_t i = 0; i < result->main->catalog.size(); i += P::group_size)
        result->samples[0].push_back(result->main->catalog[i].key);
    for (std::size_t i = 0; i < result->secondary_rows.size(); i += P::group_size)
      result->samples[1].push_back(result->secondary_rows[i].key);
    for (std::size_t i = 0; i != result->native_rows.size(); ++i)
      result->catalog.push_back({result->native_rows[i].key, 0, i});
    for (unsigned route = 0; route != 2; ++route)
      for (std::size_t i = 0; i != result->samples[route].size(); ++i)
        result->catalog.push_back({result->samples[route][i], route + 1, i});
    // Each origin was appended in ordinal order. Naming that final tie break
    // preserves the stable occurrence order without a temporary-buffer sort.
    std::sort(result->catalog.begin(), result->catalog.end(), [](auto const & a, auto const & b) {
      auto order = compare(a.key.view(), b.key.view()).order;
      if (order) return order < 0;
      if (a.origin != b.origin) return a.origin < b.origin;
      return a.ordinal < b.ordinal;
    });
    cola_index_builder<P> builder(result->native, result->main ? result->main->encoded : nullptr, result->secondary);
    std::uint64_t consumed = 0, calls = 0;
    while (!builder.done()) {
      auto size = builder.size();
      require(!builder.failed() && builder.step(0) == 0 && builder.size() == size,
              "zero budget changed COLA builder");
      std::uint64_t budget = calls % 3 ? 1 : 11;
      auto work = builder.step(budget);
      require(work && work <= budget, "COLA builder exceeded or stalled its entry budget");
      consumed += work;
      require(++calls <= result->catalog.size() + 1, "COLA builder failed to progress");
    }
    require(consumed == result->catalog.size(), "COLA builder did not charge each virtual entry once");
    result->encoded = std::make_shared<cola_index<P> const>(builder.finish());
    require(builder.finished(), "finish did not consume COLA builder");
    require(&result->encoded->native() == result->native.get(), "COLA builder copied the native array");
    require(result->encoded->main_target() == (result->main ? result->main->encoded : nullptr) &&
            result->encoded->secondary_target() == result->secondary, "COLA builder changed exact target owners");
    return result;
  }

  template <class P> void check_structure(model<P> const & input) {
    auto const & node = *input.encoded;
    auto view = node.view();
    auto groups = (input.catalog.size() + P::group_size - 1) / P::group_size;
    require(view.virtual_size() == input.catalog.size() && view.group_count() == groups,
            "COLA catalog extent differs from stable three-way oracle");
    std::array<std::uint64_t, 3> counts{};
    for (std::size_t first = 0; first < input.catalog.size(); first += P::group_size) {
      auto group = first / P::group_size;
      auto projected = view.project(group);
      require(projected.native_first == counts[0], "native projected start");
      for (unsigned route = 0; route != 2; ++route) {
        auto previous = counts[route + 1];
        require(projected.borrowed_first[route] == previous && node.interleave(route).view().rank(group) == previous,
                "borrowed projected start/rank");
        auto lcp = previous ? compare(input.samples[route][previous - 1].view(),
                                     input.catalog[first].key.view()).common : 0;
        require(node.cut_lcps(route)[group] == lcp, "cut metadata is not exact per-stream predecessor LCP");
      }
      auto before = counts;
      for (auto i = first; i < std::min<std::size_t>(first + P::group_size, input.catalog.size()); ++i)
        ++counts[input.catalog[i].origin];
      require(projected.native_last == counts[0], "native projected end");
      for (unsigned route = 0; route != 2; ++route)
        require(projected.borrowed_last[route] == counts[route + 1] &&
                node.interleave(route).view().class_at(group) == counts[route + 1] - before[route + 1],
                "borrowed projected end/class");
    }
    for (unsigned route = 0; route != 2; ++route) {
      auto borrowed = node.borrowed(route).view();
      auto cursor = borrowed.cursor();
      bit_view previous;
      for (std::size_t i = 0; i != input.samples[route].size(); ++i) {
        require(!cursor.done(), "borrowed stream ended early");
        auto item = cursor.peek();
        auto key = input.samples[route][i].view();
        require(equal(item.key.prefix, key) && item.value.empty(), "borrowed physical key/value mismatch");
        auto encoded = borrowed.encoded_at(i);
        require(encoded.retained == compare(previous, key).common / P::bits_per_unit &&
                encoded.key_units == key.size() / P::bits_per_unit, "borrowed stream is not ordinary physical FC");
        auto flags = node.false_borrow_bits(route);
        auto flag = (std::to_integer<unsigned>(flags[i / 8]) >> (i % 8)) & 1;
        require(bool(flag) == bool(find(input.native_rows, key)), "false flag must mean local native equality only");
        previous = key;
        cursor.advance();
      }
      require(cursor.done(), "borrowed stream has extra records");
      require(node.cut_lcps(route).size() == groups, "missing per-route cut metadata");
    }
    cola_sample_cursor<P> sampler(input.encoded);
    for (std::size_t i = 0; i < input.catalog.size(); i += P::group_size) {
      require(!sampler.done(), "sampler ended early");
      auto sample = sampler.peek();
      auto const & expected = input.catalog[i];
      require(equal(sample.key, expected.key.view()) && sample.target_ordinal == i &&
              unsigned(sample.origin) == expected.origin && sample.source_ordinal == expected.ordinal,
              "sampler differs from independently sorted occurrence/tie order");
      sampler.advance();
    }
    require(sampler.done(), "sampler has extra output");
    rejects([&] { sampler.peek(); });
    rejects([&] { sampler.advance(); });
    rejects([&] { view.project(groups); });
    rejects([&] { view.borrowed(2); });
    rejects([&] { view.false_borrow(0, input.samples[0].size()); });
  }

  template <class P> void check_query(model<P> const & input, bit_view query, unsigned shift) {
    if (input.catalog.empty()) return;
    auto end = upper(input.catalog, query, [](auto const & x) { return x.key.view(); });
    auto group = (end ? end - 1 : 0) / P::group_size;
    bit_string displaced;
    for (unsigned i = 0; i < shift; ++i) append_bit(displaced, true);
    for (std::uint64_t i = 0; i < query.size(); ++i) append_bit(displaced, bit(query, i));
    auto lower = profile_query_context<P>(displaced.view().subview(shift, query.size()));
    displaced = {};
    if (group) lower = lower.with_key(input.catalog[group * P::group_size].key.view());
    profile_comparison_work native_work;
    std::array<profile_comparison_work, 2> borrowed_work;
    auto result = input.encoded->view().search_window(group, lower, &native_work,
      {&borrowed_work[0], &borrowed_work[1]});
    require(native_work.skipped_headers < P::codec_block_size &&
            borrowed_work[0].skipped_headers < P::codec_block_size &&
            borrowed_work[1].skipped_headers < P::codec_block_size,
            "comparison replay exceeded one physical block");
    require(native_work.visited_headers + borrowed_work[0].visited_headers + borrowed_work[1].visited_headers
            <= P::group_size, "three projected streams exceed one virtual window");
    auto expected_native = find(input.native_rows, query);
    require(bool(result.native) == bool(expected_native), "routed search lost/invented native match");
    if (expected_native)
      require(result.native->ordinal == *expected_native &&
              equal(result.native->value.view(), input.native_rows[*expected_native].value.view()),
              "routed search returned wrong native ordinal/value");
    for (unsigned route = 0; route != 2; ++route) {
      auto n = upper(input.samples[route], query, [](auto const & x) { return x.view(); });
      auto const & actual = result.predecessors[route];
      require(bool(actual) == bool(n), "outgoing predecessor presence");
      if (n) {
        require(actual->ordinal == n - 1 && actual->target_ordinal == (n - 1) * P::group_size,
                "outgoing predecessor ordinal/target");
        require(actual->false_borrow == bool(find(input.native_rows, input.samples[route][n - 1].view())),
                "outgoing predecessor false flag");
        check_context(actual->comparison, input.samples[route][n - 1].view(), query);
        if (route == 1) {
          auto found = cola_search_secondary<P>(input.secondary->view(), *actual);
          auto wanted = find(input.secondary_rows, query);
          require(bool(found) == bool(wanted), "terminal leaf search presence");
          if (wanted) require(found->ordinal == *wanted &&
            equal(found->value.view(), input.secondary_rows[*wanted].value.view()), "terminal leaf match");
        }
      }
    }
  }

  struct match { void const * source; std::uint64_t ordinal; bit_string value; };
  template <class P> void walk(typename cola_index<P>::pair_type const & node,
      std::uint64_t group, profile_query_context<P> const & lower, std::vector<match> & result,
      std::uint64_t & visits) {
    if (!node || !node->virtual_size()) return;
    require(++visits < 64, "COLA query did not terminate");
    auto found = node->view().search_window(group, lower);
    if (found.native) result.push_back({&node->native(), found.native->ordinal, found.native->value});
    if (found.predecessors[0]) {
      auto const & predecessor = *found.predecessors[0];
      require(bool(node->main_target()), "main route is unbound");
      walk<P>(node->main_target(), predecessor.ordinal, predecessor.comparison, result, visits);
    }
    if (found.predecessors[1]) {
      require(bool(node->secondary_target()), "secondary route is unbound");
      auto leaf = cola_search_secondary<P>(node->secondary_target()->view(), *found.predecessors[1]);
      if (leaf) result.push_back({node->secondary_target().get(), leaf->ordinal, leaf->value});
    }
  }
  template <class P> void expected(model<P> const & node, bit_view query, std::vector<match> & result) {
    if (auto i = find(node.native_rows, query)) result.push_back({node.native.get(), *i, node.native_rows[*i].value});
    if (node.main) expected(*node.main, query, result);
    if (auto i = find(node.secondary_rows, query)) result.push_back({node.secondary.get(), *i, node.secondary_rows[*i].value});
  }
  void check_matches(std::vector<match> const & actual, std::vector<match> const & wanted) {
    require(actual.size() == wanted.size(), "complete routing lost/duplicated a native source");
    for (std::size_t i = 0; i != wanted.size(); ++i)
      require(actual[i].source == wanted[i].source && actual[i].ordinal == wanted[i].ordinal &&
              equal(actual[i].value.view(), wanted[i].value.view()), "complete routing source/order/value");
  }

  template <class P> void expected_query(model<P> const & node, bit_view query, std::vector<match> & result) {
    if (auto i = find(node.native_rows, query)) result.push_back({node.native.get(), *i, node.native_rows[*i].value});
    if (auto i = find(node.secondary_rows, query)) result.push_back({node.secondary.get(), *i, node.secondary_rows[*i].value});
    if (node.main) expected_query(*node.main, query, result);
  }
  template <class P> std::vector<match> drain(cola_query_cursor<P> & cursor) {
    std::vector<match> result;
    std::uint64_t visits = 0, calls = 0;
    while (!cursor.done()) {
      auto pending = cursor.has_match();
      require(!cursor.failed() && !cursor.step(0) && cursor.has_match() == pending,
              "zero query budget changed cursor");
      if (pending) {
        require(!cursor.step(19), "pending query match did not backpressure traversal");
        auto found = cursor.take_match();
        auto source = found.secondary ? found.source->secondary_target().get() : &found.source->native();
        require(source, "query match has no owning native source");
        result.push_back({source, found.ordinal, std::move(found.value)});
      } else {
        std::uint64_t budget = calls % 3 ? 1 : 7;
        auto work = cursor.step(budget);
        require(work && work <= budget, "query exceeded or stalled its main-node budget");
        visits += work;
        require(visits < 64, "query revisited a main node");
      }
      require(++calls < 192, "query did not terminate");
    }
    rejects([&] { cursor.take_match(); });
    return result;
  }

  template <class P> struct test_target {
    using native_pointer = typename cola_index<P>::native_pointer;
    std::shared_ptr<cola_index<P> const> storage;
    std::shared_ptr<test_target const> main;
    native_pointer secondary;
    auto view() const { return storage->view(); }
    auto main_target() const { return main; }
    auto secondary_target() const { return secondary; }
    auto virtual_size() const { return storage->virtual_size(); }
    auto group_count() const { return storage->group_count(); }
  };
  template <class P> void malformed_chains() {
    using root = cola_query_root<P, test_target<P>>;
    auto empty = std::make_shared<cola_index<P> const>(cola_index<P>::build({}));
    auto cycle = std::make_shared<test_target<P>>();
    cycle->storage = empty;
    cycle->main = cycle;
    rejects([&] { root::adopt_prepared(cycle); });
    cycle->main.reset();
    auto records = rows<P>({text("a")}, 281);
    auto terminal = std::make_shared<cola_index<P> const>(cola_index<P>::build(records));
    auto indexed = std::make_shared<cola_index<P> const>(cola_index<P>::build({}, terminal));
    auto unbound = std::make_shared<test_target<P>>();
    unbound->storage = indexed;
    rejects([&] { root::adopt_prepared(unbound); });
    unbound->storage = empty;
    unbound->secondary = terminal->native_owner();
    rejects([&] { root::adopt_prepared(unbound); });
  }

  template <class P> void endpoints_and_ownership() {
    std::vector<bit_string> keys{{}, text(std::string(1, '\0')), text("a"), text("aa"), text("ab"), text("b")};
    if constexpr (P::unit == profile_unit::bit) {
      keys.push_back(bit_string::from_bits("0"));
      keys.push_back(bit_string::from_bits("011"));
      keys.push_back(bit_string::from_bits("10101"));
    }
    auto base = make_node<P>(rows<P>(keys, 301));
    auto node = make_node<P>(rows<P>(keys, 331), base, rows<P>(keys, 351));
    check_structure(*node);
    keys.push_back(text("aaa"));
    keys.push_back(text("z"));
    for (auto const & query : keys) check_query(*node, query.view(), 5);
    auto query = text("a");
    auto boundary = text("z");
    auto bad_lower = profile_query_context<P>(query.view()).with_key(boundary.view());
    rejects([&] { node->encoded->view().search_window(0, bad_lower); });

    auto owner = std::make_shared<cola_index<P> const>(cola_index<P>::build(node->native_rows));
    std::weak_ptr<cola_index<P> const> weak = owner;
    std::optional<cola_sample_cursor<P>> sampler;
    sampler.emplace(owner);
    owner.reset();
    require(!weak.expired(), "sampler lost its source owner");
    auto moved_sampler = std::move(*sampler);
    sampler.reset();
    require(!weak.expired(), "moving sampler lost source owner");
    while (!moved_sampler.done()) moved_sampler.advance();

    cola_index_builder<P> original(node->native, base->encoded, node->secondary);
    original.step(1);
    auto moved = std::move(original);
    rejects([&] { original.step(0); });
    rejects([&] { original.finish(); });
    while (!moved.done()) moved.step(3);
    auto completed = moved.finish();
    require(completed.virtual_size() == node->catalog.size(), "moved builder lost partial state");
    rejects([&] { moved.step(0); });
    rejects([&] { moved.finish(); });

    // Only the query cursor retains the chain; both pending matches survive
    // destruction of its root and the caller's query bytes.
    auto cursor = [&] {
      auto local = cola_query_root<P>::build(node->encoded);
      auto storage = query;
      auto result = local.cursor_owned(std::move(storage));
      while (!result.has_match() && !result.done()) result.step();
      return result;
    }();
    auto moving = std::move(cursor);
    require(cursor.done() && !cursor.has_match() && !cursor.step(), "moved-from query retained pending matches");
    cursor = std::move(moving);
    require(moving.done() && !moving.has_match(), "move assignment retained source pending matches");
    auto pending_copy = cursor;
    pending_copy.take_match();
    require(pending_copy.has_match(), "fixture must retain both local and secondary pending matches");
    std::vector<match> wanted;
    expected_query(*node, query.view(), wanted);
    auto retained_source = node->encoded;
    std::weak_ptr<cola_index<P> const> retained = retained_source;
    node.reset(); base.reset(); retained_source.reset();
    require(!retained.expired(), "pending query did not pin exact source");
    check_matches(drain(cursor), wanted);
  }

  template <class P> void malformed_projection() {
    constexpr auto k = P::group_size;
    auto records = rows<P>({text("a")}, 401);
    auto native = profile_array<P>::build(records);
    profile_borrowed_writer<P> writer;
    auto key = text("a");
    for (std::uint64_t i = 0; i < k; ++i) writer.append(key.view());
    auto borrowed = writer.finish();
    std::vector<std::byte> flags((k + 7) / 8);
    std::vector<std::uint64_t> cuts(3);
    auto make_view = [&](auto const & rank) {
      return cola_index_view<P>(native.view(), {borrowed.view(), borrowed.view()},
        {rank.view(), rank.view()}, {flags, flags},
        {word_view(std::span<std::uint64_t const>(cuts)), word_view(std::span<std::uint64_t const>(cuts))}, 2 * k + 1);
    };
    std::array<std::uint64_t, 3> populations{k, 0, 0};
    auto overlapping = rank_groups<k>::build(populations, 2 * k + 1);
    auto bad = make_view(overlapping);
    rejects([&] { bad.project(0); }); // Individually valid classes sum above K.
    rejects([&] { bad.project(1); }); // Individually valid prefixes sum above first.
    populations = {k - 1, 0, 1};
    auto partial = rank_groups<k>::build(populations, 2 * k + 1);
    auto bad_partial = make_view(partial);
    rejects([&] { bad_partial.project(2); }); // Final width is one, population sum is two.
    populations = {};
    auto missing = rank_groups<k>::build(populations, 2 * k + 1);
    auto bad_native = make_view(missing);
    rejects([&] { bad_native.project(0); });
    cuts.pop_back();
    rejects([&] { make_view(missing); });

    profile_blob_borrowed_predecessor<P> predecessor{0, 0, false, profile_query_context<P>(key.view()).with_key(key.view())};
    predecessor.target_ordinal = 1;
    rejects([&] { cola_search_secondary<P>(native.view(), predecessor); });
    predecessor.target_ordinal = k;
    rejects([&] { cola_search_secondary<P>(native.view(), predecessor); });
  }

#if defined(__unix__) || defined(__APPLE__)
  struct inaccessible_interior {
    void * address = nullptr;
    std::size_t size = 0;
    explicit inaccessible_interior(std::span<std::byte const> storage) {
      auto page_result = ::sysconf(_SC_PAGESIZE);
      require(page_result > 0, "page size unavailable");
      auto page = std::uintptr_t(page_result);
      auto begin = reinterpret_cast<std::uintptr_t>(storage.data());
      auto first = ((begin + page - 1) / page + 1) * page;
      auto last = ((begin + storage.size()) / page - 1) * page;
      require(last > first, "guard fixture lacks complete interior pages");
      address = reinterpret_cast<void *>(first);
      size = last - first;
      require(!::mprotect(address, size, PROT_NONE), "cannot protect FC prefix");
    }
    ~inaccessible_interior() {
      if (::mprotect(address, size, PROT_READ | PROT_WRITE)) std::terminate();
    }
  };
  template <class P> void no_old_prefix_reads() {
    auto page = ::sysconf(_SC_PAGESIZE);
    require(page > 0, "page size unavailable");
    auto prefix_size = std::size_t(page) * 6;
    std::vector<bit_string> keys;
    for (unsigned i = 0; i < 2 * P::codec_block_size * P::group_size + 9; ++i)
      keys.push_back(text(std::string(prefix_size, 'p') + number(i)));
    auto base = make_node<P>(rows<P>(keys, 501));
    auto node = make_node<P>(rows<P>(keys, 531), base, rows<P>(keys, 551));
    auto query = keys[keys.size() - 7].view();
    auto end = upper(node->catalog, query, [](auto const & item) { return item.key.view(); });
    auto group = (end - 1) / P::group_size;
    auto projected = node->encoded->view().project(group);
    require(projected.borrowed_first[0] >= P::codec_block_size &&
            projected.borrowed_first[1] >= P::codec_block_size, "guard did not cross both physical boundaries");
    inaccessible_interior native_guard(node->encoded->native().bytes().first(prefix_size));
    inaccessible_interior main_guard(node->encoded->borrowed(0).bytes().first(prefix_size));
    inaccessible_interior side_guard(node->encoded->borrowed(1).bytes().first(prefix_size));
    check_query(*node, query, 3);
  }
#endif

  template <class P> void matrix() {
    endpoints_and_ownership<P>();
    malformed_chains<P>();
    malformed_projection<P>();
    std::vector<bit_string> keys;
    auto count = 2 * P::codec_block_size * P::group_size + 3 * P::group_size + 41;
    for (unsigned i = 0; i < count; ++i) {
      auto key = text("m/common/" + number(i));
      if constexpr (P::unit == profile_unit::bit)
        for (unsigned j = 0; j < i % 5; ++j) append_bit(key, (i >> j) & 1);
      keys.push_back(std::move(key));
    }
    auto base = make_node<P>(rows<P>(keys, 31));
    auto middle = make_node<P>(rows<P>(keys, 71), base, rows<P>(keys, 101));
    auto natives = keys;
    for (unsigned i = 0; i < P::group_size - 1; ++i) natives.push_back(text("a/" + number(i)));
    auto top = make_node<P>(rows<P>(natives, 151), middle, rows<P>(keys, 191));
    check_structure(*base);
    check_structure(*middle);
    check_structure(*top);
    // The first three equal origins straddle a virtual cut. Both false flags
    // name one local native, and both child routes must still be followed.
    require(top->catalog[P::group_size - 1].origin == 0 && top->catalog[P::group_size].origin == 1 &&
            top->catalog[P::group_size + 1].origin == 2 &&
            equal(top->catalog[P::group_size - 1].key.view(), top->catalog[P::group_size + 1].key.view()),
            "fixture did not force all-origin equality across a cut");
    auto prepared = cola_index<P>::prepare_root(top->encoded, top->secondary);
    require(prepared && prepared->virtual_size() <= P::group_size, "prepared root exceeds one window");
    auto query_root = cola_query_root<P>::adopt_prepared(prepared);
    rejects([&] { cola_query_root<P>::adopt_prepared(nullptr); });
    rejects([&] { cola_query_root<P>::adopt_prepared(top->encoded); });
    std::vector<bit_string> queries = natives;
    queries.push_back({});
    queries.push_back(text("m/common/"));
    queries.push_back(text("zz"));
    for (std::size_t i = 0; i < keys.size(); i += 17) {
      auto extended = keys[i];
      for (unsigned j = 0; j != P::bits_per_unit; ++j) append_bit(extended, false);
      queries.push_back(std::move(extended));
    }
    for (std::size_t i = 0; i != queries.size(); ++i) {
      auto query = queries[i].view();
      check_query(*top, query, unsigned(i % 8));
      std::vector<match> actual, wanted;
      std::uint64_t visits = 0;
      walk<P>(prepared, 0, profile_query_context<P>(query), actual, visits);
      expected(*top, query, wanted);
      if (auto found = find(top->secondary_rows, query))
        wanted.push_back({top->secondary.get(), *found, top->secondary_rows[*found].value});
      check_matches(actual, wanted);
      auto cursor = i & 1 ? query_root.cursor_owned(bit_string::copy(query)) : query_root.cursor(query);
      if (i % 29 == 0) {
        cursor.step(1);
        auto copy = cursor;
        check_matches(drain(cursor), drain(copy));
        cursor = query_root.cursor_owned(bit_string::copy(query));
      }
      std::vector<match> query_expected;
      if (auto found = find(top->secondary_rows, query))
        query_expected.push_back({top->secondary.get(), *found, top->secondary_rows[*found].value});
      expected_query(*top, query, query_expected);
      check_matches(drain(cursor), query_expected);
    }
    auto empty = make_node<P>({}, {}, {}, true);
    auto main_only = make_node<P>({}, base);
    auto secondary_only = make_node<P>({}, {}, rows<P>(keys, 211));
    auto both_only = make_node<P>({}, middle, rows<P>(keys, 231));
    check_structure(*empty);
    check_structure(*main_only);
    check_structure(*secondary_only);
    check_structure(*both_only);
    for (auto const & query : std::vector<bit_string>{{}, keys.front(), keys.back(), text("zz")}) {
      check_query(*main_only, query.view(), 3);
      check_query(*secondary_only, query.view(), 5);
      check_query(*both_only, query.view(), 7);
    }
    auto no_targets = cola_index<P>::prepare_root();
    require(no_targets && !no_targets->virtual_size(), "empty prepared root");
    auto empty_cursor = cola_query_root<P>::build().cursor(bit_view{});
    require(empty_cursor.done() && !empty_cursor.step(), "empty query root should terminate immediately");
    rejects([&] { cola_index<P>::adopt_native(nullptr); });
    rejects([&] { cola_index_builder<P> bad(nullptr); });
    if constexpr (P::unit == profile_unit::byte)
      rejects([&] { auto key = bit_string::from_bits("1"); profile_query_context<P> bad(key.view()); });
  }
}

int main() {
  try {
    matrix<storage_policy<everett::tip<everett::encoded_sort<everett::byte_encoding<>>>, 3, exponential_golomb<0>, 15>>();
    matrix<storage_policy<everett::tip<everett::encoded_sort<everett::bit_encoding<>>>, 3, golomb<3>, 16>>();
    matrix<storage_policy<everett::tip<everett::encoded_sort<everett::byte_encoding<fixed_values<0>>>>, 7, exponential_golomb<0>, 16>>();
    matrix<storage_policy<everett::tip<everett::encoded_sort<everett::bit_encoding<fixed_values<13>>>>, 7, exponential_golomb<2>, 15>>();
    matrix<storage_policy<everett::tip<everett::encoded_sort<everett::byte_encoding<fixed_values<7>>>>, 15, exponential_golomb<0>, 16>>();
    matrix<storage_policy<everett::tip<everett::encoded_sort<everett::bit_encoding<>>>, 15, golomb<17>, 15>>();
    matrix<storage_policy<everett::tip<everett::encoded_sort<everett::byte_encoding<>>>, 31, exponential_golomb<0>, 15>>();
    matrix<storage_policy<everett::tip<everett::encoded_sort<everett::bit_encoding<fixed_values<0>>>>, 31, exponential_golomb<1>, 16>>();
#if defined(__unix__) || defined(__APPLE__)
    no_old_prefix_reads<storage_policy<everett::tip<everett::encoded_sort<everett::byte_encoding<>>>, 3, exponential_golomb<0>, 15>>();
    no_old_prefix_reads<storage_policy<everett::tip<everett::encoded_sort<everett::bit_encoding<fixed_values<13>>>>, 3, exponential_golomb<2>, 16>>();
#endif
    std::cout << "COLA index tests passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
