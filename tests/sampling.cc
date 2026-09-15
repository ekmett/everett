/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/sampling.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
  using namespace everett;

  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }

  template <class F> void rejects(F && f) {
    bool rejected = false;
    try { f(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "invalid sampler operation accepted");
  }

  bool bit_at(bit_string const & key, std::uint64_t i) {
    return (std::to_integer<unsigned>(key.bytes[static_cast<std::size_t>(i / 8)]) >> (7 - i % 8)) & 1;
  }

  int key_order(bit_string const & a, bit_string const & b) {
    auto count = std::min(a.bit_size, b.bit_size);
    for (std::uint64_t i = 0; i != count; ++i) {
      auto x = bit_at(a, i), y = bit_at(b, i);
      if (x != y) return x ? 1 : -1;
    }
    return a.bit_size == b.bit_size ? 0 : (a.bit_size < b.bit_size ? -1 : 1);
  }

  void check_key(bit_view actual, bit_string const & expected) {
    require(actual.size() == expected.bit_size, "sample key length differs from oracle");
    for (std::uint64_t i = 0; i != actual.size(); ++i)
      require(actual.at(i) == bit_at(expected, i), "sample key differs from oracle");
  }

  struct occurrence {
    bit_string key;
    stream_role role;
    std::uint64_t source_ordinal;
  };

  std::vector<occurrence> flatten(std::vector<profile_record> const & native,
                                  std::vector<bit_string> const & borrowed) {
    std::vector<occurrence> result;
    for (std::size_t i = 0; i != native.size(); ++i)
      result.push_back({native[i].key, stream_role::native, i});
    for (std::size_t i = 0; i != borrowed.size(); ++i)
      result.push_back({borrowed[i], stream_role::borrowed, i});
    std::stable_sort(result.begin(), result.end(), [](auto const & a, auto const & b) {
      auto order = key_order(a.key, b.key);
      return order ? order < 0 : (a.role == stream_role::native && b.role == stream_role::borrowed);
    });
    return result;
  }

  template <class P> bit_string value_for(unsigned n) {
    auto units = P::value_width ? *P::value_width : n % 9;
    if constexpr (P::unit == profile_unit::byte)
      return bit_string::from_bytes(std::string(static_cast<std::size_t>(units), char(n)));
    else return bit_string::from_bits(std::string(static_cast<std::size_t>(units), n % 2 ? '1' : '0'));
  }

  template <class P> std::vector<bit_string> keys_for(unsigned count, unsigned prefix_length = 12) {
    std::vector<bit_string> result{bit_string{}, bit_string::from_bytes(std::string(1, '\0'))};
    for (unsigned n = 0; n != count; ++n) {
      if constexpr (P::unit == profile_unit::byte) {
        auto number = std::to_string(n);
        result.push_back(bit_string::from_bytes(std::string(prefix_length, 'a')
          + std::string(6 - number.size(), '0') + number));
      } else {
        std::string bits(prefix_length, '1');
        for (unsigned i = 0; i != 16; ++i) bits.push_back((n >> (15 - i)) & 1 ? '1' : '0');
        bits.append(n % 5, '0');
        result.push_back(bit_string::from_bits(bits));
      }
    }
    std::sort(result.begin(), result.end(), [](auto const & a, auto const & b) { return key_order(a, b) < 0; });
    return result;
  }

  template <class P> void check_fixture(std::vector<profile_record> const & native,
                                      std::vector<bit_string> const & borrowed,
                                      profile_borrowed_policy policy = profile_borrowed_policy::shared_boundaries) {
    auto expected = flatten(native, borrowed);
    auto source = std::make_shared<profile_blob<P> const>(profile_blob<P>::build(native, borrowed, 18, {}, policy));
    std::weak_ptr<profile_blob<P> const> weak = source;
    auto native_bytes = source->native().bytes();
    auto borrowed_bytes = source->borrowed().bytes();
    std::vector<std::byte> native_copy(native_bytes.begin(), native_bytes.end());
    std::vector<std::byte> borrowed_copy(borrowed_bytes.begin(), borrowed_bytes.end());
    auto original = source.get();
    auto cursor = std::make_unique<sample_cursor<P>>(source);
    source.reset();
    require(!weak.expired() && cursor->target().get() == original, "sampler must pin the exact source pair");
    require(cursor->counters().decoded_entries == std::uint64_t(!native.empty()) + std::uint64_t(!borrowed.empty()),
            "sampler constructor decodes at most one record per stream");

    std::uint64_t at = 0, native_at = 0, borrowed_at = 0;
    while (!cursor->done()) {
      require(at < expected.size(), "sampler emitted an extra sample");
      auto const & entry = expected[static_cast<std::size_t>(at)];
      auto sample = cursor->peek();
      check_key(sample.key, entry.key);
      require(sample.target_ordinal == at && sample.source_role == entry.role
              && sample.source_ordinal == entry.source_ordinal, "sample tagged occurrence differs from oracle");
      auto before = cursor->counters();
      for (unsigned stall = 0; stall != 3; ++stall) {
        auto again = cursor->peek();
        require(again.key.storage().data() == sample.key.storage().data()
                && again.target_ordinal == at, "stalled peek changed key storage or position");
        check_key(again.key, entry.key);
      }
      require(cursor->counters().decoded_entries == before.decoded_entries
              && cursor->counters().key_comparisons == before.key_comparisons,
              "stalled peek must not repeat decoding or comparisons");

      // Destroy the moved-from object before using the destination, so member
      // pointers accidentally retained across the move cannot pass this check.
      auto moved = std::make_unique<sample_cursor<P>>(std::move(*cursor));
      cursor.reset();
      cursor = std::move(moved);
      check_key(cursor->peek().key, entry.key);
      require(!weak.expired(), "moving a sampler lost its source pin");

      auto next = std::min<std::uint64_t>(expected.size(), at + P::group_size);
      for (auto i = at; i != next; ++i) {
        if (expected[static_cast<std::size_t>(i)].role == stream_role::native) ++native_at;
        else ++borrowed_at;
      }
      cursor->advance();
      auto after = cursor->counters();
      require(after.consumed_entries() - before.consumed_entries() == next - at,
              "advance must consume exactly one bounded group or its final tail");
      require(after.native_entries == native_at && after.borrowed_entries == borrowed_at,
              "source-consumption counts differ from the independent merge oracle");
      require(after.decoded_entries == next + std::uint64_t(native_at != native.size())
              + std::uint64_t(borrowed_at != borrowed.size()), "each source entry must decode exactly once");
      require(after.key_comparisons - before.key_comparisons <= next - at,
              "advance repeated a merged-order comparison");
      at = next;
    }
    require(at == expected.size() && cursor->counters().decoded_entries == expected.size(),
            "complete sampler must visit and decode every occurrence once");
    require(cursor->counters().key_comparisons <= expected.size(), "too many full-key comparisons");
    rejects([&] { (void)cursor->peek(); });
    rejects([&] { cursor->advance(); });
    require(cursor->target()->native().bytes().data() == native_bytes.data()
            && cursor->target()->borrowed().bytes().data() == borrowed_bytes.data(),
            "sampling replaced a source allocation");
    require(std::equal(native_copy.begin(), native_copy.end(), native_bytes.begin())
            && std::equal(borrowed_copy.begin(), borrowed_copy.end(), borrowed_bytes.begin()),
            "sampling modified source bytes");
    cursor.reset();
    require(weak.expired(), "sampler source pin leaked after destruction");
  }

  template <class P> void check_policy() {
    rejects([] { sample_cursor<P> cursor(nullptr); });
    check_fixture<P>({}, {});
    auto keys = keys_for<P>(unsigned(P::group_size * 2 + 11));

    // Every possible short tail, including empty native and borrowed streams.
    for (std::size_t length = 1; length <= P::group_size * 2; ++length) {
      std::vector<profile_record> native;
      std::vector<bit_string> borrowed;
      for (std::size_t i = 0; i != length; ++i) {
        native.push_back({keys[i], value_for<P>(unsigned(i))});
        borrowed.push_back(keys[i]);
      }
      check_fixture<P>(native, {});
      check_fixture<P>({}, borrowed);
    }

    // Runs of equal borrowed keys cross group cuts, including native ties.
    std::vector<profile_record> native;
    std::vector<bit_string> borrowed;
    for (std::size_t i = 0; i != keys.size(); ++i) {
      if (i % 3 != 1) native.push_back({keys[i], value_for<P>(unsigned(i))});
      for (std::size_t copy = 0; copy != (i % 5 == 0 ? P::group_size + 2 : i % 3); ++copy)
        borrowed.push_back(keys[i]);
    }
    check_fixture<P>(native, borrowed);
    check_fixture<P>(native, borrowed, profile_borrowed_policy::ordinary);
    check_fixture<P>(native, borrowed, profile_borrowed_policy::bidirectional);

    std::mt19937_64 random(0xd1479a45 + P::group_size);
    for (unsigned trial = 0; trial != 8; ++trial) {
      native.clear(); borrowed.clear();
      for (std::size_t i = 0; i != keys.size(); ++i) {
        if (random() % 3) native.push_back({keys[i], value_for<P>(unsigned(i))});
        auto copies = random() % 4;
        for (std::uint64_t copy = 0; copy != copies; ++copy) borrowed.push_back(keys[i]);
      }
      check_fixture<P>(native, borrowed);
    }

    auto long_keys = keys_for<P>(70, 1025);
    native.clear(); borrowed.clear();
    for (std::size_t i = 0; i != long_keys.size(); ++i) {
      if (i % 2 == 0) native.push_back({long_keys[i], value_for<P>(unsigned(i))});
      if (i % 3 != 0) borrowed.push_back(long_keys[i]);
    }
    check_fixture<P>(native, borrowed);
  }

  template <std::uint64_t K> void check_groups() {
    check_policy<storage_policy<profile_unit::byte, variable_values, K>>();
    check_policy<storage_policy<profile_unit::byte, fixed_values<3>, K>>();
    check_policy<storage_policy<profile_unit::bit, variable_values, K>>();
    check_policy<storage_policy<profile_unit::bit, fixed_values<3>, K>>();
  }
}

int main() {
  try {
    check_groups<3>();
    check_groups<7>();
    check_groups<15>();
    check_groups<31>();
    std::cout << "sampling tests passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Everett's sequential sampling of pinned encoded blob pairs.
 */
