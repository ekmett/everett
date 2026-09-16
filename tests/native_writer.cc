/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks incremental native encoding, exact batch equivalence and writer ownership.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/native_writer.h>
#include <everett/query.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
  using namespace everett;
  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F && callback) {
    bool rejected = false;
    try { callback(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "native writer accepted invalid operation");
  }
  template <class P> std::vector<profile_record> records(std::uint64_t count,
                                                        std::optional<std::uint64_t> common) {
    std::vector<profile_record> result;
    for (std::uint64_t i = 0; i != count; ++i) {
      std::string key = std::string(137, '1');
      for (unsigned j = 0; j != 16; ++j) key += char('0' + ((i >> (15 - j)) & 1));
      if constexpr (P::unit == profile_unit::byte)
        key.append((8 - key.size() % 8) % 8, '0');
      auto width = common.value_or(i % 9);
      std::string value(width * P::bits_per_unit, '0');
      for (std::size_t j = 0; j != value.size(); ++j) value[j] = (i + j) % 3 ? '0' : '1';
      result.push_back({bit_string::from_bits(key), bit_string::from_bits(value)});
    }
    return result;
  }
  template <class P> void compare(profile_array<P> const & actual, std::span<profile_record const> input,
                                  bool same_as_batch) {
    auto cursor = actual.view().cursor();
    for (auto const & record : input) {
      require(!cursor.done(), "short incremental native stream");
      auto item = cursor.peek();
      require(bit_string::copy(item.key.prefix) == record.key, "incremental native key oracle");
      require(bit_string::copy(item.value) == record.value, "incremental native value oracle");
      cursor.advance();
    }
    require(cursor.done(), "trailing incremental native stream");
    if (!same_as_batch) return;
    auto expected = profile_array<P>::build(input);
    require(std::equal(actual.bytes().begin(), actual.bytes().end(), expected.bytes().begin(), expected.bytes().end()),
            "incremental native bytes differ from batch encoding");
    auto const & a = actual.group_offsets();
    auto const & b = expected.group_offsets();
    require(a.low == b.low && a.high == b.high && a.sparse == b.sparse &&
            a.universe == b.universe && a.low_width == b.low_width && a.samples.size() == b.samples.size(),
            "incremental native Elias-Fano differs from batch");
    for (std::size_t i = 0; i != a.samples.size(); ++i)
      require(a.samples[i].first == b.samples[i].first && a.samples[i].sparse == b.samples[i].sparse,
              "incremental native select sample differs from batch");
    require(actual.metadata().common_value_width == expected.metadata().common_value_width &&
            actual.metadata().terminal_key_units == expected.metadata().terminal_key_units &&
            actual.metadata().extent == expected.metadata().extent,
            "incremental native metadata differs from batch");
  }
  template <class P> void exercise() {
    std::vector<std::optional<std::uint64_t>> widths;
    if constexpr (P::fixed_width) widths.push_back(P::value_width);
    else widths = {std::nullopt, 0, 3};
    for (auto width : widths) {
      for (auto count : {0u, 1u, 2u, 14u, 15u, 16u, 31u, 64u, 257u}) {
        auto input = records<P>(count, width);
        profile_native_writer<P> writer(width);
        require(!writer.finished() && writer.size() == 0 && writer.common_value_width() == width,
                "native writer initial state");
        for (std::size_t i = 0; i != input.size(); ++i) {
          auto scratch = input[i];
          writer.append(scratch.key.view(), scratch.value.view());
          scratch = {}; // The next append must not depend on the caller's key scratch.
          require(writer.size() == i + 1, "native writer count");
          rejects([&] { writer.append(input[i]); });
          if (i) rejects([&] { writer.append(input[i - 1]); });
          if (width) {
            auto bad_value = bit_string::from_bits(std::string((*width + 1) * P::bits_per_unit, '0'));
            rejects([&] { writer.append(input[i].key.view(), bad_value.view()); });
          }
          require(writer.size() == i + 1, "rejected append changed record count");
        }
        profile_native_writer<P> moved(std::move(writer));
        require(writer.finished(), "moved writer remains active");
        rejects([&] { (void)writer.finish(); });
        rejects([&] { writer.append(bit_view{}, bit_view{}); });
        profile_native_writer<P> assigned(width);
        assigned = std::move(moved);
        require(moved.finished(), "move-assigned writer remains active");
        auto array = assigned.finish();
        require(assigned.finished() && assigned.size() == count, "finished native writer state");
        rejects([&] { (void)assigned.finish(); });
        rejects([&] { assigned.append(bit_view{}, bit_view{}); });
        bool exact = (width && (count || *width == 0 || P::fixed_width)) || count > 1;
        compare(array, std::span<profile_record const>(input), exact);
        auto allocation = array.bytes().data();
        auto blob = std::make_shared<profile_blob<P> const>(profile_blob<P>::adopt_native(std::move(array)));
        require(blob->native().bytes().data() == allocation, "native adoption copied encoded bytes");
        require(blob->virtual_size() == count && blob->borrowed().size() == 0 && !blob->target(),
                "adopted native-only shape");
        for (std::uint64_t group = 0; group != blob->group_count(); ++group) {
          require(blob->interleave().view().rank(group) == 0 && blob->cut_lcps()[group] == 0,
                  "native-only rank and cut directory");
        }
        auto root = query_root<P>::build(blob);
        for (auto const & record : input) {
          auto query = root.cursor(record.key.view());
          while (!query.done() && !query.has_match()) query.step(1);
          require(query.has_match(), "adopted native query missed key");
          auto match = query.take_match();
          require(match.source == blob && match.value == record.value, "adopted native query value/source");
          while (!query.done()) {
            query.step(1);
            require(!query.has_match(), "adopted native query duplicate match");
          }
        }
      }
    }
    if constexpr (P::fixed_width) {
      rejects([] { profile_native_writer<P> bad(std::nullopt); });
      rejects([] { profile_native_writer<P> bad(*P::value_width + 1); });
    }
    if constexpr (P::unit == profile_unit::byte) {
      profile_native_writer<P> writer;
      auto key = bit_string::from_bits("1");
      rejects([&] { writer.append(key.view(), bit_view{}); });
      require(writer.size() == 0, "misaligned native key changed count");
    }
  }

  void prefix_keys() {
    using P = storage_policy<everett::tip<everett::encoded_sort<everett::byte_encoding<>>>>;
    std::vector<profile_record> input;
    for (std::string key : {"", "a", "aa", "ab", "b"})
      input.push_back({bit_string::from_bytes(key), bit_string::from_bytes("value")});
    profile_native_writer<P> writer(5);
    for (auto const & record : input) writer.append(record);
    compare(writer.finish(), std::span<profile_record const>(input), true);
  }
}

int main() {
  try {
    exercise<storage_policy<everett::tip<everett::encoded_sort<everett::byte_encoding<>>>>>();
    exercise<storage_policy<everett::tip<everett::encoded_sort<everett::byte_encoding<fixed_values<3>>>>, 7, exponential_golomb<0>, 16>>();
    exercise<storage_policy<everett::tip<everett::encoded_sort<everett::byte_encoding<fixed_values<0>>>>, 31, exponential_golomb<0>, 1>>();
    exercise<storage_policy<everett::tip<everett::encoded_sort<everett::bit_encoding<>>>>>();
    exercise<storage_policy<everett::tip<everett::encoded_sort<everett::bit_encoding<fixed_values<3>>>>, 3, golomb<3>, 7>>();
    exercise<storage_policy<everett::tip<everett::encoded_sort<everett::bit_encoding<fixed_values<0>>>>, 7, exponential_golomb<3>, 16>>();
    prefix_keys();
    std::cout << "native writer tests passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
