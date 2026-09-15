/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

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
#include <utility>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

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

  unsigned slow_bit(std::span<std::byte const> bytes, std::uint64_t at) {
    return (std::to_integer<unsigned>(bytes[at / 8]) >> (7 - at % 8)) & 1;
  }
  void slow_store(std::span<std::byte> bytes, std::uint64_t at, unsigned value) {
    auto mask = std::byte(1u << (7 - at % 8));
    if (value) bytes[at / 8] |= mask; else bytes[at / 8] &= ~mask;
  }
  void subview_oracle() {
    constexpr bit_view constant_empty;
    constexpr bit_view constant_copy = constant_empty;
    auto empty = constant_copy.subview(0, 0);
    require(empty.empty() && empty.offset() == 0 && empty.storage().empty(),
            "default empty subview");
    std::array<std::byte, 4> bytes{std::byte{0x81}, std::byte{0x6a}, std::byte{0xfe}, std::byte{0x35}};
    auto check = [&](bit_view value, std::uint64_t offset, std::uint64_t size) {
      require(value.offset() == offset && value.size() == size &&
              value.storage().data() == bytes.data() && value.storage().size() == bytes.size(),
              "subview storage and extent");
      for (std::uint64_t i = 0; i < size; ++i)
        require(value.at(i) == (slow_bit(bytes, offset + i) != 0), "subview bit oracle");
    };
    // All valid views and subranges, including byte boundaries and empty ends.
    for (std::uint64_t offset = 0; offset <= 32; ++offset)
      for (std::uint64_t size = 0; size <= 32 - offset; ++size) {
        bit_view original(bytes, size, offset);
        check(original.prefix(std::numeric_limits<std::uint64_t>::max()), offset, size);
        for (std::uint64_t first = 0; first <= size; ++first)
          for (std::uint64_t count = 0; count <= size - first; ++count) {
            auto value = original.subview(first, count);
            check(value, offset + first, count);
            check(value.subview(count / 2, count - count / 2), offset + first + count / 2,
                  count - count / 2);
          }
        check(original, offset, size);
        auto rejected = [&](std::uint64_t first, std::uint64_t count) {
          bool caught = false;
          try { (void)original.subview(first, count); }
          catch (std::out_of_range const & error) {
            caught = std::string(error.what()) == "bit subview";
          }
          require(caught, "subview exception type and message");
        };
        rejected(size + 1, 0);
        rejected(size, 1);
        rejected(0, size + 1);
        rejected(std::numeric_limits<std::uint64_t>::max(), 0);
        rejected(0, std::numeric_limits<std::uint64_t>::max());
      }
    auto zero_storage = std::span<std::byte const>(bytes).subspan(2, 0);
    auto zero = bit_view(zero_storage, 0).subview(0, 0);
    require(zero.empty() && zero.storage().data() == bytes.data() + 2 && zero.storage().empty(),
            "empty subview preserves non-null storage pointer");
  }

  bit_comparison slow_compare(bit_view a, bit_view b) {
    std::uint64_t i = 0;
    while (i < a.size() && i < b.size() && a.at(i) == b.at(i)) ++i;
    if (i < a.size() && i < b.size()) return {i, a.at(i) ? 1 : -1};
    return {i, a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0};
  }
  void check_comparison(bit_view a, bit_view b) {
    auto want = slow_compare(a, b), got = compare_common_bits(a, b);
    require(got.common_bits == want.common_bits && got.order == want.order, "word comparison oracle");
    require(compare_bits(a, b) == want.order, "comparison order wrapper");
    require(common_prefix_units<bit_policy>(a, b) == want.common_bits, "bit common-prefix oracle");
    if (a.size() % 8 == 0 && b.size() % 8 == 0)
      require(common_prefix_units<byte_policy>(a, b) == want.common_bits / 8, "byte common-prefix oracle");
  }
  void byte_comparison_oracle() {
    for (std::size_t length : {0u,1u,7u,8u,9u,15u,16u,17u,31u,32u,33u,127u,128u,129u,1024u}) {
      std::string text(length, '\0');
      for (std::size_t i = 0; i != length; ++i) text[i] = char((i * 19 + 137) & 255);
      auto a = bit_string::from_bytes(text);
      require(compare_bits(a.view(), a.view()) == 0 &&
              common_prefix_units<byte_policy>(a.view(), a.view()) == length, "equal byte keys");
      for (std::size_t i = 0; i != length; ++i) {
        auto other = text;
        other[i] ^= 1;
        auto b = bit_string::from_bytes(other);
        auto order = static_cast<unsigned char>(text[i]) < static_cast<unsigned char>(other[i]) ? -1 : 1;
        auto both = compare_common_bits(a.view(), b.view());
        require(both.common_bits == i * 8 + 7 && both.order == order, "long byte comparison oracle");
        require(compare_bits(b.view(), a.view()) == -order, "reverse byte comparison oracle");
        require(common_prefix_units<byte_policy>(a.view(), b.view()) == i, "whole-byte LCP oracle");
      }
      if (length) {
        auto shorter = a.view().prefix((length - 1) * 8);
        require(compare_bits(a.view(), shorter) > 0 && compare_bits(shorter, a.view()) < 0,
                "byte strict prefix order");
        require(common_prefix_units<byte_policy>(a.view(), shorter) == length - 1, "byte strict prefix LCP");
      }
    }
  }

  void intermediate_byte_anchors() {
    std::vector<std::string> keys{"", "a", "aa", "aaa", "aab", "ab", "aba", "abb", "b", "ba"};
    for (std::size_t x = 0; x != keys.size(); ++x)
      for (std::size_t y = x; y != keys.size(); ++y) {
        std::array<profile_record, 2> records{{{bit_string::from_bytes(keys[x]), {}},
                                            {bit_string::from_bytes(keys[y]), {}}}};
        auto array = profile_array<byte_policy>::build(records);
        for (std::size_t middle = x; middle <= y; ++middle)
          for (std::uint64_t limit = 0; limit != 4; ++limit) {
            auto key = bit_string::from_bytes(keys[middle]);
            profile_anchor<byte_policy> anchor{key.view().prefix(std::min<std::uint64_t>(key.bit_size, limit * 8)),
                                                keys[middle].size()};
            unsigned visited = 0;
            array.view().visit_window(1, 2, anchor, limit, [&](profile_item<byte_policy> item) {
              auto expected = bit_string::from_bytes(keys[y].substr(0, limit));
              require(bit_string::copy(item.key.prefix) == expected && item.key.full_units == keys[y].size(),
                      "intermediate byte anchor prefix convexity");
              ++visited;
              return true;
            });
            require(visited == 1, "intermediate byte anchor visit count");
          }
      }
    std::string prefix(200000, 'x');
    std::vector<profile_record> records;
    for (char suffix : {'a', 'b', 'c'}) records.push_back({bit_string::from_bytes(prefix + suffix), {}});
    auto array = profile_array<byte_policy>::build(records);
    auto key = bit_string::from_bytes("xxx");
    profile_anchor<byte_policy> anchor{key.view(), prefix.size() + 1};
    unsigned visited = 0;
    array.view().visit_window(1, 3, anchor, 3, [&](profile_item<byte_policy> item) {
      require(bit_string::copy(item.key.prefix) == key && item.key.full_units == prefix.size() + 1,
              "long key partial reconstruction");
      ++visited;
      return true;
    });
    require(visited == 2, "long key partial visit count");
  }

  void bit_primitives() {
    constexpr std::array<unsigned, 22> lengths{0,1,2,7,8,9,15,16,17,31,32,33,63,64,65,127,128,129,255,256,257,1023};
    for (unsigned source_offset = 0; source_offset != 8; ++source_offset)
      for (unsigned target_offset = 0; target_offset != 8; ++target_offset)
        for (auto length : lengths) {
          auto original = binary(length + source_offset + 13, 0x981235u + length);
          auto source = original.view().subview(source_offset, length);
          auto target = binary(length + target_offset + 13, 0xaef351u + length);
          auto expected = target;
          for (unsigned i = 0; i != length; ++i) slow_store(expected.bytes, target_offset + i, source.at(i));
          profile_detail::copy_into(target, target_offset, source);
          require(target == expected, "bit copy changed edge bits or contents");
          auto copied = bit_string::copy(source);
          copied.validate();
          for (unsigned i = 0; i != length; ++i) require(copied.view().at(i) == source.at(i), "bit-string copy oracle");
          auto other = target.view().subview(target_offset, length);
          check_comparison(source, other); check_comparison(other, source);
          if (length) {
            check_comparison(source.prefix(length - 1), other);
            check_comparison(other, source.prefix(length - 1));
            for (auto position : {0u, length / 2, length - 1}) {
              auto changed = target;
              auto at = target_offset + position;
              slow_store(changed.bytes, at, 1 - slow_bit(changed.bytes, at));
              auto changed_view = changed.view().subview(target_offset, length);
              check_comparison(source, changed_view); check_comparison(changed_view, source);
            }
          }
        }
    // Every differing bit, including byte/word/vector boundaries and unused tails.
    auto original = binary(513, 0x15931);
    for (unsigned i = 0; i != 513; ++i) {
      auto changed = original;
      slow_store(changed.bytes, i, 1 - slow_bit(changed.bytes, i));
      check_comparison(original.view(), changed.view());
      check_comparison(changed.view(), original.view());
    }
    for (unsigned source : {0u,1u,7u,8u,9u,63u,64u})
      for (unsigned destination : {0u,1u,7u,8u,9u,63u,64u}) {
        auto target = binary(512, 55), expected = target, before = target;
        for (unsigned i = 0; i != 257; ++i) slow_store(expected.bytes, destination + i, slow_bit(before.bytes, source + i));
        profile_detail::copy_into(target, destination, target.view().subview(source, 257));
        require(target == expected, "overlapping bit copy");
      }
    for (unsigned offset = 0; offset != 8; ++offset) {
      auto target = binary(129, offset + 1), before = target;
      target.bytes.shrink_to_fit();
      profile_detail::append(target, target.view().subview(offset, 117));
      target.validate();
      require(target.bit_size == 246, "self-append size");
      for (unsigned i = 0; i != 129; ++i) require(target.view().at(i) == before.view().at(i), "self-append original prefix");
      for (unsigned i = 0; i != 117; ++i) require(target.view().at(129 + i) == before.view().at(offset + i), "self-append source lifetime");
      for (unsigned width = 0; width <= 64; ++width) {
        auto mask = width == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << width) - 1;
        for (auto value : {std::uint64_t{0}, mask, std::uint64_t{0x9135a2fedc6748bb}}) {
          auto field = binary(offset + width + 13, 619), expected = field;
          for (unsigned i = 0; i != width; ++i) slow_store(expected.bytes, offset + i, unsigned((value >> (width - 1 - i)) & 1));
          std::uint64_t at = offset;
          profile_detail::put_fixed(field, at, value, width);
          require(at == offset + width && field == expected, "fixed field store oracle");
          at = offset;
          require(profile_detail::read_fixed(field.view(), at, width) == (value & mask) && at == offset + width, "fixed field load oracle");
        }
      }
    }
  }
  std::string slow_count(std::uint64_t value) {
    if (value == ~std::uint64_t{0}) return std::string(64, '0') + '1' + std::string(64, '0');
    auto code = value + 1;
    unsigned width = 0;
    for (auto n = code; n; n >>= 1) ++width;
    std::string result(width - 1, '0');
    for (unsigned i = width; i; --i) result += ((code >> (i - 1)) & 1) ? '1' : '0';
    return result;
  }
  void count_primitives() {
    std::vector<std::uint64_t> values{0,1,~std::uint64_t{0}};
    for (unsigned bit = 1; bit != 64; ++bit) {
      auto value = std::uint64_t{1} << bit;
      values.insert(values.end(), {value - 1, value, value + 1});
    }
    for (unsigned offset = 0; offset != 8; ++offset) for (auto value : values) {
      auto word = slow_count(value);
      auto expected = bit_string::from_bits(std::string(offset, '1') + word);
      auto actual = bit_string::from_bits(std::string(offset, '1'));
      profile_detail::write_count<bit_policy>(actual, value);
      require(actual == expected, "count encoding independent oracle");
      auto encoded = expected.view().subview(offset, word.size());
      std::uint64_t at = 0;
      require(profile_detail::read_count<bit_policy>(encoded, at) == value && at == word.size(), "shifted count decode");
      if (value == ~std::uint64_t{0}) for (unsigned length = 0; length != 129; ++length) {
        at = 0; rejects([&] { profile_detail::read_count<bit_policy>(encoded.prefix(length), at); });
        require(at == length, "truncated count consumes the available prefix");
      }
      if (value == ~std::uint64_t{0}) {
        for (unsigned suffix = 0; suffix != 64; ++suffix) {
          auto malformed = expected;
          slow_store(malformed.bytes, offset + 65 + suffix, 1);
          at = 0;
          rejects([&] { profile_detail::read_count<bit_policy>(malformed.view().subview(offset,129), at); });
          require(at == 129, "overflowing max count consumes its complete field");
        }
        auto overflow = bit_string::from_bits(std::string(offset + 65, '0'));
        at = 0;
        rejects([&] { profile_detail::read_count<bit_policy>(overflow.view().subview(offset,65), at); });
        require(at == 65, "unary overflow rejects at the first excessive zero");
      }
    }
  }

  void small_count_boundaries() {
    for (unsigned offset = 0; offset != 8; ++offset) for (std::uint64_t value = 0; value <= 256; ++value) {
      auto code = slow_count(value);
      for (unsigned tail = 0; tail != 17; ++tail) {
        auto encoded = bit_string::from_bits(std::string(offset, '1') + "101" + code + std::string(tail, '1'));
        auto data = encoded.view().subview(offset, 3 + code.size() + tail);
        std::uint64_t at = 3;
        require(profile_detail::read_count<bit_policy>(data, at) == value && at == 3 + code.size(),
                "small count full-field/alignment oracle");
      }
      auto encoded = bit_string::from_bits(std::string(offset, '1') + "101" + code);
      auto data = encoded.view().subview(offset, 3 + code.size());
      for (unsigned length = 0; length < code.size(); ++length) {
        std::uint64_t at = 3;
        rejects([&] { profile_detail::read_count<bit_policy>(data.prefix(3 + length), at); });
        require(at == 3 + length, "truncated small count consumed the wrong prefix");
      }
      auto at = data.size() + 1;
      rejects([&] { profile_detail::read_count<bit_policy>(data, at); });
      require(at == data.size() + 1, "out-of-range count offset changed");
    }
  }

