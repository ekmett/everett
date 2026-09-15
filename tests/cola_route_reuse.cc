/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/cola_index.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
  using namespace everett;
  using keys = std::vector<std::string>; // Original logical bits, independent of codec/navigation.

  void require(bool ok, char const * message) {
    if (!ok) throw std::runtime_error(message);
  }
  std::size_t common(std::string const & a, std::string const & b) {
    std::size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    return i;
  }
  template <class A, class B> bool equal(A const & a, B const & b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end());
  }
  template <class P> keys fixture(bool high) {
    keys result;
    std::string prefix(P::unit == profile_unit::byte ? 152 : 153, '0');
    prefix[0] = high ? '1' : '0';
    // Empty and proper-prefix endpoints, followed by long common prefixes.
    if (!high) result.push_back("");
    else result.push_back(std::string(P::bits_per_unit, '1'));
    result.push_back(prefix.substr(0, prefix.size() - P::bits_per_unit));
    result.push_back(prefix);
    auto count = 2 * P::codec_block_size * P::group_size + 11;
    for (unsigned i = 0; i < count; ++i) {
      auto key = prefix;
      for (unsigned b = 16; b; --b) key += char('0' + ((i >> (b - 1)) & 1));
      if constexpr (P::unit == profile_unit::bit) key += "101";
      result.push_back(std::move(key));
    }
    std::sort(result.begin(), result.end());
    require(std::adjacent_find(result.begin(), result.end()) == result.end(), "duplicate native fixture");
    return result;
  }
  std::vector<profile_record> rows(keys const & input) {
    std::vector<profile_record> result;
    for (auto const & key : input) result.push_back({bit_string::from_bits(key), {}});
    return result;
  }
  template <class P> keys sampled(keys const & input) {
    keys result;
    for (std::size_t i = 0; i < input.size(); i += P::group_size) result.push_back(input[i]);
    return result;
  }
  template <class P> struct target {
    typename cola_index<P>::pair_type pair;
    keys catalog;
  };
  template <class P> target<P> make_target(keys const & input) {
    auto records = rows(input);
    auto leaf = std::make_shared<cola_index<P> const>(cola_index<P>::build(records));
    auto native = std::make_shared<profile_array<P> const>(profile_array<P>::build(records));
    auto pair = std::make_shared<cola_index<P> const>(cola_index<P>::build(records, leaf, native));
    auto catalog = input;
    auto samples = sampled<P>(input);
    catalog.insert(catalog.end(), samples.begin(), samples.end());
    catalog.insert(catalog.end(), samples.begin(), samples.end());
    std::sort(catalog.begin(), catalog.end());
    require(std::adjacent_find(catalog.begin(), catalog.end()) != catalog.end(), "target lacks equal occurrences");
    require(pair->virtual_size() == catalog.size(), "target catalog size oracle");
    return {std::move(pair), std::move(catalog)};
  }

  // Deliberately scalar MSB-first encoder: no production count/LCP/FC/EF helpers.
  struct wire {
    std::vector<std::byte> bytes;
    std::uint64_t bits = 0;
    std::vector<std::uint64_t> offsets;
    void bit(bool one) {
      if (!(bits & 7)) bytes.push_back(std::byte{0});
      if (one) bytes.back() |= std::byte(1u << (7 - (bits & 7)));
      ++bits;
    }
    void fixed(std::uint64_t value, unsigned width) {
      while (width) bit((value >> --width) & 1);
    }
    template <class P> void count(std::uint64_t value) {
      if constexpr (P::unit == profile_unit::byte) {
        do { auto byte = value & 127; value >>= 7; fixed(byte | (value ? 128 : 0), 8); } while (value);
      } else {
        auto code = value + 1;
        unsigned width = 0;
        for (auto rest = code; rest; rest >>= 1) ++width;
        for (unsigned i = 1; i < width; ++i) bit(false);
        fixed(code, width);
      }
    }
  };
  template <class P> wire expected_wire(keys const & input) {
    wire result;
    std::string previous;
    for (std::size_t i = 0; i < input.size(); ++i) {
      auto const & key = input[i];
      auto retained = common(previous, key) >> P::unit_shift;
      if (i % P::codec_block_size == 0) {
        result.offsets.push_back(result.bits >> P::unit_shift);
        result.template count<P>(retained);
      } else result.template count<P>((previous.size() >> P::unit_shift) - retained);
      result.template count<P>((key.size() >> P::unit_shift) - retained);
      for (auto bit = retained << P::unit_shift; bit < key.size(); ++bit) result.bit(key[bit] == '1');
      previous = key;
    }
    result.offsets.push_back(result.bits >> P::unit_shift);
    return result;
  }
  void check_ef(elias_fano const & actual, std::vector<std::uint64_t> const & offsets) {
    auto universe = offsets.back();
    auto quotient = universe / offsets.size();
    unsigned width = 0;
    while (quotient > 1) { quotient >>= 1; ++width; }
    std::vector<std::uint64_t> low((offsets.size() * width + 63) >> 6);
    std::vector<std::uint64_t> high(((universe >> width) + offsets.size() + 63) >> 6);
    std::vector<std::uint64_t> positions;
    for (std::size_t i = 0; i < offsets.size(); ++i) {
      for (unsigned b = 0; b < width; ++b)
        if ((offsets[i] >> b) & 1) low[(i * width + b) >> 6] |= std::uint64_t{1} << ((i * width + b) & 63);
      auto p = (offsets[i] >> width) + i;
      high[p >> 6] |= std::uint64_t{1} << (p & 63);
      positions.push_back(p);
    }
    std::vector<std::uint64_t> sparse;
    require(actual.entry_count == offsets.size() && actual.universe == universe && actual.low_width == width &&
            actual.low == low && actual.high == high, "borrowed EF scalar packing oracle");
    require(actual.samples.size() == (offsets.size() + 255) / 256, "borrowed EF sample count oracle");
    for (std::size_t begin = 0; begin < offsets.size(); begin += 256) {
      auto end = std::min(begin + 256, offsets.size());
      auto start = std::numeric_limits<std::uint64_t>::max();
      if (positions[end - 1] - positions[begin] >= 4096) {
        start = sparse.size();
        sparse.insert(sparse.end(), positions.begin() + begin, positions.begin() + end);
      }
      auto sample = actual.samples[begin / 256];
      require(sample.first == positions[begin] && sample.sparse == start, "borrowed EF sample oracle");
    }
    require(actual.sparse == sparse, "borrowed EF sparse oracle");
    auto view = actual.view();
    for (std::size_t i = 0; i < offsets.size(); ++i)
      require(view.select(i) == offsets[i], "borrowed EF offset including EOF oracle");
  }
  template <class P> void check_stream(cola_index<P> const & node, unsigned route, keys const & samples) {
    auto const & stream = node.borrowed(route);
    auto expected = expected_wire<P>(samples);
    require(equal(stream.bytes(), expected.bytes), "unchanged route FC differs from scalar bit oracle");
    require(stream.metadata().extent == (expected.bits >> P::unit_shift) && stream.size() == samples.size() &&
            stream.metadata().terminal_key_units == (samples.back().size() >> P::unit_shift), "borrowed extent/count/EOF oracle");
    check_ef(stream.group_offsets(), expected.offsets);
    auto cursor = stream.view().cursor();
    for (auto const & key : samples) {
      require(!cursor.done(), "short borrowed cursor");
      auto found = cursor.peek().key.prefix;
      require(found.size() == key.size(), "borrowed key length oracle");
      for (std::size_t i = 0; i < key.size(); ++i) {
        auto position = found.offset() + i;
        auto one = (std::to_integer<unsigned>(found.storage()[position >> 3]) >> (7 - (position & 7))) & 1;
        require(one == unsigned(key[i] == '1'), "borrowed original logical-bit oracle");
      }
      cursor.advance();
    }
    require(cursor.done(), "trailing borrowed cursor");
  }
  template <class P> void same_stream(cola_index<P> const & a, cola_index<P> const & b, unsigned route) {
    auto const & x = a.borrowed(route);
    auto const & y = b.borrowed(route);
    require(equal(x.bytes(), y.bytes()), "other inputs changed retained route FC bytes");
    auto const & ef = x.group_offsets();
    auto const & other = y.group_offsets();
    require(ef.entry_count == other.entry_count && ef.universe == other.universe && ef.low_width == other.low_width &&
            ef.low == other.low && ef.high == other.high && ef.sparse == other.sparse &&
            ef.samples.size() == other.samples.size(), "other inputs changed retained route EF arrays");
    for (std::size_t i = 0; i < ef.samples.size(); ++i)
      require(ef.samples[i].first == other.samples[i].first && ef.samples[i].sparse == other.samples[i].sparse,
              "other inputs changed retained route EF samples");
    require(x.metadata().extent == y.metadata().extent && x.metadata().terminal_key_units == y.metadata().terminal_key_units,
            "other inputs changed retained route stream metadata");
  }
  template <class P> void exercise() {
    auto low = fixture<P>(false), high = fixture<P>(true);
    auto main = make_target<P>(low), replacement_main = make_target<P>(high);
    auto low_rows = rows(low), high_rows = rows(high);
    auto secondary = std::make_shared<profile_array<P> const>(profile_array<P>::build(low_rows));
    auto replacement_secondary = std::make_shared<profile_array<P> const>(profile_array<P>::build(high_rows));
    for (unsigned route = 0; route != 2; ++route) {
      auto samples = route == 0 ? sampled<P>(main.catalog) : sampled<P>(low);
      require(samples.size() > 2 * P::codec_block_size, "fixture must cross physical blocks");
      auto baseline = cola_index<P>::build(low_rows, main.pair, secondary);
      check_stream(baseline, route, samples);
      for (unsigned changes = 1; changes != 4; ++changes) {
        auto changed = cola_index<P>::build(changes & 1 ? high_rows : low_rows,
          route == 1 && (changes & 2) ? replacement_main.pair : main.pair,
          route == 0 && (changes & 2) ? replacement_secondary : secondary);
        require(route == 0 ? changed.main_target() == baseline.main_target() :
                             changed.secondary_target() == baseline.secondary_target(), "retained exact target identity");
        check_stream(changed, route, samples);
        same_stream(baseline, changed, route);
        require(baseline.interleave(route).classes != changed.interleave(route).classes,
                "fixture did not change retained route rank classes");
        require(!equal(baseline.cut_lcps(route), changed.cut_lcps(route)), "fixture did not change retained route cut LCPs");
        if (changes & 1)
          require(!equal(baseline.false_borrow_bits(route), changed.false_borrow_bits(route)),
                  "fixture did not change false-borrow flags with local equality");
        else require(equal(baseline.false_borrow_bits(route), changed.false_borrow_bits(route)),
                     "other target changed native equality flags");
      }
    }
  }
}

int main() {
  try {
    exercise<storage_policy<profile_unit::byte, fixed_values<0>, 3, exponential_golomb<0>, 16>>();
    exercise<storage_policy<profile_unit::bit, fixed_values<0>, 3, exponential_golomb<0>, 16>>();
    exercise<storage_policy<profile_unit::byte, fixed_values<0>, 15, exponential_golomb<0>, 16>>();
    exercise<storage_policy<profile_unit::bit, fixed_values<0>, 15, exponential_golomb<0>, 16>>();
    std::cout << "COLA route reuse tests passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks target-local FC/EF stability against independent bit and directory oracles.
 */
