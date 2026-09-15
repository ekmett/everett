/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/profile_blob.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
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

  template <class F>
  void rejects(F && f) {
    bool rejected = false;
    try { f(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "invalid profile blob input accepted");
  }

  std::uint64_t oracle_common(bit_view a, bit_view b) {
    std::uint64_t i = 0;
    while (i < std::min(a.size(), b.size()) && a.at(i) == b.at(i)) ++i;
    return i;
  }
  int oracle_order(bit_view a, bit_view b) {
    auto i = oracle_common(a, b);
    if (i < std::min(a.size(), b.size())) return a.at(i) ? 1 : -1;
    return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
  }
  bool equal(bit_view a, bit_view b) { return oracle_order(a, b) == 0; }

  std::string numbered(unsigned n) {
    auto number = std::to_string(n);
    return "shared-prefix/" + std::string(5 - number.size(), '0') + number;
  }

  template <class P>
  bit_string value_for(unsigned n) {
    auto units = P::value_width ? *P::value_width : n % 14;
    if constexpr (P::unit == profile_unit::byte) {
      std::string bytes(static_cast<std::size_t>(units), '\0');
      for (std::size_t i = 0; i != bytes.size(); ++i) bytes[i] = char((n * 37 + i * 91) & 255);
      return bit_string::from_bytes(bytes);
    } else {
      std::string bits(static_cast<std::size_t>(units), '0');
      for (std::size_t i = 0; i != bits.size(); ++i) bits[i] = ((n * 37 + i * 91) >> (i % 5)) & 1 ? '1' : '0';
      return bit_string::from_bits(bits);
    }
  }

  template <class P>
  std::vector<bit_string> keys_for() {
    std::vector<bit_string> keys;
    if constexpr (P::unit == profile_unit::byte) {
      for (auto const & key : std::vector<std::string>{"", std::string(1, '\0'), std::string("\0x", 2),
            "a", "aa", "ab", "abc", "abd", std::string(1, char(128)), std::string(1, char(255))}) {
        keys.push_back(bit_string::from_bytes(key));
      }
      for (unsigned i = 0; i != 160; ++i) keys.push_back(bit_string::from_bytes(numbered(i)));
    } else {
      for (unsigned length = 0; length != 9; ++length) {
        for (unsigned n = 0; n != (1u << length); ++n) {
          std::string bits(length, '0');
          for (unsigned i = 0; i != length; ++i) bits[i] = (n >> (length - i - 1)) & 1 ? '1' : '0';
          keys.push_back(bit_string::from_bits(bits));
        }
      }
    }
    std::sort(keys.begin(), keys.end(), [](auto const & a, auto const & b) {
      return oracle_order(a.view(), b.view()) < 0;
    });
    return keys;
  }

  struct virtual_entry {
    bit_string key;
    bool borrowed = false;
    std::uint64_t ordinal = 0;
  };

  std::vector<virtual_entry> catalog_for(std::span<profile_record const> native,
                                        std::span<bit_string const> borrowed) {
    std::vector<virtual_entry> catalog;
    for (std::size_t i = 0; i != native.size(); ++i) catalog.push_back({native[i].key, false, i});
    for (std::size_t i = 0; i != borrowed.size(); ++i) catalog.push_back({borrowed[i], true, i});
    std::stable_sort(catalog.begin(), catalog.end(), [](auto const & a, auto const & b) {
      auto order = oracle_order(a.key.view(), b.key.view());
      return order ? order < 0 : a.borrowed < b.borrowed;
    });
    return catalog;
  }

  template <class P>
  void check_blob(std::span<profile_record const> native, std::span<bit_string const> borrowed,
                  std::span<bit_string const> queries, profile_blob<P> const * existing = nullptr) {
    auto encoded = existing ? *existing : profile_blob<P>::build(native, borrowed);
    require(encoded.native().size() == native.size() && encoded.borrowed().size() == borrowed.size(),
            "profile blob native/borrowed counts");
    for (std::size_t i = 0; i != borrowed.size(); ++i) {
      auto record = encoded.borrowed().view().encoded_at(i);
      auto retained = i ? oracle_common(borrowed[i - 1].view(), borrowed[i].view()) / P::bits_per_unit : 0;
      require(record.retained == retained, "borrowed stream is ordinary FC");
      require(record.value.size() == 0, "borrowed stream has zero-width values under the same policy");
      auto found = std::find_if(native.begin(), native.end(), [&](auto const & record) {
        return equal(record.key.view(), borrowed[i].view());
      });
      require(encoded.false_borrow(i) == (found != native.end()), "false-borrow equality oracle");
    }
    for (std::size_t i = 0; i != native.size(); ++i) {
      auto retained = i ? oracle_common(native[i - 1].key.view(), native[i].key.view()) / P::bits_per_unit : 0;
      require(encoded.native().view().encoded_at(i).retained == retained, "native stream is ordinary FC");
    }

    auto catalog = catalog_for(native, borrowed);
    require(encoded.cut_lcps().size() == encoded.group_count(), "one cut LCP per virtual group");
    std::uint64_t native_at = 0;
    std::uint64_t borrowed_at = 0;
    for (std::uint64_t g = 0; g != encoded.group_count(); ++g) {
      auto projected = encoded.project(g);
      auto expected_lcp = borrowed_at ? oracle_common(borrowed[borrowed_at - 1].view(),
        catalog[g * P::group_size].key.view()) : 0;
      require(encoded.cut_lcps()[g] == expected_lcp, "exact bit LCP at each virtual cut");
      require(projected.native_first == native_at && projected.borrowed_first == borrowed_at,
              "group ranks count entries independently of profile units");
      for (auto i = g * P::group_size; i != std::min<std::uint64_t>(catalog.size(), g * P::group_size + P::group_size); ++i) {
        if (catalog[i].borrowed) ++borrowed_at;
        else ++native_at;
      }
      require(projected.native_last == native_at && projected.borrowed_last == borrowed_at,
              "group rank upper projection oracle");
      require(projected.native_last - projected.native_first + projected.borrowed_last - projected.borrowed_first <= P::group_size,
              "projected window is bounded by the configured record count");
    }
    if (catalog.empty()) {
      rejects([&] { encoded.search_window(0, profile_query_context<P>({})); });
      return;
    }

    for (auto const & query : queries) {
      auto upper = std::upper_bound(catalog.begin(), catalog.end(), query, [](auto const & q, auto const & record) {
        return oracle_order(q.view(), record.key.view()) < 0;
      });
      auto ordinal = upper == catalog.begin() ? 0 : static_cast<std::uint64_t>(upper - catalog.begin() - 1);
      auto group = ordinal / P::group_size;
      auto const & boundary = catalog[group * P::group_size].key;
      profile_query_context<P> anchor(query.view());
      if (upper != catalog.begin()) anchor = anchor.with_key(boundary.view());
      {
        auto result = encoded.search_window(group, anchor);
        auto expected = std::find_if(native.begin(), native.end(), [&](auto const & record) {
          return equal(record.key.view(), query.view());
        });
        require(bool(result.native) == (expected != native.end()), "native lookup presence oracle");
        if (result.native) {
          require(result.native->ordinal == std::uint64_t(expected - native.begin()), "native ordinal oracle");
          require(equal(result.native->value.view(), expected->value.view()), "native value bits and length oracle");
        }
        auto expected_sample = std::upper_bound(borrowed.begin(), borrowed.end(), query,
          [](auto const & q, auto const & key) { return oracle_order(q.view(), key.view()) < 0; });
        require(bool(result.borrowed_predecessor) == (expected_sample != borrowed.begin()),
                "borrowed predecessor presence oracle");
        if (result.borrowed_predecessor) {
          auto const & next = *result.borrowed_predecessor;
          auto expected_ordinal = std::uint64_t(expected_sample - borrowed.begin() - 1);
          require(next.ordinal == expected_ordinal && next.target_ordinal == expected_ordinal * P::group_size,
                  "borrowed target ordinal uses entry units");
          auto const & expected_key = borrowed[expected_ordinal];
          require(next.comparison.common_bits() == oracle_common(expected_key.view(), query.view()),
                  "outgoing exact bit agreement oracle");
          require(next.comparison.order() == oracle_order(expected_key.view(), query.view()),
                  "outgoing comparison direction oracle");
          require(!next.comparison.full_units() || next.comparison.full_units() == expected_key.bit_size / P::bits_per_unit,
                  "known borrowed full length uses profile units");
          require(equal(next.comparison.query(), query.view()), "outgoing comparison retains its query");
        }
      }
    }
  }

  template <class P>
  void window_oracles() {
    auto keys = keys_for<P>();
    check_blob<P>({}, {}, keys);
    std::vector<profile_record> native;
    std::vector<bit_string> borrowed;
    for (std::size_t i = 0; i != keys.size(); ++i) {
      if (i % 3) native.push_back({keys[i], value_for<P>(static_cast<unsigned>(i))});
      if (i % 4) borrowed.push_back(keys[i]);
      if (i % 4 && i % 13 == 0) borrowed.push_back(keys[i]);
    }
    check_blob<P>(native, borrowed, keys);
    check_blob<P>(native, {}, keys);
    check_blob<P>({}, borrowed, keys);

    // A native equality can precede the selected window by several groups.
    // Every repeated routing copy must preserve the same native binding.
    auto q = keys[keys.size() / 2];
    std::vector<profile_record> single{{q, value_for<P>(99)}};
    std::vector<bit_string> duplicates(70, q);
    check_blob<P>(single, duplicates, keys);
  }

  template <class P>
  std::vector<bit_string> samples_for(std::span<profile_record const> native,
                                     std::span<bit_string const> borrowed) {
    auto catalog = catalog_for(native, borrowed);
    std::vector<bit_string> samples;
    for (std::size_t i = 0; i < catalog.size(); i += P::group_size) samples.push_back(catalog[i].key);
    return samples;
  }

  template <class P>
  void cascade_oracle() {
    auto keys = keys_for<P>();
    std::vector<std::vector<profile_record>> records(3);
    for (std::size_t i = 0; i != keys.size(); ++i) {
      if (i % 2 == 0) records[0].push_back({keys[i], value_for<P>(static_cast<unsigned>(i))});
      if (i % 11 == 0) records[1].push_back({keys[i], value_for<P>(1000 + static_cast<unsigned>(i))});
      if (i % 79 == 0) records[2].push_back({keys[i], value_for<P>(2000 + static_cast<unsigned>(i))});
    }
    std::vector<std::vector<bit_string>> samples(3);
    samples[1] = samples_for<P>(records[0], {});
    samples[2] = samples_for<P>(records[1], samples[1]);
    // Empty-native routing catalogs retain the intervening levels. This is a
    // query fixture, not an assertion about a future redundant-level scheduler.
    while (records.back().size() + samples.back().size() > P::group_size) {
      auto next = samples_for<P>(records.back(), samples.back());
      records.emplace_back();
      samples.push_back(std::move(next));
    }
    std::vector<profile_blob<P>> levels;
    for (std::size_t i = 0; i != records.size(); ++i) {
      levels.push_back(profile_blob<P>::build(records[i], samples[i]));
    }
    require(levels.back().virtual_size() <= P::group_size, "cascade root fits one bounded window");
    bool simultaneous_native_and_route = false;
    for (auto const & query : keys) {
      std::vector<std::optional<bit_string>> expected(records.size());
      for (std::size_t level = 0; level != records.size(); ++level) {
        for (auto const & record : records[level]) {
          if (equal(record.key.view(), query.view())) { expected[level] = record.value; break; }
        }
      }
      std::vector<std::optional<bit_string>> actual(records.size());
      profile_query_context<P> anchor(query.view());
      std::uint64_t group = 0;
      for (std::size_t remaining = levels.size(); remaining; --remaining) {
        auto level = remaining - 1;
        auto result = levels[level].search_window(group, anchor);
        if (result.native) actual[level] = result.native->value;
        if (result.native && result.borrowed_predecessor) simultaneous_native_and_route = true;
        // Keep searching on equality: this layer returns value fragments and
        // routing, leaving chronological composition to its caller.
        if (!result.borrowed_predecessor) break;
        auto const & next = *result.borrowed_predecessor;
        anchor = next.comparison;
        group = next.target_ordinal / P::group_size;
      }
      for (std::size_t level = 0; level != actual.size(); ++level) {
        require(bool(actual[level]) == bool(expected[level]), "cascade finds every native segment");
        if (actual[level]) require(equal(actual[level]->view(), expected[level]->view()), "cascade preserves all value bits");
      }
    }
    require(simultaneous_native_and_route, "native equality still returns a downstream route");
  }

  template <class P>
  void reindex_preserves_native() {
    auto keys = keys_for<P>();
    std::vector<profile_record> native;
    std::vector<bit_string> old_samples;
    std::vector<bit_string> new_samples;
    for (std::size_t i = 0; i + 1 < keys.size(); i += 2) {
      native.push_back({keys[i], value_for<P>(static_cast<unsigned>(i))});
      old_samples.push_back(keys[i]);
      new_samples.push_back(keys[i + 1]);
    }
    auto original = profile_blob<P>::build(native, old_samples);
    auto retained = original;
    auto replacement = original.reindex(new_samples);
    require(&replacement.native() == &original.native(), "reindex shares exact native FC allocation");
    require(replacement.native().bytes().data() == retained.native().bytes().data(), "retained world keeps native bytes");
    for (std::size_t i = 0; i != old_samples.size(); ++i) {
      require(original.false_borrow(i) && retained.false_borrow(i), "old index keeps equality flags");
      require(!replacement.false_borrow(i), "new index recomputes equality flags");
      auto before = retained.borrowed().view().reconstruct_at(i);
      auto after = replacement.borrowed().view().reconstruct_at(i);
      require(equal(before.prefix.view(), old_samples[i].view()), "old borrowed data remains readable");
      require(equal(after.prefix.view(), new_samples[i].view()), "new borrowed data is independent");
    }
    // Both old and new indices continue to work after the replacement exists.
    check_blob<P>(native, old_samples, keys, &original);
    check_blob<P>(native, new_samples, keys, &replacement);
  }

  template <class P>
  void invalid_inputs() {
    auto keys = keys_for<P>();
    std::array<profile_record, 2> duplicate{{{keys[1], value_for<P>(1)}, {keys[1], value_for<P>(2)}}};
    rejects([&] { profile_blob<P>::build(duplicate); });
    std::array<profile_record, 2> reversed{{{keys[2], value_for<P>(1)}, {keys[1], value_for<P>(2)}}};
    rejects([&] { profile_blob<P>::build(reversed); });
    std::array<bit_string, 2> bad_samples{{keys[2], keys[1]}};
    rejects([&] { profile_blob<P>::build({}, bad_samples); });
    std::array<bit_string, 1> sample{{keys[1]}};
    if constexpr (P::unit == profile_unit::byte) {
      auto valid = profile_blob<P>::build({}, sample);
      auto odd = bit_string::from_bits("1");
      rejects([&] { valid.search_window(0, profile_query_context<P>(odd.view())); });
    }
  }

  template <class P>
  void profile_suite() {
    static_assert(std::is_same_v<typename profile_blob<P>::policy_type, P>);
    static_assert(std::is_same_v<typename profile_blob<P>::native_array,
                  profile_array<P, stream_role::native>>);
    static_assert(std::is_same_v<typename profile_blob<P>::borrowed_array,
                  profile_array<P, stream_role::borrowed>>);
    window_oracles<P>();
    cascade_oracle<P>();
    reindex_preserves_native<P>();
    invalid_inputs<P>();
  }
}

int main() {
  try {
    [&]<std::size_t... K>(std::index_sequence<K...>) {
      (profile_suite<storage_policy<profile_unit::byte, variable_values, K>>(), ...);
      (profile_suite<storage_policy<profile_unit::byte, fixed_values<3>, K>>(), ...);
      (profile_suite<storage_policy<profile_unit::bit, variable_values, K>>(), ...);
      (profile_suite<storage_policy<profile_unit::bit, fixed_values<5>, K>>(), ...);
    }(std::index_sequence<3, 7, 15, 31>{});
    std::cout << "Typed byte/bit blob, cascade, false-borrow and index-reuse oracles passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Everett's profile blob behavior.
 */