#if defined(__unix__) || defined(__APPLE__)
  void guarded_primitives() {
    auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    auto mapping = static_cast<std::byte *>(mmap(nullptr, 3 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    require(mapping != MAP_FAILED, "guard allocation");
    try {
      require(mprotect(mapping + page, page, PROT_READ | PROT_WRITE) == 0, "guard protection");
      for (unsigned offset = 0; offset != 8; ++offset)
        for (unsigned length : {0u,1u,7u,8u,9u,63u,64u,65u,127u,128u,129u,255u,256u,257u,1023u}) {
          auto bytes = (offset + length + 7) / 8;
          auto data = mapping + 2 * page - bytes;
          for (unsigned i = 0; i != bytes; ++i) data[i] = std::byte((i * 19 + 137) & 255);
          bit_view source({data, bytes}, length, offset);
          auto copied = bit_string::copy(source);
          check_comparison(source, copied.view()); check_comparison(copied.view(), source);
          auto target = binary(length + 7, 147);
          profile_detail::copy_into(target, 7, source);
          for (unsigned i = 0; i != length; ++i) require(target.view().at(7 + i) == source.at(i), "guarded bit copy");
          std::uint64_t at = 0;
          auto width = std::min(length, 64u);
          auto expected = std::uint64_t{0};
          for (unsigned i = 0; i != width; ++i) expected = (expected << 1) | unsigned(source.at(i));
          require(profile_detail::read_fixed(source, at, width) == expected, "guarded fixed field");
        }
      for (unsigned offset = 0; offset != 8; ++offset)
        for (std::uint64_t value = 0; value <= 256; ++value)
          for (unsigned tail : {0u, 1u, 15u, 16u}) {
            auto code = slow_count(value);
            auto encoded = bit_string::from_bits(std::string(offset, '0') + code + std::string(tail, '1'));
            auto data = mapping + 2 * page - encoded.bytes.size();
            std::copy(encoded.bytes.begin(), encoded.bytes.end(), data);
            std::uint64_t at = 0;
            require(profile_detail::read_count<bit_policy>(
                      bit_view({data, encoded.bytes.size()}, code.size() + tail, offset), at) == value &&
                    at == code.size(), "guarded small count boundary");
          }
      for (unsigned offset = 0; offset != 8; ++offset) {
        auto word = std::string(offset, '0') + slow_count(~std::uint64_t{0});
        auto encoded = bit_string::from_bits(word);
        auto data = mapping + 2 * page - encoded.bytes.size();
        std::copy(encoded.bytes.begin(), encoded.bytes.end(), data);
        std::uint64_t at = 0;
        require(profile_detail::read_count<bit_policy>(bit_view({data, encoded.bytes.size()},129,offset),at) == ~std::uint64_t{0}, "guarded max count");
      }
    } catch (...) { munmap(mapping, 3 * page); throw; }
    munmap(mapping, 3 * page);
  }
#endif

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

  template <class P, stream_role Role> void check_cursor(profile_array<P, Role> const & array,
                                                        std::span<profile_record const> records) {
    auto cursor = array.view().cursor();
    for (std::size_t i = 0; i != records.size(); ++i) {
      require(!cursor.done() && cursor.ordinal() == i, "cursor ordinal before advance");
      auto item = cursor.peek();
      require(item.ordinal == i && bit_string::copy(item.key.prefix) == records[i].key, "cursor key");
      require(item.key.full_units == records[i].key.bit_size / P::bits_per_unit, "cursor full key length");
      require(bit_string::copy(item.value) == records[i].value, "cursor value");
      auto encoded = array.view().encoded_at(i);
      require(item.value.storage().data() == array.bytes().data() &&
              item.value.storage().size() == array.bytes().size() &&
              item.value.offset() == encoded.value.offset() && item.value.size() == encoded.value.size(),
              "cursor values must borrow the original payload, not a copy");
      auto again = cursor.peek();
      require(again.key.prefix.storage().data() == item.key.prefix.storage().data() &&
              again.value.offset() == item.value.offset(), "peek preserves the current borrowed views");
      // Pausing and moving live traversal state never reconstructs prior keys.
      if (i % (P::group_size + 1) == 0) {
        auto resumed = std::move(cursor);
        require(resumed.ordinal() == i, "cursor move preserves ordinal");
        cursor = std::move(resumed);
      }
      cursor.advance();
    }
    require(cursor.done() && cursor.ordinal() == records.size(), "cursor reaches exact terminal ordinal");
    rejects([&] { (void)cursor.peek(); });
    rejects([&] { cursor.advance(); });
  }

  template <class P> void same_borrowed(profile_array<P, stream_role::borrowed> const & a,
                                       profile_array<P, stream_role::borrowed> const & b) {
    require(std::equal(a.bytes().begin(), a.bytes().end(), b.bytes().begin(), b.bytes().end()),
            "incremental borrowed bytes differ from batch");
    auto const & x = a.metadata();
    auto const & y = b.metadata();
    require(x.version == y.version && x.key_unit == y.key_unit && x.value_unit == y.value_unit &&
            x.count_unit == y.count_unit && x.offset_unit == y.offset_unit && x.count_code == y.count_code &&
            x.backspace_code == y.backspace_code && x.backspace_parameter == y.backspace_parameter &&
            x.bit_order == y.bit_order && x.role == y.role && x.policy_fixed_values == y.policy_fixed_values &&
            x.policy_value_width == y.policy_value_width && x.common_value_width == y.common_value_width &&
            x.group_size == y.group_size && x.record_count == y.record_count && x.extent == y.extent,
            "incremental borrowed metadata differs from batch");
    auto const & u = a.group_offsets();
    auto const & v = b.group_offsets();
    require(u.low == v.low && u.high == v.high && u.sparse == v.sparse && u.low_width == v.low_width &&
            u.entry_count == v.entry_count && u.universe == v.universe && u.samples.size() == v.samples.size(),
            "incremental borrowed EF sections differ from batch");
    for (std::size_t i = 0; i != u.samples.size(); ++i)
      require(u.samples[i].first == v.samples[i].first && u.samples[i].sparse == v.samples[i].sparse,
              "incremental borrowed EF samples differ from batch");
  }

  template <class P> void incremental_borrowed() {
    std::vector<profile_record> records;
    for (auto const & record : fixture<P>()) {
      records.push_back({record.key, {}});
      records.push_back({record.key, {}});
    }
    for (unsigned mode = 0; mode != 3; ++mode) {
      std::vector<std::uint64_t> ceilings;
      if (mode) for (std::size_t i = 0; i != records.size(); ++i)
        ceilings.push_back(mode == 1 ? 0 : i % (records[i].key.bit_size / P::bits_per_unit + 1));
      auto batch = profile_array<P, stream_role::borrowed>::build(records, ceilings);
      for (std::uint64_t step : {std::uint64_t{1}, P::group_size - 1, P::group_size, P::group_size + 1}) {
        profile_borrowed_writer<P> writer;
        std::size_t at = 0;
        auto append_step = [&] {
          auto end = std::min<std::uint64_t>(records.size(), at + step);
          for (; at != end; ++at) {
            // Scratch changes immediately after append: the writer must own
            // its predecessor rather than retaining the caller's key view.
            auto scratch = records[at].key;
            writer.append(scratch.view(), ceilings.empty() ? std::numeric_limits<std::uint64_t>::max() : ceilings[at]);
            scratch = {};
            require(writer.size() == at + 1 && !writer.finished(), "incremental writer progress");
          }
        };
        while (at != records.size()) append_step();
        auto actual = writer.finish();
        require(writer.finished() && writer.size() == records.size(), "incremental writer finished state");
        same_borrowed(actual, batch);
        check_cursor(actual, records);
        rejects([&] { writer.append({}); });
        rejects([&] { (void)writer.finish(); });
      }
    }
    profile_borrowed_writer<P> empty;
    auto result = empty.finish();
    auto batch = profile_array<P, stream_role::borrowed>::build({});
    same_borrowed(result, batch);
    check_cursor(result, {});
    profile_borrowed_writer<P> sorted;
    sorted.append(records.back().key.view());
    rejects([&] { sorted.append(records.front().key.view()); });
    require(sorted.size() == 1, "rejected append preserves progress");
    sorted.append(records.back().key.view());
    std::array<profile_record, 2> last{{records.back(), records.back()}};
    same_borrowed(sorted.finish(), profile_array<P, stream_role::borrowed>::build(last));
    auto shifted_key = bit_string::from_bits("1");
    profile_detail::append(shifted_key, records.back().key.view());
    profile_borrowed_writer<P> shifted;
    shifted.append(shifted_key.view().subview(1, records.back().key.bit_size));
    same_borrowed(shifted.finish(), profile_array<P, stream_role::borrowed>::build(
      std::span<profile_record const>(records).last(1)));
    if constexpr (P::unit == profile_unit::byte) {
      auto unaligned = bit_string::from_bits("101");
      profile_borrowed_writer<P> writer;
      rejects([&] { writer.append(unaligned.view()); });
      require(writer.size() == 0, "unit rejection preserves writer state");
    }
  }

  template <class P> void check_array(profile_array<P> const & array, std::span<profile_record const> records) {
    require(array.size() == records.size(), "profile record count");
    check_cursor(array, records);
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
    auto groups = view.block_count();
    for (std::uint64_t group = 0; group != groups; ++group) {
      auto first = group * P::codec_block_size;
      auto offset = view.block_offset(group);
      require(offset < view.encoded_at(first).next_offset, "sample points before record group prefix");
      require(offset == view.group_offsets().select(group) + first * common, "fixed value stride restoration");
    }
    require(view.block_offset(groups) == array.metadata().extent, "actual-N terminal offset");
    require(array.metadata().group_size == P::group_size, "group policy metadata");
    require(array.metadata().backspace_code == P::backspace_code &&
            array.metadata().backspace_parameter == P::backspace_parameter, "backspace policy metadata");
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
    for (std::uint64_t group = 0; group < fixed.group_offsets().view().size(); ++group)
      require(fixed.group_offsets().view().select(group) == borrowed.group_offsets().view().select(group),
              "constant value bytes excluded from EF universe");
    auto empty = profile_array<P>::build({});
    require(empty.view().size() == 0 && empty.metadata().extent == 0, "empty profile");
    check_cursor(empty, {});
    incremental_borrowed<P>();
    profile_array<P> default_empty;
    require(default_empty.view().size() == 0, "default profile");

    auto wrong = fixed.metadata();
    wrong.group_size = P::group_size == 15 ? 7 : 15;
    rejects([&] { profile_view<P> invalid(fixed.bytes(), fixed.group_offsets().view(), wrong); });
    wrong = fixed.metadata();
    wrong.backspace_code = P::backspace_code == bit_backspace_code::golomb
      ? bit_backspace_code::exponential_golomb : bit_backspace_code::golomb;
    rejects([&] { profile_view<P> invalid(fixed.bytes(), fixed.group_offsets().view(), wrong); });
    wrong = fixed.metadata();
    wrong.backspace_parameter ^= 1;
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
    for (unsigned i = 0; i != 400; ++i)
      records.push_back({prefix, binary(P::value_width.value_or(0) * P::bits_per_unit, i + 1)});
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
    if constexpr (P::fixed_width)
      for (auto & record : records) record.value = binary(*P::value_width * P::bits_per_unit, 99);
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

  template <class Code> using backspace_policy = storage_policy<profile_unit::bit, variable_values, 15, Code>;

  template <class Code> void known_backspace(std::uint64_t value, std::string_view word) {
    using policy = backspace_policy<Code>;
    auto expected = bit_string::from_bits(word);
    bit_string actual;
    profile_detail::write_backspace<policy>(actual, value);
    require(actual == expected, "known backspace bit vector");
    require(profile_detail::backspace_bits<policy>(value) == word.size(), "backspace length prediction");
    std::uint64_t at = 0;
    require(profile_detail::read_backspace<policy>(expected.view(), at) == value && at == word.size(),
            "known backspace vector decoding");
    for (std::uint64_t size = 0; size < word.size(); ++size) {
      at = 0;
      rejects([&] { profile_detail::read_backspace<policy>(expected.view().prefix(size), at); });
    }
    auto shifted = bit_string::from_bits("101");
    profile_detail::write_backspace<policy>(shifted, value);
    require(compare_bits(shifted.view().subview(3, word.size()), expected.view()) == 0,
            "unaligned backspace append changed bits");
    at = 0;
    require(profile_detail::read_backspace<policy>(shifted.view().subview(3, word.size()), at) == value,
            "unaligned backspace decode");
  }

  template <class Code> void backspace_roundtrip(std::uint64_t limit = 1000) {
    using policy = backspace_policy<Code>;
    bit_string encoded;
    std::vector<std::uint64_t> values;
    for (std::uint64_t i = 0; i <= limit; ++i) {
      values.push_back(i);
      profile_detail::write_backspace<policy>(encoded, i);
    }
    if constexpr (policy::backspace_code == bit_backspace_code::golomb) {
      constexpr auto modulus = policy::backspace_parameter;
      for (auto value : {modulus - 1, modulus}) {
        values.push_back(value);
        profile_detail::write_backspace<policy>(encoded, value);
      }
      if constexpr (modulus != std::numeric_limits<std::uint64_t>::max()) {
        values.push_back(modulus + 1);
        profile_detail::write_backspace<policy>(encoded, modulus + 1);
      }
      if constexpr (modulus <= std::numeric_limits<std::uint64_t>::max() / 2) {
        values.push_back(2 * modulus - 1);
        profile_detail::write_backspace<policy>(encoded, 2 * modulus - 1);
      }
    }
    // Large moduli and exponential codes can also cover the entire domain
    // without allocating an impractically large unary quotient.
    if constexpr (policy::backspace_code == bit_backspace_code::exponential_golomb ||
                  policy::backspace_parameter >= (std::uint64_t{1} << 63)) {
      for (auto value : {std::uint64_t{1} << 63, std::numeric_limits<std::uint64_t>::max() - 1,
                         std::numeric_limits<std::uint64_t>::max()}) {
        values.push_back(value);
        profile_detail::write_backspace<policy>(encoded, value);
      }
    }
    std::uint64_t at = 0;
    for (auto value : values) {
      auto start = at;
      require(profile_detail::read_backspace<policy>(encoded.view(), at) == value, "backspace code roundtrip");
      require(at - start == profile_detail::backspace_bits<policy>(value), "backspace consumed length");
    }
    require(at == encoded.bit_size, "backspace stream extent");
    rejects([&] { profile_detail::read_backspace<policy>(encoded.view(), at); });
    at = encoded.bit_size + 1;
    rejects([&] { profile_detail::read_backspace<policy>(encoded.view(), at); });
  }

  template <class Code> void rejects_backspace(std::string_view word) {
    auto bits = bit_string::from_bits(word);
    std::uint64_t at = 0;
    rejects([&] { profile_detail::read_backspace<backspace_policy<Code>>(bits.view(), at); });
  }

  void backspace_codes() {
    static_assert(std::is_same_v<byte_policy::backspace_encoding, exponential_golomb<0>>);
    static_assert(backspace_policy<golomb<3>>::backspace_code == bit_backspace_code::golomb);
    static_assert(backspace_policy<golomb<3>>::backspace_parameter == 3);
    static_assert(backspace_policy<exponential_golomb<63>>::backspace_parameter == 63);
    for (auto [value, word] : std::array<std::pair<std::uint64_t, std::string_view>, 7>{{
           {0, "10"}, {1, "110"}, {2, "111"}, {3, "010"}, {4, "0110"}, {5, "0111"}, {6, "0010"}}})
      known_backspace<golomb<3>>(value, word);
    for (auto [value, word] : std::array<std::pair<std::uint64_t, std::string_view>, 5>{{
           {0, "100"}, {1, "101"}, {2, "110"}, {3, "1110"}, {4, "1111"}}})
      known_backspace<golomb<5>>(value, word);
    for (auto [value, word] : std::array<std::pair<std::uint64_t, std::string_view>, 8>{{
           {0, "100"}, {1, "101"}, {2, "110"}, {3, "111"}, {4, "01000"}, {7, "01011"},
           {8, "01100"}, {12, "0010000"}}})
      known_backspace<exponential_golomb<2>>(value, word);
    known_backspace<golomb<1>>(0, "1");
    known_backspace<golomb<1>>(4, "00001");
    known_backspace<golomb<4>>(5, "0101");
    known_backspace<exponential_golomb<1>>(0, "10");
    known_backspace<exponential_golomb<1>>(3, "0101");
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    known_backspace<exponential_golomb<0>>(maximum, std::string(64, '0') + '1' + std::string(64, '0'));
    known_backspace<exponential_golomb<63>>(maximum, "010" + std::string(63, '1'));
    known_backspace<golomb<maximum>>(0, "1" + std::string(63, '0'));
    known_backspace<golomb<maximum>>(1, "1" + std::string(62, '0') + "10");
    known_backspace<golomb<maximum>>(maximum - 1, "1" + std::string(64, '1'));
    known_backspace<golomb<maximum>>(maximum, "01" + std::string(63, '0'));
    backspace_roundtrip<golomb<1>>(127);
    backspace_roundtrip<golomb<2>>();
    backspace_roundtrip<golomb<3>>();
    backspace_roundtrip<golomb<5>>();
    backspace_roundtrip<golomb<9>>();
    backspace_roundtrip<golomb<17>>();
    backspace_roundtrip<golomb<127>>();
    backspace_roundtrip<golomb<65535>>();
    backspace_roundtrip<golomb<(std::uint64_t{1} << 63)>>();
    backspace_roundtrip<golomb<(std::uint64_t{1} << 63) + 1>>();
    backspace_roundtrip<golomb<maximum>>();
    backspace_roundtrip<exponential_golomb<0>>();
    backspace_roundtrip<exponential_golomb<1>>();
    backspace_roundtrip<exponential_golomb<2>>();
    backspace_roundtrip<exponential_golomb<7>>();
    backspace_roundtrip<exponential_golomb<31>>();
    backspace_roundtrip<exponential_golomb<63>>();
    // Impossible Golomb1 length is rejected before allocation or unary work.
    bit_string target = bit_string::from_bits("101");
    auto saved = target;
    rejects([&] { profile_detail::write_backspace<backspace_policy<golomb<1>>>(target, maximum); });
    require(target == saved, "overflowing backspace changed output");
    rejects([&] { profile_detail::write_backspace<backspace_policy<golomb<1>>>(target, maximum - 1); });
    require(target == saved, "overflowing combined extent changed output");
    rejects([&] { profile_detail::backspace_bits<backspace_policy<golomb<1>>>(maximum); });
    require(profile_detail::backspace_bits<backspace_policy<golomb<2>>>(maximum) ==
            (std::uint64_t{1} << 63) + 1, "large representable Golomb length");
    rejects_backspace<golomb<3>>("");
    rejects_backspace<golomb<3>>("00000");
    rejects_backspace<golomb<3>>("11"); // Long truncated remainder is missing its last bit.
    rejects_backspace<golomb<maximum>>("00"); // Quotient alone is already out of range.
    rejects_backspace<golomb<maximum>>("01" + std::string(62, '0') + "10"); // max + 1.
    rejects_backspace<exponential_golomb<63>>("011"); // Quotient 2 cannot fit after shifting.
    rejects_backspace<exponential_golomb<0>>(std::string(65, '0'));
    rejects_backspace<exponential_golomb<0>>(std::string(64, '0') + '1' + std::string(63, '0') + '1');
    // The default backspace encoding stays byte-for-byte identical to the
    // existing count codec, including max and representative short values.
    for (auto value : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{7}, std::uint64_t{255}, maximum}) {
      auto check = [&]<class P>() {
        bit_string before, after;
        profile_detail::write_count<P>(before, value);
        profile_detail::write_backspace<P>(after, value);
        require(before == after, "default backspace changed existing bytes");
      };
      check.template operator()<byte_policy>();
      check.template operator()<bit_policy>();
    }
  }

  void default_profile_bytes() {
    std::vector<profile_record> bytes{{bit_string::from_bytes("ab"), {}},
                                      {bit_string::from_bytes("ac"), {}}};
    auto byte_array = profile_array<byte_policy>::build(bytes);
    std::array<std::byte, 8> byte_expected{std::byte{0}, std::byte{0}, std::byte{2}, std::byte{'a'},
      std::byte{'b'}, std::byte{1}, std::byte{1}, std::byte{'c'}};
    require(std::ranges::equal(byte_array.bytes(), byte_expected), "default byte profile golden encoding");
    std::vector<profile_record> bits{{bit_string::from_bits("0010"), {}},
                                     {bit_string::from_bits("0011"), {}}};
    auto bit_array = profile_array<bit_policy>::build(bits);
    auto bit_expected = bit_string::from_bits("1" "1" "00101" "0010" "010" "010" "1");
    require(bit_array.metadata().extent == bit_expected.bit_size &&
            std::ranges::equal(bit_array.bytes(), bit_expected.bytes), "default bit profile golden encoding");
  }

  template <class P> void coded_profiles() {
    roundtrip<P>();
    lpfc<P>();
    surrogate_anchor<P>();
    bit_string bad;
    profile_detail::write_count<P>(bad, 0); // Group checkpoint retains exp0.
    profile_detail::write_backspace<P>(bad, 1);
    profile_detail::write_count<P>(bad, 0); // Suffix length retains exp0.
    auto metadata = profile_detail::initial_metadata<P, stream_role::borrowed>();
    metadata.record_count = 1;
    metadata.extent = bad.bit_size;
    metadata.common_value_width = 0;
    std::array<std::uint64_t, 2> offsets{0, bad.bit_size};
    auto index = elias_fano::build(offsets);
    profile_view<P, stream_role::borrowed> view(bad.bytes, index.view(), metadata);
    rejects([&] { view.cursor(); });
  }

  void long_golomb_backspace() {
    using policy = backspace_policy<golomb<1>>;
    std::vector<profile_record> records{{bit_string::from_bits(std::string(4096, '0')), {}},
                                        {bit_string::from_bits("1"), {}}};
    auto array = profile_array<policy>::build(records, {}, 18);
    auto view = array.view();
    auto first = view.encoded_at(0);
    auto second = view.encoded_at(1);
    require(second.retained == 0 && second.key_units == 1, "short literal key after long predecessor");
    auto at = first.next_offset;
    require(profile_detail::read_backspace<policy>(bit_view(array.bytes(), array.metadata().extent), at) == 4096 &&
            at - first.next_offset == 4097, "Golomb backspace cost must include unary payload");
    require(view.reconstruct_at(1).prefix == records[1].key, "long Golomb predecessor reconstruction");
  }

  template <class P> void malformed_streams() {
    // A well-shaped sampled directory cannot make an invalid record header valid.
    auto make_view = [&](bit_string const & data, std::uint64_t records = 1) {
      auto metadata = profile_detail::initial_metadata<P, stream_role::native>();
      metadata.record_count = records;
      metadata.extent = data.bit_size / P::bits_per_unit;
      metadata.common_value_width = 0;
      std::array<std::uint64_t, 2> offsets{0, metadata.extent};
      auto index = elias_fano::build(offsets);
      profile_view<P> view(data.bytes, index.view(), metadata);
      rejects([&] {
        auto cursor = view.cursor();
        while (!cursor.done()) cursor.advance();
      });
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
    for (std::uint64_t group = 0; group < array.group_offsets().view().size(); ++group)
      wrong_offsets.push_back(array.group_offsets().view().select(group));
    wrong_offsets[0] = 1;
    auto leading = elias_fano::build(wrong_offsets);
    rejects([&] { profile_view<P> invalid(array.bytes(), leading.view(), array.metadata()); });
    auto shape_leading = profile_view<P>::from_sections(array.bytes(), leading.view(), array.metadata());
    rejects([&] { shape_leading.validate_contents(); });
    wrong_offsets[0] = 0;
    ++wrong_offsets[1];
    auto inconsistent = elias_fano::build(wrong_offsets);
    profile_view<P> wrong_samples(array.bytes(), inconsistent.view(), array.metadata());
    rejects([&] { wrong_samples.visit_all([](auto) { return true; }); });
    rejects([&] {
      auto cursor = wrong_samples.cursor();
      while (!cursor.done()) cursor.advance();
    });
    // Change only a group's predecessor checkpoint. Its encoded size and
    // sampled address stay unchanged, so sequential context must reject it.
    auto checkpoint_bytes = std::vector<std::byte>(array.bytes().begin(), array.bytes().end());
    auto group_start = array.view().block_offset(1);
    auto group_data = bit_view(checkpoint_bytes, array.metadata().extent * P::bits_per_unit);
    auto after_checkpoint = group_start;
    (void)profile_detail::read_count<P>(group_data, after_checkpoint);
    if constexpr (P::unit == profile_unit::byte) checkpoint_bytes[group_start] ^= std::byte{1};
    else {
      auto bit = after_checkpoint - 1;
      checkpoint_bytes[bit / 8] ^= static_cast<std::byte>(1u << (7 - bit % 8));
    }
    profile_view<P> wrong_checkpoint(checkpoint_bytes, array.group_offsets().view(), array.metadata());
    rejects([&] {
      auto cursor = wrong_checkpoint.cursor();
      while (!cursor.done()) cursor.advance();
    });
    auto bad_bytes = std::vector<std::byte>(array.bytes().begin(), array.bytes().end());
    auto metadata = array.metadata();
    if constexpr (P::unit == profile_unit::bit) {
      std::vector<profile_record> singleton{{{}, {}}};
      auto tail = profile_array<P>::build(singleton);
      auto bytes = std::vector<std::byte>(tail.bytes().begin(), tail.bytes().end());
      require(tail.metadata().extent % 8 != 0, "partial-byte fixture");
      bytes.back() |= std::byte{1};
      rejects([&] { profile_view<P> invalid(bytes, tail.group_offsets().view(), tail.metadata()); });
      auto shape_padding = profile_view<P>::from_sections(bytes, tail.group_offsets().view(), tail.metadata());
      rejects([&] { shape_padding.validate_contents(); });
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

  template <class P, stream_role Role> void mapped_profile_sections() {
    std::vector<profile_record> records;
    for (unsigned i = 0; i < 33; ++i) {
      auto key = bit_string::from_bytes("prefix/" + std::to_string(100 + i));
      bit_string value;
      if constexpr (Role == stream_role::native) {
        if constexpr (P::fixed_width) value = binary(*P::value_width * P::bits_per_unit, i + 1);
        else value = binary((i % 5) * P::bits_per_unit, i + 1);
      }
      records.push_back({std::move(key), std::move(value)});
    }
    auto array = profile_array<P, Role>::build(records);
    auto const & ef = array.group_offsets();
    std::array<std::vector<std::byte>, 5> sections;
    sections[0].assign(array.bytes().begin(), array.bytes().end());
    auto encode = [](std::span<std::uint64_t const> words) {
      std::vector<std::byte> result(words.size() * 8);
      for (std::size_t i = 0; i < words.size(); ++i)
        for (unsigned j = 0; j < 8; ++j) result[i * 8 + j] = std::byte((words[i] >> (j * 8)) & 255);
      return result;
    };
    sections[1] = encode(ef.low); sections[2] = encode(ef.high); sections[4] = encode(ef.sparse);
    std::vector<std::uint64_t> sample_words;
    for (auto sample : ef.samples) { sample_words.push_back(sample.first); sample_words.push_back(sample.sparse); }
    sections[3] = encode(sample_words);
    auto inspect = [&](std::array<std::span<std::byte const>, 5> spans, bool read) {
      elias_fano_view offsets(
        word_view::little_endian(spans[1]), word_view::little_endian(spans[2]),
        sample_view::little_endian(spans[3]), word_view::little_endian(spans[4]),
        ef.entry_count, ef.universe, ef.low_width);
      auto view = profile_view<P, Role>::from_sections(spans[0], offsets, array.metadata());
      require(view.bytes().data() == spans[0].data() && view.size() == records.size(), "mapped profile retains payload");
      require(view.group_offsets().samples().bytes().data() == spans[3].data(), "mapped profile retains samples");
      auto bad = array.metadata(); ++bad.codec_block_size;
      rejects([&] { (void)profile_view<P, Role>::from_sections(spans[0], offsets, bad); });
      if (!read) return;
      view.validate_contents();
      auto cursor = view.cursor();
      for (std::size_t i = 0; i < records.size(); ++i) {
        require(!cursor.done(), "mapped cursor early EOF");
        auto item = cursor.peek();
        require(compare_bits(item.key.prefix, records[i].key.view()) == 0 &&
                compare_bits(item.value, records[i].value.view()) == 0, "mapped sequential profile oracle");
        auto encoded = view.encoded_at(i);
        require(compare_bits(encoded.value, records[i].value.view()) == 0, "mapped selected value oracle");
        cursor.advance();
      }
      require(cursor.done() && view.predecessor_units(view.size()) == array.metadata().terminal_key_units,
              "mapped terminal key length");
    };
    for (unsigned offset = 0; offset < 8; ++offset) {
      std::array<std::vector<std::byte>, 5> unaligned;
      std::array<std::span<std::byte const>, 5> spans;
      for (std::size_t i = 0; i < sections.size(); ++i) {
        unaligned[i].resize(offset, std::byte{0x97});
        unaligned[i].insert(unaligned[i].end(), sections[i].begin(), sections[i].end());
        spans[i] = std::span(unaligned[i]).subspan(offset);
      }
      inspect(spans, true);
    }
#if defined(__unix__) || defined(__APPLE__)
    auto page = std::size_t(sysconf(_SC_PAGESIZE));
    auto raw = mmap(nullptr, page * 5, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    require(raw != MAP_FAILED, "mapped profile guard allocation");
    struct cleanup {
      void * memory;
      std::size_t size;
      ~cleanup() { munmap(memory, size); }
    } guard{raw, page * 5};
    std::array<std::span<std::byte const>, 5> unread;
    for (std::size_t i = 0; i < sections.size(); ++i) {
      require(sections[i].size() + 1 <= page, "mapped profile fixture fits page");
      unread[i] = {static_cast<std::byte const *>(raw) + i * page + 1, sections[i].size()};
    }
    // Every directory and payload page is inaccessible: even an EF endpoint
    // or the final stream-padding byte would fault during shape construction.
    inspect(unread, false);
    require(mprotect(raw, page * 5, PROT_READ | PROT_WRITE) == 0, "mapped profile unprotect");
    for (std::size_t i = 0; i < sections.size(); ++i)
      std::copy(sections[i].begin(), sections[i].end(), static_cast<std::byte *>(raw) + i * page + 1);
    inspect(unread, true);
#endif
  }

  template <class P, stream_role Role> void owning_views() {
    using array = profile_array<P, Role>;
    array default_array;
    require(default_array.view().size() == 0, "default empty owner has no usable view");
    default_array.view().validate_contents();
    auto empty = array::build({});
    require(empty.view().size() == 0, "built empty owner has no usable view");
    empty.view().validate_contents();

    auto value = [&] {
      if constexpr (Role == stream_role::borrowed) return bit_string{};
      else return binary(P::value_width.value_or(2) * P::bits_per_unit, 991);
    }();
    std::vector<profile_record> records{
      {bit_string::from_bytes("shared/a"), value}, {bit_string::from_bytes("shared/b"), value}};
    auto source = array::build(records);
    auto copy = source;
    auto moved = std::move(source);
    rejects([&] { (void)source.view(); });
    require(copy.bytes().data() != moved.bytes().data(), "copied owner aliases its payload allocation");
    source = array::build({});
    for (auto const * owner : {&copy, &moved}) {
      auto view = owner->view();
      view.validate_contents();
      std::size_t at = 0;
      view.visit_all([&](profile_item<P> item) {
        require(at < records.size() && compare_bits(item.key.prefix, records[at].key.view()) == 0 &&
                compare_bits(item.value, records[at].value.view()) == 0, "copied/moved owner view changed records");
        ++at;
        return true;
      });
      require(at == records.size(), "copied/moved owner view lost records");
      // Public readers still validate supplied contents at construction.
      auto metadata = owner->metadata();
      auto offsets = owner->group_offsets();
      offsets.high.assign(offsets.high.size(), 0);
      rejects([&] { profile_view<P, Role> checked(owner->bytes(), offsets.view(), metadata); });
    }
  }

  template <class P> void offset_metadata() {
    static_assert(!P::fixed_width);
    auto records = fixture<P>(true);
    // W is independent of the virtual sampling stride K. Include a short
    // physical tail, so the EOF stride must use the actual record count.
    records.resize(2 * P::codec_block_size + 1);
    auto array = profile_array<P>::build(records);
    auto view = array.view();
    auto width = array.metadata().common_value_width.value_or(0);
    require(width != 0 && view.block_count() == 3, "offset metadata fixture");
    require(view.group_offsets().size() == 4, "profile must encode explicit EOF in generic EF");
    view.validate_offset_metadata();
    view.validate_contents();
    for (std::uint64_t block = 0; block <= view.block_count(); ++block) {
      auto ordinal = std::min(block * P::codec_block_size, view.size());
      require(view.block_offset(block) == view.group_offsets().select(block) + ordinal * width,
              "profile-owned fixed stride arithmetic");
    }
    require(view.block_offset(3) == array.metadata().extent, "short tail actual-N EOF");
    rejects([&] { (void)view.block_offset(4); });

    auto wrong = array.metadata();
    ++wrong.common_value_width.value();
    auto mismatch = profile_view<P>::from_sections(array.bytes(), array.group_offsets().view(), wrong);
    rejects([&] { mismatch.validate_offset_metadata(); });
    rejects([&] { mismatch.validate_contents(); });
    wrong.common_value_width = std::numeric_limits<std::uint64_t>::max();
    auto overflow = profile_view<P>::from_sections(array.bytes(), array.group_offsets().view(), wrong);
    rejects([&] { overflow.validate_offset_metadata(); });
    rejects([&] { overflow.validate_contents(); });

    std::vector<std::uint64_t> offsets;
    for (std::uint64_t i = 0; i != view.group_offsets().size(); ++i)
      offsets.push_back(view.group_offsets().select(i));
    auto missing = offsets; missing.pop_back();
    auto no_eof = elias_fano::build(missing);
    rejects([&] { (void)profile_view<P>::from_sections(array.bytes(), no_eof.view(), array.metadata()); });
    auto extra = offsets; extra.push_back(extra.back());
    auto two_eof = elias_fano::build(extra);
    rejects([&] { (void)profile_view<P>::from_sections(array.bytes(), two_eof.view(), array.metadata()); });
    for (auto & value : offsets) ++value;
    auto shifted = elias_fano::build(offsets);
    auto bad_universe = profile_view<P>::from_sections(array.bytes(), shifted.view(), array.metadata());
    rejects([&] { bad_universe.validate_offset_metadata(); });

    elias_fano empty;
    require(empty.view().size() == 0, "generic EF defaults to no values");
    profile_array<P> empty_profile;
    auto empty_view = empty_profile.view();
    require(empty_view.block_count() == 0 && empty_view.group_offsets().size() == 1 &&
            empty_view.block_offset(0) == 0, "empty profile explicitly retains EOF");
    empty_view.validate_contents();
    rejects([&] { (void)profile_view<P>::from_sections({}, empty.view(), empty_profile.metadata()); });
  }

  void rounded_bit_extents() {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    require(profile_detail::byte_count(maximum - 7) == (maximum >> 3),
            "largest rounded bit extent");
    for (std::uint64_t bits = 0; bits != 130; ++bits)
      require(profile_detail::byte_count(bits) == bits / 8 + (bits % 8 != 0),
              "rounded bit extent differs from quotient/remainder oracle");
    auto saved = bit_string::from_bits("10101");
    for (std::uint64_t tail = 0; tail != 7; ++tail) {
      bit_string malformed{{}, maximum - tail};
      rejects([&] { malformed.validate(); });
      auto target = saved;
      rejects([&] { profile_detail::resize(target, maximum - tail); });
      require(target == saved, "unrepresentable rounded resize changed destination");
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
    rounded_bit_extents();
    subview_oracle();
    byte_comparison_oracle();
    offset_metadata<storage_policy<profile_unit::byte, variable_values, 7, exponential_golomb<0>, 16>>();
    offset_metadata<storage_policy<profile_unit::bit, variable_values, 3, golomb<3>, 7>>();
    intermediate_byte_anchors();
    bit_primitives();
    count_primitives();
    small_count_boundaries();
#if defined(__unix__) || defined(__APPLE__)
    guarded_primitives();
#endif
    counts();
    backspace_codes();
    default_profile_bytes();
    long_golomb_backspace();
    coded_profiles<storage_policy<profile_unit::bit, variable_values, 3, golomb<3>>>();
    coded_profiles<storage_policy<profile_unit::bit, fixed_values<3>, 7, golomb<5>>>();
    coded_profiles<storage_policy<profile_unit::bit, fixed_values<0>, 15, exponential_golomb<2>>>();
    coded_profiles<storage_policy<profile_unit::bit, variable_values, 31, exponential_golomb<63>>>();
    mapped_profile_sections<storage_policy<profile_unit::byte, variable_values, 3, exponential_golomb<0>, 16>, stream_role::native>();
    mapped_profile_sections<storage_policy<profile_unit::byte, fixed_values<3>, 15, exponential_golomb<0>, 7>, stream_role::native>();
    mapped_profile_sections<storage_policy<profile_unit::bit, variable_values, 7, golomb<3>, 16>, stream_role::native>();
    mapped_profile_sections<storage_policy<profile_unit::bit, fixed_values<3>, 31, exponential_golomb<2>, 1>, stream_role::native>();
    mapped_profile_sections<storage_policy<profile_unit::byte, fixed_values<3>, 15, exponential_golomb<0>, 16>, stream_role::borrowed>();
    mapped_profile_sections<storage_policy<profile_unit::bit, fixed_values<3>, 15, golomb<7>, 16>, stream_role::borrowed>();
    owning_views<storage_policy<profile_unit::byte, variable_values, 15>, stream_role::native>();
    owning_views<storage_policy<profile_unit::bit, variable_values, 7, golomb<3>, 16>, stream_role::native>();
    owning_views<storage_policy<profile_unit::byte, fixed_values<3>, 15>, stream_role::borrowed>();
    owning_views<storage_policy<profile_unit::bit, fixed_values<3>, 15>, stream_role::borrowed>();
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
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Everett's profile behavior.
 */
