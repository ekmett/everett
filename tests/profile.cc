#include <everett/profile.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
  using namespace everett;
  using byte_policy = storage_policy<profile_unit::byte>;
  using bit_policy = storage_policy<profile_unit::bit>;

  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }

  template <class F> void rejects(F && callback) {
    bool rejected = false;
    try { callback(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "invalid profile input accepted");
  }

  bit_string binary(std::uint64_t bits, std::uint64_t seed) {
    bit_string result;
    profile_detail::resize(result, bits);
    for (std::uint64_t i = 0; i != bits; ++i) {
      seed ^= seed << 13;
      seed ^= seed >> 7;
      seed ^= seed << 17;
      if (seed & 1) result.bytes[i / 8] |= static_cast<std::byte>(1u << (7 - i % 8));
    }
    return result;
  }

  template <class P> std::vector<profile_record> fixture(bool uniform = false) {
    std::vector<bit_string> keys;
    if constexpr (P::unit == profile_unit::byte) {
      for (std::string key : {"", "a", "aa", "ab", "abc", "abd", "abde", "b"})
        keys.push_back(bit_string::from_bytes(key));
      keys.push_back(bit_string::from_bytes(std::string("\0\xff\0", 3)));
      keys.push_back(bit_string::from_bytes(std::string(1, char(255))));
      for (unsigned i = 0; i != 140; ++i) {
        auto number = std::to_string(i);
        keys.push_back(bit_string::from_bytes("long-common-prefix/" + std::string(4 - number.size(), '0') + number));
      }
    } else {
      for (std::string bits : {"", "0", "00", "001", "0010", "0011", "01", "1", "10", "11"})
        keys.push_back(bit_string::from_bits(bits));
      for (unsigned i = 0; i != 140; ++i) keys.push_back(binary(i % 57, i + 127));
    }
    std::sort(keys.begin(), keys.end(), [](auto const & a, auto const & b) {
      return compare_bits(a.view(), b.view()) < 0;
    });
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::vector<profile_record> result;
    for (std::size_t i = 0; i != keys.size(); ++i) {
      auto width = P::value_width.value_or(uniform ? 5 : i % 6);
      result.push_back({keys[i], binary(width * P::bits_per_unit, i + 1)});
    }
    return result;
  }

  template <class P> void check_array(profile_array<P> const & array, std::span<profile_record const> records) {
    require(array.size() == records.size(), "profile record count");
    auto view = array.view();
    std::uint64_t ordinal = 0;
    view.visit_all([&](profile_item<P> item) {
      require(item.ordinal == ordinal, "profile traversal ordinal");
      require(bit_string::copy(item.key.prefix) == records[ordinal].key, "profile traversal key");
      require(bit_string::copy(item.value) == records[ordinal].value, "profile traversal value");
      require(item.key.full_units == records[ordinal].key.bit_size / P::bits_per_unit, "profile full length");
      ++ordinal;
      return true;
    });
    require(ordinal == records.size(), "profile traversal count");
    for (std::size_t i = 0; i != records.size(); ++i) {
      auto full = view.reconstruct_at(i);
      require(full.prefix == records[i].key && full.value == records[i].value, "independent reconstruction");
      for (std::uint64_t limit : {0, 1, 3, 7, 19}) {
        auto decoded = view.reconstruct_at(i, limit);
        auto expected = records[i].key.view().prefix(limit * P::bits_per_unit);
        require(compare_bits(decoded.prefix.view(), expected) == 0, "partial reconstruction");
        require(decoded.full_units == records[i].key.bit_size / P::bits_per_unit, "partial full length");
      }
      auto record = view.encoded_at(i);
      auto previous = i ? records[i - 1].key.bit_size / P::bits_per_unit : 0;
      require(record.previous_units == previous && record.backspace <= previous, "actual predecessor length");
      require(record.retained == previous - record.backspace, "actual backspace count");
      auto last = std::min<std::uint64_t>(records.size(), i + P::group_size);
      auto anchor = i ? profile_anchor<P>::complete(records[i - 1].key.view()) : profile_anchor<P>{};
      std::uint64_t at = i;
      view.visit_window(i, last, anchor, 7, [&](profile_item<P> item) {
        require(item.ordinal == at, "window ordinal");
        require(compare_bits(item.key.prefix, records[at].key.view().prefix(7 * P::bits_per_unit)) == 0,
                "window spanning physical group");
        require(bit_string::copy(item.value) == records[at].value, "window value");
        ++at;
        return true;
      });
      require(at == last, "window count");
    }
    auto common = array.metadata().common_value_width.value_or(0);
    auto groups = records.size() / P::group_size + (records.size() % P::group_size != 0);
    for (std::uint64_t group = 0; group != groups; ++group) {
      auto first = group * P::group_size;
      auto offset = view.group_offsets().offset(group, common);
      require(offset < view.encoded_at(first).next_offset, "sample points before record group prefix");
      require(offset == view.group_offsets().residual(group) + first * common, "fixed value stride restoration");
    }
    require(view.group_offsets().offset(groups, common) == array.metadata().extent, "actual-N terminal offset");
    require(array.metadata().group_size == P::group_size, "group policy metadata");
  }

  template <class P> void roundtrip() {
    auto records = fixture<P>();
    for (std::uint64_t factor : {0, 3, 18}) {
      auto array = profile_array<P>::build(records, {}, factor);
      check_array(array, records);
      require(array.metadata().common_value_width == P::value_width, "nonuniform value metadata");
    }
    std::vector<std::uint64_t> ceilings(records.size());
    for (std::size_t i = 0; i != records.size(); ++i) ceilings[i] = i % 5;
    auto conservative = profile_array<P>::build(records, ceilings, 18);
    check_array(conservative, records);
    for (std::size_t i = 0; i != records.size(); ++i)
      require(conservative.view().encoded_at(i).retained <= ceilings[i], "conservative prefix ceiling");

    auto uniform = fixture<P>(true);
    auto fixed = profile_array<P>::build(uniform);
    require(fixed.metadata().common_value_width == P::value_width.value_or(5), "uniform value detection");
    check_array(fixed, uniform);

    std::vector<profile_record> no_values = uniform;
    for (auto & item : no_values) item.value = {};
    auto borrowed = profile_array<P, stream_role::borrowed>::build(no_values);
    require(borrowed.metadata().common_value_width == 0, "same-P borrowed empty values");
    for (std::uint64_t group = 0; group <= fixed.group_offsets().view().group_count(); ++group)
      require(fixed.group_offsets().view().residual(group) == borrowed.group_offsets().view().residual(group),
              "constant value bytes excluded from EF universe");
    auto empty = profile_array<P>::build({});
    require(empty.view().size() == 0 && empty.metadata().extent == 0, "empty profile");
    profile_array<P> default_empty;
    require(default_empty.view().size() == 0, "default profile");

    auto wrong = fixed.metadata();
    wrong.group_size = P::group_size == 15 ? 7 : 15;
    rejects([&] { profile_view<P> invalid(fixed.bytes(), fixed.group_offsets().view(), wrong); });
    wrong = fixed.metadata();
    wrong.offset_unit = P::unit == profile_unit::byte ? profile_unit::bit : profile_unit::byte;
    rejects([&] { profile_view<P> invalid(fixed.bytes(), fixed.group_offsets().view(), wrong); });
    wrong = fixed.metadata();
    wrong.policy_fixed_values = !P::fixed_width;
    rejects([&] { profile_view<P> invalid(fixed.bytes(), fixed.group_offsets().view(), wrong); });
    rejects([&] { fixed.view().visit_window(0, P::group_size + 1, {}, 1, [](auto) { return true; }); });
    rejects([&] { fixed.view().encoded_at(fixed.size()); });
    rejects([&] { profile_array<P>::build(records, {}, 2); });
    rejects([&] { profile_array<P>::build(records, std::span<std::uint64_t const>(ceilings).first(1)); });
    if constexpr (P::fixed_width) {
      auto invalid = uniform;
      invalid.front().value = binary((*P::value_width + 1) * P::bits_per_unit, 99);
      rejects([&] { profile_array<P>::build(invalid); });
    }
    if (!uniform.front().value.view().empty())
      rejects([&] { profile_array<P, stream_role::borrowed>::build(uniform); });
  }

  template <class P> void lpfc() {
    std::vector<profile_record> records;
    auto prefix = P::unit == profile_unit::byte ? bit_string::from_bytes(std::string(31, 'a'))
                                               : bit_string::from_bits(std::string(31, '0'));
    for (unsigned i = 0; i != 400; ++i) records.push_back({prefix, {}});
    auto array = profile_array<P>::build(records, {}, 18);
    auto view = array.view();
    std::uint64_t anchor = 0;
    std::uint64_t restarts = 0;
    for (std::uint64_t i = 0; i != records.size(); ++i) {
      auto record = view.encoded_at(i);
      auto position = i ? view.encoded_at(i - 1).next_offset : 0;
      if (i % P::group_size == 0) {
        auto packed = bit_view(array.bytes(), array.metadata().extent * P::bits_per_unit);
        (void)profile_detail::read_count<P>(packed, position);
      }
      if (!record.retained) { anchor = position; ++restarts; }
      else require(position - anchor <= 18 * record.key_units, "LPFC physical distance bound");
    }
    require(restarts > 1, "LPFC fixture must trigger a restart");
    check_array(array, records);
  }

  template <class P> void surrogate_anchor() {
    // The incoming key lies between the actual predecessor and this candidate.
    // Stored retained length is a lower bound, not exact LCP with that anchor.
    std::vector<profile_record> records{{bit_string::from_bytes("aa"), {}},
                                        {bit_string::from_bytes("abc"), {}},
                                        {bit_string::from_bytes("abde"), {}}};
    auto array = profile_array<P>::build(records);
    auto lower = bit_string::from_bytes("ab");
    auto query = bit_string::from_bytes("abd");
    auto anchor = profile_anchor<P>::complete(lower.view());
    int comparisons = 0;
    array.view().visit_window(1, 3, anchor, query.bit_size / P::bits_per_unit, [&](profile_item<P> item) {
      auto order = compare_profile_prefix(item.key, query.view());
      require(comparisons ? order > 0 : order < 0, "surrogate anchor comparison");
      ++comparisons;
      return true;
    });
    require(comparisons == 2, "surrogate comparison count");
    // A conservative prefix can also reconstruct a preceding borrowed key
    // from a known upper frontier; it uses the absolute retained prefix.
    auto upper = bit_string::from_bytes("abd");
    std::array<std::uint64_t, 3> ceilings{0, 0, 0};
    auto conservative = profile_array<P>::build(records, ceilings);
    auto decoded_anchor = profile_anchor<P>::complete(upper.view());
    conservative.view().visit_window(1, 2, decoded_anchor, 24 / P::bits_per_unit, [&](profile_item<P> item) {
      require(compare_bits(item.key.prefix, records[1].key.view()) == 0, "upper frontier conservative decode");
      return true;
    });
  }

  void counts() {
    std::array<std::uint64_t, 12> values{0, 1, 2, 3, 7, 127, 128, 255, 65535,
      std::uint64_t{1} << 63, std::numeric_limits<std::uint64_t>::max() - 1,
      std::numeric_limits<std::uint64_t>::max()};
    auto roundtrip_counts = [&]<class P>() {
      bit_string encoded;
      for (auto value : values) profile_detail::write_count<P>(encoded, value);
      std::uint64_t offset = 0;
      for (auto value : values)
        require(profile_detail::read_count<P>(encoded.view(), offset) == value, "count code roundtrip");
      require(offset * P::bits_per_unit == encoded.bit_size, "count code extent");
      rejects([&] { (void)profile_detail::read_count<P>(encoded.view(), offset); });
    };
    roundtrip_counts.template operator()<byte_policy>();
    roundtrip_counts.template operator()<bit_policy>();
    for (std::string malformed : std::vector<std::string>{"", "0", "0001", std::string(65, '0'), std::string(64, '0') + "11" + std::string(63, '0')}) {
      auto data = bit_string::from_bits(malformed);
      std::uint64_t offset = 0;
      rejects([&] { (void)profile_detail::read_count<bit_policy>(data.view(), offset); });
    }
    for (auto malformed : {std::vector<std::byte>{std::byte{0x80}},
                           std::vector<std::byte>{std::byte{0x80}, std::byte{0}},
                           std::vector<std::byte>(10, std::byte{0xff})}) {
      auto data = bit_string::from_bytes(malformed);
      std::uint64_t offset = 0;
      rejects([&] { (void)profile_detail::read_count<byte_policy>(data.view(), offset); });
    }
  }

  template <class P> void malformed_streams() {
    // A well-shaped sampled directory cannot make an invalid record header valid.
    auto make_view = [&](bit_string const & data, std::uint64_t records = 1) {
      auto metadata = profile_detail::initial_metadata<P, stream_role::native>();
      metadata.record_count = records;
      metadata.extent = data.bit_size / P::bits_per_unit;
      metadata.common_value_width = 0;
      std::array<std::uint64_t, 2> offsets{0, metadata.extent};
      auto index = select_groups<P::group_size>::build(offsets, records);
      profile_view<P> view(data.bytes, index.view(), metadata);
      view.visit_all([](auto) { return true; });
    };
    auto encoded = [&](std::initializer_list<std::uint64_t> headers) {
      bit_string result;
      for (auto header : headers) profile_detail::write_count<P>(result, header);
      return result;
    };
    auto backspace = encoded({0, 1, 0});
    rejects([&] { make_view(backspace); });
    auto missing_suffix = encoded({0, 0, 9});
    rejects([&] { make_view(missing_suffix); });
    auto predecessor = encoded({1, 0, 0});
    rejects([&] { make_view(predecessor); });
    auto trailing = encoded({0, 0, 0, 0});
    rejects([&] { make_view(trailing); });

    auto records = fixture<P>();
    auto array = profile_array<P>::build(records);
    std::vector<std::uint64_t> wrong_offsets;
    for (std::uint64_t group = 0; group <= array.group_offsets().view().group_count(); ++group)
      wrong_offsets.push_back(array.group_offsets().view().residual(group));
    wrong_offsets[0] = 1;
    auto leading = select_groups<P::group_size>::build(wrong_offsets, array.size());
    rejects([&] { profile_view<P> invalid(array.bytes(), leading.view(), array.metadata()); });
    wrong_offsets[0] = 0;
    ++wrong_offsets[1];
    auto inconsistent = select_groups<P::group_size>::build(wrong_offsets, array.size());
    profile_view<P> wrong_samples(array.bytes(), inconsistent.view(), array.metadata());
    rejects([&] { wrong_samples.visit_all([](auto) { return true; }); });
    auto bad_bytes = std::vector<std::byte>(array.bytes().begin(), array.bytes().end());
    auto metadata = array.metadata();
    if constexpr (P::unit == profile_unit::bit) {
      std::vector<profile_record> singleton{{{}, {}}};
      auto tail = profile_array<P>::build(singleton);
      auto bytes = std::vector<std::byte>(tail.bytes().begin(), tail.bytes().end());
      require(tail.metadata().extent % 8 != 0, "partial-byte fixture");
      bytes.back() |= std::byte{1};
      rejects([&] { profile_view<P> invalid(bytes, tail.group_offsets().view(), tail.metadata()); });
    }
    bad_bytes.pop_back();
    rejects([&] { profile_view<P> invalid(bad_bytes, array.group_offsets().view(), metadata); });
    metadata.extent = std::numeric_limits<std::uint64_t>::max();
    rejects([&] { profile_view<P> invalid(array.bytes(), array.group_offsets().view(), metadata); });
    auto bad = binary(3, 1);
    bad.bytes[0] |= std::byte{1};
    rejects([&] { (void)bad.view(); });
    rejects([&] { bit_view invalid({}, 1); });
    if constexpr (P::unit == profile_unit::byte) {
      std::vector<profile_record> invalid{{bit_string::from_bits("101"), {}}};
      rejects([&] { profile_array<P>::build(invalid); });
    }
  }

  template <std::uint64_t K> void policies() {
    roundtrip<storage_policy<profile_unit::byte, variable_values, K>>();
    roundtrip<storage_policy<profile_unit::byte, fixed_values<3>, K>>();
    roundtrip<storage_policy<profile_unit::byte, fixed_values<0>, K>>();
    roundtrip<storage_policy<profile_unit::bit, variable_values, K>>();
    roundtrip<storage_policy<profile_unit::bit, fixed_values<3>, K>>();
    roundtrip<storage_policy<profile_unit::bit, fixed_values<0>, K>>();
    lpfc<storage_policy<profile_unit::byte, variable_values, K>>();
    lpfc<storage_policy<profile_unit::bit, variable_values, K>>();
  }
}

int main() {
  try {
    counts();
    policies<3>();
    policies<7>();
    policies<15>();
    policies<31>();
    surrogate_anchor<byte_policy>();
    surrogate_anchor<bit_policy>();
    malformed_streams<byte_policy>();
    malformed_streams<bit_policy>();
    std::cout << "profile tests passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-Everett-All-Rights-Reserved
 * \endlicense
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Everett's profile behavior.
 */
