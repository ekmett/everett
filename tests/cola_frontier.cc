/**
 * \file
 * \license
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/cola_index.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {
  using namespace diet;
  using lanes = std::array<std::vector<std::string>, 3>;
  void check(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void ended(F && f) {
    bool caught = false;
    try { f(); } catch (std::out_of_range const &) { caught = true; }
    check(caught, "ended sample cursor accepted operation");
  }
  std::string original_bits(bit_view view) {
    std::string result;
    for (std::uint64_t i = 0; i != view.size(); ++i) {
      auto at = view.offset() + i;
      result += (std::to_integer<unsigned>(view.storage()[at / 8]) >> (7 - at % 8)) & 1 ? '1' : '0';
    }
    return result;
  }
  std::uint64_t lcp(std::string const & a, std::string const & b) {
    std::size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    return i;
  }
  struct occurrence {
    std::string key;
    unsigned origin;
    std::uint64_t ordinal;
  };
  struct coverage {
    std::array<std::array<unsigned, 3>, 3> nonminimal{};
    unsigned equal = 0, proper_prefix = 0, interior_lcp = 0, partial_bits = 0, short_tail = 0;
  };
  std::vector<occurrence> catalog(lanes const & source) {
    std::vector<occurrence> result;
    for (unsigned origin = 0; origin != 3; ++origin)
      for (std::size_t i = 0; i != source[origin].size(); ++i)
        result.push_back({source[origin][i], origin, i});
    // Independent complete-key sort, with explicit stable-origin/ordinal ties.
    // No production comparison, frontier, rank or sampling operation is used.
    std::sort(result.begin(), result.end(), [](auto const & a, auto const & b) {
      return std::tie(a.key, a.origin, a.ordinal) < std::tie(b.key, b.origin, b.ordinal);
    });
    return result;
  }
  template <class P, stream_role Role> profile_array<P, Role> encode(
      std::vector<std::string> const & keys, unsigned mode, unsigned origin, coverage & seen) {
    std::vector<profile_record> rows;
    std::vector<std::uint64_t> ceilings;
    for (std::size_t i = 0; i != keys.size(); ++i) {
      auto value_bits = Role == stream_role::native ? (i % 5) * P::bits_per_unit : 0;
      rows.push_back({bit_string::from_bits(keys[i]), bit_string::from_bits(std::string(value_bits, '1'))});
      if (mode == 1) ceilings.push_back(i % 3 == 0 ? 0 : i % 3);
    }
    auto result = profile_array<P, Role>::build(rows, ceilings, mode == 2 ? 3 : 0);
    for (std::size_t i = 0; i != keys.size(); ++i) {
      auto actual = result.view().encoded_at(i);
      auto maximal = i ? lcp(keys[i - 1], keys[i]) / P::bits_per_unit : 0;
      check(actual.retained <= maximal, "fixture retained beyond original predecessor LCP");
      if (!mode) check(actual.retained == maximal, "ordinary fixture is not maximal FC");
      if (actual.retained < maximal) {
        ++seen.nonminimal[mode][origin];
        if (mode == 2) check(actual.retained == 0, "restart fixture changed a nonzero retained prefix");
      }
    }
    return result;
  }

  // A locally valid COLA view permits deliberately redundant FC in all three
  // physical streams. It does not claim that these arbitrary duplicate runs
  // arise from one particular recursive downstream sampling graph.
  template <class P> struct target {
    using policy_type = P;
    profile_array<P> native;
    std::array<profile_array<P, stream_role::borrowed>, 2> borrowed;
    std::array<rank_groups<P::group_size>, 2> ranks;
    std::array<std::vector<std::byte>, 2> flags;
    std::array<std::vector<std::uint64_t>, 2> cuts;
    std::uint64_t count;

    target(lanes const & source, std::vector<occurrence> const & ordered, unsigned mode, coverage & seen)
      : native(encode<P, stream_role::native>(source[0], mode, 0, seen)),
        borrowed{encode<P, stream_role::borrowed>(source[1], mode, 1, seen),
                 encode<P, stream_role::borrowed>(source[2], mode, 2, seen)}, count(ordered.size()) {
      auto groups = count / P::group_size + (count % P::group_size != 0);
      std::array<std::vector<std::uint64_t>, 2> populations;
      std::array<std::optional<std::string>, 2> previous;
      for (unsigned route = 0; route != 2; ++route) {
        populations[route].resize(groups);
        flags[route].resize((source[route + 1].size() + 7) / 8);
        for (std::size_t i = 0; i != source[route + 1].size(); ++i)
          if (std::binary_search(source[0].begin(), source[0].end(), source[route + 1][i]))
            flags[route][i / 8] |= std::byte(1u << (i % 8));
      }
      for (std::size_t i = 0; i != ordered.size(); ++i) {
        auto const & item = ordered[i];
        if (i % P::group_size == 0)
          for (unsigned route = 0; route != 2; ++route)
            cuts[route].push_back(previous[route] ? lcp(*previous[route], item.key) : 0);
        if (item.origin) {
          auto route = item.origin - 1;
          ++populations[route][i / P::group_size];
          previous[route] = item.key;
        }
      }
      for (unsigned route = 0; route != 2; ++route) ranks[route] = rank_groups<P::group_size>::build(populations[route], count);
    }
    cola_index_view<P> view() const {
      return {native.view(), {borrowed[0].view(), borrowed[1].view()}, {ranks[0].view(), ranks[1].view()},
        {flags[0], flags[1]}, {word_view(std::span<std::uint64_t const>(cuts[0])),
          word_view(std::span<std::uint64_t const>(cuts[1]))}, count};
    }
  };
  template <class P> void check_sample(cola_sample_view<P> actual, occurrence const & expected, std::size_t ordinal) {
    check(actual.target_ordinal == ordinal && unsigned(actual.origin) == expected.origin &&
          actual.source_ordinal == expected.ordinal && original_bits(actual.key) == expected.key,
          "sample differs from original three-way ordering");
  }
  template <class P> void run(lanes source, unsigned mode, coverage & seen) {
    for (auto & lane : source) std::sort(lane.begin(), lane.end());
    check(std::adjacent_find(source[0].begin(), source[0].end()) == source[0].end(), "native fixture is not unique");
    auto ordered = catalog(source);
    auto owner = std::make_shared<target<P> const>(source, ordered, mode, seen);
    cola_sample_cursor<P, target<P>> compared(owner), plain(owner);
    check(compared.target() == owner && plain.target() == owner, "sample target identity");
    if (ordered.empty()) check(compared.done() && plain.done(), "empty target not exhausted");
    for (std::size_t at = 0; at < ordered.size(); at += P::group_size) {
      check(!compared.done() && !plain.done(), "sample cursor exhausted early");
      check_sample<P>(compared.peek(), ordered[at], at);
      check_sample<P>(plain.peek(), ordered[at], at);
      auto next = at + std::min<std::size_t>(P::group_size, ordered.size() - at);
      auto result = compared.advance_comparison();
      plain.advance();
      if (next == ordered.size()) {
        check(!result && compared.done() && plain.done(), "EOF comparison needs no successor");
        if (next - at < P::group_size) ++seen.short_tail;
      } else {
        auto const & a = ordered[at].key;
        auto const & b = ordered[next].key;
        auto common = lcp(a, b);
        int order = a < b ? -1 : a > b ? 1 : 0;
        check(result && result->common_bits == common && result->order == order,
              "sampled endpoint comparison differs from original-bit LCP oracle");
        check_sample<P>(compared.peek(), ordered[next], next);
        check_sample<P>(plain.peek(), ordered[next], next);
        if (!order) ++seen.equal;
        else if (common == a.size()) ++seen.proper_prefix;
        else if (common && common < std::min(a.size(), b.size())) ++seen.interior_lcp;
        if ((a.size() % 8) || (b.size() % 8)) ++seen.partial_bits;
      }
    }
    ended([&] { (void)compared.peek(); });
    ended([&] { (void)compared.advance_comparison(); });
    ended([&] { plain.advance(); });
  }
  std::string binary(unsigned value, unsigned width) {
    std::string out;
    for (unsigned i = width; i; --i) out += (value >> (i - 1)) & 1 ? '1' : '0';
    return out;
  }
  template <class P> void policy() {
    coverage seen;
    std::vector<std::string> keys{"", std::string(P::bits_per_unit, '0'),
      std::string(2 * P::bits_per_unit, '0'), std::string(P::bits_per_unit, '1')};
    if constexpr (P::unit == profile_unit::bit)
      for (auto suffix : {"001", "01", "101", "11101"}) keys.emplace_back(suffix);
    auto prefix = std::string(32 * 8, '0');
    keys.push_back(prefix);
    for (unsigned i = 0; i != 128; ++i) {
      auto key = prefix + binary(i, 8);
      if constexpr (P::unit == profile_unit::bit) key += binary(i & 7, 1 + i % 7);
      keys.push_back(std::move(key));
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    auto repeated = keys[keys.size() / 3];
    for (unsigned mode = 0; mode != 3; ++mode) {
      lanes rich{keys, keys, keys};
      for (unsigned i = 0; i != 2 * P::group_size + 3; ++i) rich[1].push_back(repeated);
      for (unsigned i = 0; i != P::group_size + 4; ++i) rich[2].push_back(repeated);
      run<P>(std::move(rich), mode, seen);
    }
    for (unsigned origin = 0; origin != 3; ++origin) {
      check(seen.nonminimal[1][origin] != 0, "ceiling fixture did not make this stream redundant");
      check(seen.nonminimal[2][origin] != 0, "restart fixture did not restart this stream");
    }
    // Every combination of empty streams, including equal empty strings
    // spanning several K cuts when only borrowed streams remain.
    for (unsigned mask = 0; mask != 8; ++mask) {
      lanes source;
      if (mask & 1) source[0] = {"", std::string(P::bits_per_unit, '0'), std::string(P::bits_per_unit, '1')};
      if (mask & 2) source[1] = std::vector<std::string>(2 * P::group_size + 2, "");
      if (mask & 4) source[2] = std::vector<std::string>(P::group_size + 1, "");
      run<P>(std::move(source), mask % 3, seen);
    }
    // Exact, partial and one-past-K tails, with interleaved unique endpoints.
    for (auto count : {std::uint64_t{1}, P::group_size - 1, P::group_size,
                      P::group_size + 1, 2 * P::group_size - 1, 2 * P::group_size, 2 * P::group_size + 1}) {
      lanes source;
      for (unsigned i = 0; i != count; ++i) source[i % 3].push_back(binary(i, 16));
      run<P>(std::move(source), unsigned(count % 3), seen);
    }
    check(seen.equal && seen.proper_prefix && seen.interior_lcp && seen.short_tail,
          "endpoint fixture missed equality, prefix, internal mismatch or short tail");
    if constexpr (P::unit == profile_unit::bit) check(seen.partial_bits, "bit fixture missed partial-byte endpoints");
  }
}

int main() try {
  policy<storage_policy<profile_unit::byte, variable_values, 3, exponential_golomb<0>, 16>>();
  policy<storage_policy<profile_unit::bit, variable_values, 3, golomb<3>, 15>>();
  policy<storage_policy<profile_unit::byte, variable_values, 7, exponential_golomb<0>, 15>>();
  policy<storage_policy<profile_unit::bit, variable_values, 7, exponential_golomb<1>, 16>>();
  policy<storage_policy<profile_unit::byte, variable_values, 15, exponential_golomb<0>, 16>>();
  policy<storage_policy<profile_unit::bit, variable_values, 15, exponential_golomb<0>, 16>>();
  policy<storage_policy<profile_unit::byte, variable_values, 31, exponential_golomb<0>, 15>>();
  policy<storage_policy<profile_unit::bit, variable_values, 31, exponential_golomb<0>, 16>>();
  std::cout << "COLA sampled endpoint frontier tests passed\n";
} catch (std::exception const & error) {
  std::cerr << error.what() << '\n';
  return 1;
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks sampled endpoint comparisons using original keys and redundant physical FC streams.
 */
