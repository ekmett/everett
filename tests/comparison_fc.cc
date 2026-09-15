/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Independently checks ordinary-FC comparison state and exact cut LCP metadata.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/profile_blob.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
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
  using namespace diet;

  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F && action) {
    bool rejected = false;
    try { action(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "invalid comparison-FC operation accepted");
  }

  // This oracle reads the original logical bits individually. It does not use
  // the library's key comparisons, LCPs, count codecs, samples or rank/select.
  bool original_bit(bit_view value, std::uint64_t i) {
    auto position = value.offset() + i;
    return (std::to_integer<unsigned>(value.storage()[position / 8]) >>
      (7 - position % 8)) & 1;
  }
  struct comparison {
    std::uint64_t common_bits;
    int order;
  };
  comparison compare_original(bit_view a, bit_view b) {
    std::uint64_t i = 0;
    for (; i < std::min(a.size(), b.size()); ++i)
      if (original_bit(a, i) != original_bit(b, i))
        return {i, original_bit(a, i) ? 1 : -1};
    return {i, a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0};
  }
  bool same_bits(bit_view a, bit_view b) { return compare_original(a, b).order == 0; }
  bit_string text(std::string const & value) { return bit_string::from_bytes(value); }
  void append_bit(bit_string & value, bool bit) {
    if (value.bit_size % 8 == 0) value.bytes.push_back(std::byte{0});
    if (bit) value.bytes.back() |= std::byte(1u << (7 - value.bit_size % 8));
    ++value.bit_size;
  }
  bit_string prefix_of(bit_view value, std::uint64_t count) {
    require(count <= value.size(), "oracle prefix is out of bounds");
    bit_string result;
    for (std::uint64_t i = 0; i < count; ++i) append_bit(result, original_bit(value, i));
    return result;
  }
  bit_string displaced(bit_view value, unsigned shift) {
    bit_string result;
    for (unsigned i = 0; i != shift; ++i) append_bit(result, true);
    for (std::uint64_t i = 0; i != value.size(); ++i) append_bit(result, original_bit(value, i));
    for (unsigned i = 0; i != 9; ++i) append_bit(result, true);
    return result;
  }
  std::string number(unsigned n) {
    std::string result(2, '\0');
    result[0] = char(n >> 8);
    result[1] = char(n & 255);
    return result;
  }
  template <class P> bit_string value_for(unsigned seed) {
    auto units = P::value_width.value_or(seed % 19);
    bit_string value;
    for (std::uint64_t i = 0; i < units * P::bits_per_unit; ++i)
      append_bit(value, ((seed * 19 + i * 37) >> (i % 7)) & 1);
    return value;
  }
  void sort_keys(std::vector<bit_string> & keys, bool unique) {
    std::stable_sort(keys.begin(), keys.end(), [](auto const & a, auto const & b) {
      return compare_original(a.view(), b.view()).order < 0;
    });
    if (unique)
      keys.erase(std::unique(keys.begin(), keys.end(), [](auto const & a, auto const & b) {
        return same_bits(a.view(), b.view());
      }), keys.end());
  }

  struct occurrence {
    bit_string key;
    bool borrowed;
    std::uint64_t ordinal;
  };
  std::vector<occurrence> merged_original(std::span<profile_record const> native,
      std::span<bit_string const> borrowed) {
    std::vector<occurrence> result;
    for (std::size_t i = 0; i != native.size(); ++i)
      result.push_back({native[i].key, false, std::uint64_t(i)});
    for (std::size_t i = 0; i != borrowed.size(); ++i)
      result.push_back({borrowed[i], true, std::uint64_t(i)});
    std::stable_sort(result.begin(), result.end(), [](auto const & a, auto const & b) {
      auto order = compare_original(a.key.view(), b.key.view()).order;
      return order ? order < 0 : a.borrowed < b.borrowed;
    });
    return result;
  }

  template <class P> struct fixture {
    std::vector<profile_record> native;
    std::vector<bit_string> borrowed;
    std::vector<bit_string> queries;

    fixture(std::vector<bit_string> natives, std::vector<bit_string> samples,
        std::vector<bit_string> extra = {}) : borrowed(std::move(samples)), queries(std::move(extra)) {
      sort_keys(natives, true);
      sort_keys(borrowed, false);
      for (std::size_t i = 0; i != natives.size(); ++i)
        native.push_back({natives[i], value_for<P>(unsigned(i) + 1)});
      queries.insert(queries.end(), natives.begin(), natives.end());
      queries.insert(queries.end(), borrowed.begin(), borrowed.end());
      queries.push_back({});
      queries.push_back(text(std::string(2, char(255))));
      // Every endpoint and a one-unit extension is a query. These catch states
      // that confuse equal content with equal full key lengths.
      for (auto const & key : natives) {
        if (key.bit_size) queries.push_back(prefix_of(key.view(), key.bit_size - P::bits_per_unit));
        auto extended = key;
        for (unsigned i = 0; i != P::bits_per_unit; ++i) append_bit(extended, false);
        queries.push_back(std::move(extended));
      }
      sort_keys(queries, true);
    }
  };

  template <class P> std::vector<bit_string> key_universe() {
    std::vector<bit_string> keys;
    for (auto const & key : std::vector<std::string>{"", std::string(1, '\0'), std::string("\0x", 2),
        "a", "aa", "ab", "abc", "abd", "b", "m", "z", std::string(1, char(128)),
        std::string(1, char(255))}) keys.push_back(text(key));
    for (unsigned i = 0; i != 121; ++i) {
      auto key = text(std::string(81, 'p') + number(i));
      if constexpr (P::unit == profile_unit::bit)
        for (unsigned j = 0; j != i % 7; ++j) append_bit(key, (i >> j) & 1);
      keys.push_back(std::move(key));
    }
    if constexpr (P::unit == profile_unit::bit)
      for (auto const * key : {"0", "00", "01", "1", "10", "101", "11", "1111111"})
        keys.push_back(bit_string::from_bits(key));
    sort_keys(keys, true);
    return keys;
  }

  // In both variants the cut boundary is prefix+"by". Its preceding borrowed
  // key is prefix+"aa" or prefix+"ba", represented locally by the same FC
  // suffix "a" and retained length. Full-key answers must still differ.
  template <class P> fixture<P> indistinguishable_frontier(char hidden, unsigned padding) {
    std::string prefix(padding, 'p');
    std::vector<bit_string> native;
    for (unsigned i = 0; i != P::group_size - 3; ++i)
      native.push_back(text(prefix + "bc/" + number(i)));
    for (auto const * key : {"bx", "by", "zz"}) native.push_back(text(prefix + key));
    std::vector<bit_string> borrowed{text(prefix + hidden + "0"), text(prefix + hidden + "a"),
      text(prefix + "za")};
    return {std::move(native), std::move(borrowed), {text(prefix + "bz")}};
  }

  template <class P> void check_context(profile_query_context<P> const & actual,
      bit_view key, bit_view query) {
    auto expected = compare_original(key, query);
    require(same_bits(actual.query(), query), "comparison context lost its owned query");
    require(actual.common_bits() == expected.common_bits, "comparison LCP differs from original bits");
    require(actual.order() == expected.order, "comparison direction/endpoint differs from oracle");
    require(!actual.full_units() || actual.full_units() == key.size() / P::bits_per_unit,
            "known comparison length differs from the original key");
    if (!actual.order())
      require(actual.full_units() == query.size() / P::bits_per_unit, "equal comparison must know query length");
  }

  template <class P> profile_query_context<P> own_displaced_query(bit_view query, unsigned shift) {
    auto storage = displaced(query, shift);
    // Only this context survives destruction of the displaced input storage.
    return profile_query_context<P>(storage.view().subview(shift, query.size()));
  }

  template <class P> void check_encoded(fixture<P> const & input, profile_blob<P> const & encoded) {
    auto check_stream = [&](auto const & stream, auto key_at) {
      auto view = stream.view();
      bit_view previous;
      for (std::uint64_t i = 0; i != stream.size(); ++i) {
        auto key = key_at(i);
        auto record = view.encoded_at(i);
        auto retained = compare_original(previous, key).common_bits / P::bits_per_unit;
        require(record.retained == retained, "stream is not ordinary FC relative to physical predecessor");
        require(record.key_units == key.size() / P::bits_per_unit,
                "FC frame lost the full key length");
        profile_comparison_work work;
        require(view.predecessor_units(i, &work) == previous.size() / P::bits_per_unit &&
                work.skipped_headers <= P::codec_block_size && !work.visited_headers && !work.compared_bits,
                "pre-lane length seek reconstructed or compared key content");
        previous = key;
      }
      require(stream.metadata().terminal_key_units == previous.size() / P::bits_per_unit,
              "terminal full key length mismatch");
      profile_comparison_work terminal_work;
      require(view.predecessor_units(stream.size(), &terminal_work) == previous.size() / P::bits_per_unit &&
              !terminal_work.skipped_headers && !terminal_work.visited_headers && !terminal_work.compared_bits,
              "terminal predecessor seek did not use metadata");
      rejects([&] { view.predecessor_units(stream.size() + 1); });
    };
    check_stream(encoded.native(), [&](std::uint64_t i) { return input.native[i].key.view(); });
    check_stream(encoded.borrowed(), [&](std::uint64_t i) { return input.borrowed[i].view(); });
    auto catalog = merged_original(input.native, input.borrowed);
    auto groups = catalog.size() / P::group_size + (catalog.size() % P::group_size != 0);
    require(encoded.virtual_size() == catalog.size() && encoded.group_count() == groups,
            "comparison catalog count mismatch");
    require(encoded.cut_lcps().size() == groups, "one exact LCP is required per virtual group");
    std::uint64_t natives = 0, samples = 0;
    for (std::uint64_t i = 0; i != catalog.size(); ++i) {
      if (i % P::group_size == 0) {
        auto expected = samples ? compare_original(input.borrowed[samples - 1].view(),
          catalog[i].key.view()).common_bits : 0;
        require(encoded.cut_lcps()[i / P::group_size] == expected,
                "cut metadata is not the exact preceding-borrowed/boundary bit LCP");
        auto projection = encoded.project(i / P::group_size);
        require(projection.native_first == natives && projection.borrowed_first == samples,
                "cut projection disagrees with original stable merge");
      }
      if (catalog[i].borrowed) ++samples; else ++natives;
    }
    for (std::size_t i = 0; i != input.borrowed.size(); ++i) {
      bool equal_native = false;
      for (auto const & row : input.native)
        equal_native |= same_bits(row.key.view(), input.borrowed[i].view());
      require(encoded.false_borrow(i) == equal_native, "false-borrow flag disagrees with full keys");
    }
    if (catalog.empty()) {
      auto context = profile_query_context<P>(bit_view{});
      rejects([&] { encoded.search_window(0, context); });
      return;
    }

    for (std::size_t qi = 0; qi != input.queries.size(); ++qi) {
      auto query = input.queries[qi].view();
      auto context = own_displaced_query<P>(query, unsigned(qi % 8));
      check_context(context, {}, query);
      // Find the global rightmost qualifying occurrence by direct linear scan.
      // This neither samples the catalog nor calls a rank/offset operation.
      std::size_t position = 0;
      for (std::size_t i = 0; i != catalog.size(); ++i)
        if (compare_original(catalog[i].key.view(), query).order <= 0) position = i;
      auto group = position / P::group_size;
      auto const & boundary = catalog[group * P::group_size].key;
      auto lower = [&] {
        if (!group) return context;
        auto shift = unsigned((qi + 3) % 8);
        auto temporary = displaced(boundary.view(), shift);
        return context.with_key(temporary.view().subview(shift, boundary.bit_size));
      }();
      check_context(lower, group ? boundary.view() : bit_view{}, query);
      profile_comparison_work native_work, borrowed_work;
      auto result = encoded.search_window(group, lower, &native_work, &borrowed_work);
      require(native_work.skipped_headers < P::codec_block_size &&
              borrowed_work.skipped_headers < P::codec_block_size,
              "comparison replay exceeded one W block of preceding headers");
      require(native_work.visited_headers + borrowed_work.visited_headers <= P::group_size,
              "comparison candidates exceeded one K-entry virtual window");
      std::optional<std::size_t> native;
      for (std::size_t i = 0; i != input.native.size(); ++i)
        if (same_bits(input.native[i].key.view(), query)) native = i;
      require(bool(result.native) == bool(native), "FC comparison lost or invented a native match");
      if (native) {
        require(result.native->ordinal == *native, "FC comparison returned wrong native ordinal");
        require(same_bits(result.native->value.view(), input.native[*native].value.view()),
                "FC comparison returned wrong native value");
      }
      std::optional<std::size_t> predecessor;
      for (std::size_t i = 0; i != input.borrowed.size(); ++i)
        if (compare_original(input.borrowed[i].view(), query).order <= 0) predecessor = i;
      require(bool(result.borrowed_predecessor) == bool(predecessor), "FC borrowed predecessor presence");
      if (predecessor) {
        auto const & found = *result.borrowed_predecessor;
        require(found.ordinal == *predecessor && found.target_ordinal == *predecessor * P::group_size,
                "FC comparison returned wrong outgoing route");
        require(found.false_borrow == encoded.false_borrow(*predecessor), "outgoing false-borrow tag");
        check_context(found.comparison, input.borrowed[*predecessor].view(), query);
      }

      auto projected = encoded.project(group);
      std::uint64_t next_native = projected.native_first;
      encoded.native().view().compare_window(projected.native_first, projected.native_last, lower,
        [&](auto const & item) {
          require(item.ordinal == next_native, "native comparison window changed source order");
          auto const & expected = input.native[next_native++];
          check_context(item.comparison, expected.key.view(), query);
          require(same_bits(item.value, expected.value.view()), "native comparison item value");
          return true;
        });
      require(next_native == projected.native_last, "native comparison window ended early");
      std::uint64_t next_sample = projected.borrowed_first;
      encoded.borrowed().view().compare_window(projected.borrowed_first, projected.borrowed_last, lower,
        [&](auto const & item) {
          require(item.ordinal == next_sample, "borrowed comparison window changed source order");
          check_context(item.comparison, input.borrowed[next_sample++].view(), query);
          require(item.value.empty(), "borrowed comparison item acquired a value");
          return true;
        });
      require(next_sample == projected.borrowed_last, "borrowed comparison window ended early");
    }
  }

  template <class P> void check_fixture(fixture<P> const & input) {
    auto encoded = profile_blob<P>::build(input.native, input.borrowed);
    require(encoded.native().metadata().codec_block_size == P::codec_block_size &&
            encoded.borrowed().metadata().codec_block_size == P::codec_block_size,
            "codec W was silently replaced with navigation K");
    check_encoded(input, encoded);
  }

  template <class P> void matrix() {
    auto all = key_universe<P>();
    check_fixture(fixture<P>({}, {}));
    check_fixture(fixture<P>(all, {}));
    check_fixture(fixture<P>({}, all));
    std::vector<bit_string> native, borrowed;
    for (std::size_t i = 0; i != all.size(); ++i) {
      if (i % 3) native.push_back(all[i]);
      if (i % 2 == 0) {
        borrowed.push_back(all[i]);
        if (i % 11 == 0)
          for (unsigned copy = 0; copy != 2 * P::group_size + 1; ++copy) borrowed.push_back(all[i]);
      }
    }
    fixture<P> mixed(native, borrowed, all);
    check_fixture(mixed);
    auto repeated = text(std::string(133, 'q'));
    check_fixture(fixture<P>({repeated}, std::vector<bit_string>(3 * P::group_size + 5, repeated),
      {text("q"), text(std::string(133, 'q') + "x")}));

    for (unsigned padding : {0u, 97u}) {
      auto first = indistinguishable_frontier<P>('a', padding);
      auto second = indistinguishable_frontier<P>('b', padding);
      check_fixture(first);
      check_fixture(second);
      auto a = profile_blob<P>::build(first.native, first.borrowed);
      auto b = profile_blob<P>::build(second.native, second.borrowed);
      auto ar = a.borrowed().view().encoded_at(1), br = b.borrowed().view().encoded_at(1);
      require(ar.retained == br.retained && ar.key_units == br.key_units &&
              same_bits(ar.suffix, br.suffix), "ambiguous-frontier fixture lost identical local FC bytes");
      require(a.cut_lcps()[1] != b.cut_lcps()[1], "exact cut LCP failed to distinguish hidden prefixes");

      // A separate upper-frontier counterexample: aa -> ab retains prefix a,
      // but the carried cut key is c. Using c as literal-prefix storage would
      // compare cb instead of ab. Prefix padding also exceeds both tested W's.
      std::string prefix(padding, 'p');
      std::vector<bit_string> bridge_native;
      for (unsigned i = 0; i != P::group_size - 3; ++i)
        bridge_native.push_back(text(prefix + "ac/" + number(i)));
      for (auto const * key : {"b", "c", "d"}) bridge_native.push_back(text(prefix + key));
      check_fixture(fixture<P>(std::move(bridge_native),
        {text(prefix + "aa"), text(prefix + "ab"), text(prefix + "z")}, {text(prefix + "c")}));
    }

    // Change index cuts while retaining the exact native allocation and bytes.
    auto original = profile_blob<P>::build(mixed.native, mixed.borrowed);
    auto before = original.native().bytes();
    std::vector<std::byte> snapshot(before.begin(), before.end());
    fixture<P> changed(native, all, all);
    auto revised = original.reindex(changed.borrowed);
    require(&original.native() == &revised.native(), "reindex replaced immutable native allocation");
    require(std::ranges::equal(snapshot, revised.native().bytes()), "reindex changed ordinary native FC bytes");
    check_encoded(mixed, original);
    check_encoded(changed, revised);

    // A preceding borrowed ordinal at or beyond the end of a W-sized block
    // needs its length checkpoint/sentinel, without replaying the earlier key.
    for (std::uint64_t count : {std::uint64_t{1}, P::codec_block_size - 1, P::codec_block_size,
          P::codec_block_size + 1, 2 * P::codec_block_size}) {
      std::vector<bit_string> samples;
      for (unsigned i = 0; i != count; ++i) samples.push_back(text("a/" + number(i)));
      std::vector<bit_string> tail;
      for (unsigned i = 0; i != 3 * P::group_size + 1; ++i) tail.push_back(text("z/" + number(i)));
      check_fixture(fixture<P>(std::move(tail), std::move(samples)));
    }
  }

#if defined(__unix__) || defined(__APPLE__)
  struct inaccessible_interior {
    void * address = nullptr;
    std::size_t bytes = 0;
    explicit inaccessible_interior(std::span<std::byte const> storage) {
      auto result = ::sysconf(_SC_PAGESIZE);
      require(result > 0, "page size unavailable");
      auto page = std::uintptr_t(result);
      auto begin = reinterpret_cast<std::uintptr_t>(storage.data());
      // Leave the first and last pages readable for surrounding headers,
      // padding checks and the allocator; protect only wholly owned interiors.
      auto first = ((begin + page - 1) / page + 1) * page;
      auto last = ((begin + storage.size()) / page - 1) * page;
      require(last > first, "guard fixture lacks complete interior pages");
      address = reinterpret_cast<void *>(first);
      bytes = static_cast<std::size_t>(last - first);
      require(::mprotect(address, bytes, PROT_NONE) == 0, "cannot protect FC payload pages");
    }
    inaccessible_interior(inaccessible_interior const &) = delete;
    inaccessible_interior & operator=(inaccessible_interior const &) = delete;
    ~inaccessible_interior() {
      if (address && ::mprotect(address, bytes, PROT_READ | PROT_WRITE)) std::terminate();
    }
  };

  template <class P> void no_prefix_replay() {
    auto page = ::sysconf(_SC_PAGESIZE);
    require(page > 0, "page size unavailable");
    auto prefix_bytes = static_cast<std::size_t>(page) * 6;
    std::vector<profile_record> native;
    for (unsigned i = 0; i != P::group_size + 5; ++i)
      native.push_back({text(std::string(prefix_bytes, 'p') + number(i)), value_for<P>(i + 1)});
    auto encoded = profile_blob<P>::build(native);
    auto query = native[P::group_size].key.view();
    auto lower = profile_query_context<P>(query).with_key(query);
    auto expected = compare_original(query, query);
    require(lower.common_bits() == expected.common_bits, "guard fixture incoming comparison");
    // The first record's literal is inaccessible. It lies before this lane,
    // including within the same W block for K=3,W=15/16. Headers remain usable.
    inaccessible_interior guard(encoded.native().bytes().first(prefix_bytes));
    profile_comparison_work work;
    auto found = encoded.search_window(1, lower, &work);
    require(found.native && found.native->ordinal == P::group_size,
            "comparison depended on inaccessible pre-lane literal prefix");
    require(same_bits(found.native->value.view(), native[P::group_size].value.view()),
            "guarded comparison value");
    require(!found.borrowed_predecessor, "native-only guarded comparison invented a route");
    require(work.skipped_headers == P::group_size % P::codec_block_size && work.visited_headers == 1 &&
            work.compared_bits <= 16, "pre-lane work included literal comparison or ignored W");
  }

  template <class P> void no_terminal_predecessor_replay() {
    auto page = ::sysconf(_SC_PAGESIZE);
    require(page > 0, "page size unavailable");
    for (std::uint64_t count : {P::codec_block_size, 2 * P::codec_block_size}) {
      std::vector<bit_string> borrowed;
      for (unsigned i = 0; i != count; ++i)
        borrowed.push_back(text(number(i) + std::string(static_cast<std::size_t>(page) * 6, 'p')));
      std::vector<profile_record> native;
      for (unsigned i = 0; i != 3 * P::group_size; ++i)
        native.push_back({text("zz/" + number(i)), value_for<P>(i + 1)});
      auto encoded = profile_blob<P>::build(native, borrowed);
      auto query = native.back().key.view();
      auto catalog = merged_original(native, borrowed);
      auto group = (catalog.size() - 1) / P::group_size;
      require(encoded.project(group).borrowed_first == count &&
              encoded.project(group).borrowed_last == count, "guarded borrowed projection is not empty");
      auto lower = profile_query_context<P>(query).with_key(catalog[group * P::group_size].key.view());
      auto expected = compare_original(borrowed.back().view(), query);
      // With the borrowed stream exhausted at an exact W boundary, even the
      // previous block's header is inaccessible. Exact cut metadata repairs
      // the comparison without consulting the preceding key's full length.
      inaccessible_interior guard(encoded.borrowed().bytes());
      auto found = encoded.search_window(group, lower);
      require(found.native && found.native->ordinal + 1 == native.size(), "guarded terminal native match");
      require(found.borrowed_predecessor && found.borrowed_predecessor->ordinal + 1 == count,
              "guarded terminal borrowed route");
      auto const & state = found.borrowed_predecessor->comparison;
      require(state.common_bits() == expected.common_bits && state.order() == expected.order &&
              !state.full_units(),
              "terminal frontier comparison required inaccessible earlier block");
    }
  }

  template <class P> void no_block_predecessor_replay() {
    auto page = ::sysconf(_SC_PAGESIZE);
    require(page > 0, "page size unavailable");
    auto width = P::codec_block_size;
    std::vector<bit_string> borrowed;
    for (unsigned i = 0; i != 2 * width; ++i)
      borrowed.push_back(text(number(2 * i) + std::string(static_cast<std::size_t>(page) * 6, 'p')));
    auto before_cut = (P::group_size - width % P::group_size) % P::group_size;
    std::vector<profile_record> native;
    for (unsigned i = 0; i <= before_cut; ++i)
      native.push_back({text(number(2 * width - 1) + number(i)), value_for<P>(i + 1)});
    auto encoded = profile_blob<P>::build(native, borrowed);
    auto query = native.back().key.view();
    auto group = (width + before_cut) / P::group_size;
    auto window = encoded.project(group);
    require(window.borrowed_first == width && window.borrowed_last > width,
            "nonterminal frontier fixture did not enter a new borrowed block");
    auto expected = compare_original(borrowed[width - 1].view(), query);
    auto lower = profile_query_context<P>(query).with_key(query);
    auto block_bits = encoded.borrowed().view().block_offset(1) * P::bits_per_unit;
    // The preceding block's later headers are inaccessible, as well as its
    // inherited literals. The next block's absolute prefix is sufficient.
    inaccessible_interior guard(encoded.borrowed().bytes().first((block_bits + 7) >> 3));
    profile_comparison_work work;
    auto found = encoded.search_window(group, lower, nullptr, &work);
    require(found.native && found.native->ordinal == before_cut, "nonterminal guarded native match");
    require(found.borrowed_predecessor && found.borrowed_predecessor->ordinal + 1 == width,
            "nonterminal guarded borrowed frontier");
    auto const & state = found.borrowed_predecessor->comparison;
    require(state.common_bits() == expected.common_bits && state.order() == expected.order && !state.full_units(),
            "nonterminal borrowed comparison needed a predecessor length");
    require(!work.skipped_headers && work.visited_headers == 1,
            "nonterminal repair replayed the preceding block");
  }
#endif
}

int main() {
  try {
    matrix<storage_policy<profile_unit::byte, variable_values, 3, exponential_golomb<0>, 15>>();
    matrix<storage_policy<profile_unit::byte, variable_values, 3, exponential_golomb<0>, 16>>();
    matrix<storage_policy<profile_unit::bit, fixed_values<0>, 3, exponential_golomb<0>, 15>>();
    matrix<storage_policy<profile_unit::byte, fixed_values<0>, 7, exponential_golomb<0>, 16>>();
    matrix<storage_policy<profile_unit::bit, variable_values, 7, exponential_golomb<0>, 15>>();
    matrix<storage_policy<profile_unit::byte, variable_values, 15, exponential_golomb<0>, 16>>();
    matrix<storage_policy<profile_unit::bit, fixed_values<0>, 15, exponential_golomb<0>, 15>>();
    matrix<storage_policy<profile_unit::byte, fixed_values<0>, 31, exponential_golomb<0>, 15>>();
    matrix<storage_policy<profile_unit::bit, variable_values, 31, exponential_golomb<0>, 16>>();
#if defined(__unix__) || defined(__APPLE__)
    no_prefix_replay<storage_policy<profile_unit::byte, variable_values, 3, exponential_golomb<0>, 15>>();
    no_prefix_replay<storage_policy<profile_unit::bit, fixed_values<0>, 3, exponential_golomb<0>, 16>>();
    no_terminal_predecessor_replay<storage_policy<profile_unit::byte, fixed_values<0>, 7, exponential_golomb<0>, 16>>();
    no_terminal_predecessor_replay<storage_policy<profile_unit::bit, variable_values, 31, exponential_golomb<0>, 15>>();
    no_block_predecessor_replay<storage_policy<profile_unit::byte, fixed_values<0>, 7, exponential_golomb<0>, 16>>();
    no_block_predecessor_replay<storage_policy<profile_unit::bit, variable_values, 31, exponential_golomb<0>, 15>>();
#endif
    std::cout << "comparison FC tests passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
