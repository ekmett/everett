/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Diet's profile support.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <diet/error_detail.h>

#include <diet/policy.h>
#include <diet/key_detail.h>
#include <diet/elias_fano.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace diet {
  namespace profile_detail {
    inline std::uint64_t add(std::uint64_t a, std::uint64_t b) {
      if (b > std::numeric_limits<std::uint64_t>::max() - a)
        error_detail::raise<std::overflow_error>("profile addition");
      return a + b;
    }
    inline std::uint64_t multiply(std::uint64_t a, std::uint64_t b) {
      if (b && a > std::numeric_limits<std::uint64_t>::max() / b)
        error_detail::raise<std::overflow_error>("profile multiplication");
      return a * b;
    }
    // Admitted bit extents leave room for the final byte rounding.
    inline std::uint64_t byte_count(std::uint64_t bits) noexcept {
      return (bits + 7) >> 3;
    }
  }

  // MSB-first meaningful bits. A subview may begin inside its first byte.
  struct bit_view {
    bit_view() = default;
    bit_view(std::span<std::byte const> bytes, std::uint64_t bits, std::uint64_t offset = 0)
      : bytes_(bytes), offset_(offset), size_(bits) {
      auto capacity = profile_detail::multiply(bytes.size(), 8);
      if (offset > capacity || bits > capacity - offset)
        error_detail::raise<std::invalid_argument>("bit view exceeds storage");
    }
    std::uint64_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    std::uint64_t offset() const noexcept { return offset_; }
    std::span<std::byte const> storage() const noexcept { return bytes_; }
    bool at(std::uint64_t i) const {
      if (i >= size_) error_detail::raise<std::out_of_range>("bit position");
      auto bit = offset_ + i;
      return (std::to_integer<unsigned>(bytes_[bit >> 3]) >> (7 - (bit & 7))) & 1;
    }
    bit_view subview(std::uint64_t first, std::uint64_t count) const {
      if (first > size_ || count > size_ - first) error_detail::raise<std::out_of_range>("bit subview");
      auto result = *this;
      result.offset_ += first;
      result.size_ = count;
      return result;
    }
    bit_view prefix(std::uint64_t count) const { return subview(0, std::min(count, size_)); }

  private:
    std::span<std::byte const> bytes_;
    std::uint64_t offset_ = 0;
    std::uint64_t size_ = 0;
  };

  namespace profile_detail {
    inline std::uint64_t low_mask(unsigned width) noexcept {
      return width == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << width) - 1;
    }
    // A bounded MSB-first field, returned in the low width bits. Width <= 64.
    inline std::uint64_t load_bits(bit_view data, std::uint64_t first, unsigned width) noexcept {
      if (!width) return 0;
      auto offset = data.offset() + first;
      auto source = data.storage().data() + (offset >> 3);
      unsigned shift = unsigned(offset & 7), bytes = (shift + width + 7) >> 3;
      if (bytes >= 8) {
        auto value = key_detail::load_big(source);
        if (shift) {
          value <<= shift;
          if (bytes == 9) value |= std::uint64_t(std::to_integer<unsigned>(source[8])) >> (8 - shift);
        }
        return width == 64 ? value : value >> (64 - width);
      }
      std::uint64_t value = 0;
      for (unsigned i = 0; i != bytes; ++i) value = (value << 8) | std::to_integer<unsigned>(source[i]);
      return (value >> ((bytes << 3) - shift - width)) & low_mask(width);
    }
    // Preserve both edge bytes outside the field. The caller provides capacity.
    inline void store_bits(std::byte * target, std::uint64_t at, std::uint64_t value, unsigned width) noexcept {
      if (!width) return;
      if (width == 64 && (at & 7) == 0) { key_detail::store_big(target + (at >> 3), value); return; }
      while (width) {
        auto take = std::min(width, 8u - unsigned(at & 7));
        auto shift = 8u - unsigned(at & 7) - take;
        auto mask = unsigned(low_mask(take)) << shift;
        auto bits = unsigned((value >> (width - take)) & low_mask(take)) << shift;
        target[at >> 3] = std::byte((std::to_integer<unsigned>(target[at >> 3]) & ~mask) | bits);
        at += take; width -= take;
      }
    }
    inline void copy_bits(std::byte * target, std::uint64_t first, bit_view source) noexcept {
      auto count = source.size();
      if (!count) return;
      if (((first | source.offset() | count) & 7) == 0) {
        std::memmove(target + (first >> 3), source.storage().data() + (source.offset() >> 3),
          static_cast<std::size_t>(count >> 3));
        return;
      }
      // Detect overlapping storage without ordering unrelated C++ pointers.
      auto destination = reinterpret_cast<std::uintptr_t>(target + (first >> 3));
      auto origin = reinterpret_cast<std::uintptr_t>(source.storage().data() + (source.offset() >> 3));
      auto source_bytes = byte_count((source.offset() & 7) + count);
      bool backwards = (destination > origin || (destination == origin && (first & 7) > (source.offset() & 7))) &&
        destination - origin < source_bytes;
      if (backwards) {
        while (count) {
          auto width = unsigned(std::min<std::uint64_t>(count, 64));
          count -= width;
          store_bits(target, first + count, load_bits(source, count, width), width);
        }
        return;
      }
      std::uint64_t at = 0;
      if (first & 7) {
        auto width = unsigned(std::min<std::uint64_t>(count, 8 - (first & 7)));
        store_bits(target, first, load_bits(source, 0, width), width); at += width;
      }
      if (((source.offset() + at) & 7) == 0) {
        auto bytes = (count - at) >> 3;
        if (bytes) std::memmove(target + ((first + at) >> 3),
          source.storage().data() + ((source.offset() + at) >> 3), static_cast<std::size_t>(bytes));
        at += bytes << 3;
      } else {
        for (; count - at >= 64; at += 64)
          key_detail::store_big(target + ((first + at) >> 3), load_bits(source, at, 64));
      }
      while (at != count) {
        auto width = unsigned(std::min<std::uint64_t>(count - at, 8));
        store_bits(target, first + at, load_bits(source, at, width), width); at += width;
      }
    }
  }

  struct bit_string {
    std::vector<std::byte> bytes;
    std::uint64_t bit_size = 0;

    void validate() const {
      if (bit_size > std::numeric_limits<std::uint64_t>::max() - 7 ||
          bytes.size() != profile_detail::byte_count(bit_size))
        error_detail::raise<std::invalid_argument>("packed bit-string length");
      if ((bit_size & 7) &&
          (std::to_integer<unsigned>(bytes.back()) & ((1u << (8 - (bit_size & 7))) - 1)))
        error_detail::raise<std::invalid_argument>("nonzero bit-string padding");
    }
    bit_view view() const & { validate(); return {bytes, bit_size}; }
    bit_view view() const && = delete;

    static bit_string copy(bit_view source) {
      bit_string result;
      result.bit_size = source.size();
      result.bytes.resize(static_cast<std::size_t>(profile_detail::byte_count(source.size())));
      profile_detail::copy_bits(result.bytes.data(), 0, source);
      return result;
    }
    static bit_string from_bytes(std::span<std::byte const> source) {
      return {{source.begin(), source.end()}, profile_detail::multiply(source.size(), 8)};
    }
    static bit_string from_bytes(std::string_view source) {
      return from_bytes(std::as_bytes(std::span(source.data(), source.size())));
    }
    static bit_string from_bits(std::string_view source) {
      if (source.size() > std::numeric_limits<std::uint64_t>::max() - 7)
        error_detail::raise<std::length_error>("profile bit string too large");
      bit_string result;
      result.bit_size = source.size();
      result.bytes.resize(static_cast<std::size_t>(profile_detail::byte_count(source.size())));
      for (std::size_t i = 0; i != source.size(); ++i) {
        if (source[i] != '0' && source[i] != '1') error_detail::raise<std::invalid_argument>("expected binary digits");
        if (source[i] == '1') result.bytes[i >> 3] |= static_cast<std::byte>(1u << (7 - (i & 7)));
      }
      return result;
    }
    bool operator==(bit_string const &) const = default;
  };

  struct bit_comparison {
    std::uint64_t common_bits = 0;
    int order = 0;
  };
#if defined(__APPLE__) && defined(__aarch64__) && defined(__clang__)
  // Reduce the out-of-line NEON loop's sensitivity to caller code layout.
  [[gnu::aligned(64)]]
#endif
  inline bit_comparison compare_common_bits(bit_view a, bit_view b) {
    auto count = std::min(a.size(), b.size());
    // Most unrelated keys can differ immediately; do not load a whole word
    // just to discover a mismatch in the first meaningful bit.
    if (count) {
      auto x = (std::to_integer<unsigned>(a.storage()[a.offset() >> 3]) >> (7 - (a.offset() & 7))) & 1;
      auto y = (std::to_integer<unsigned>(b.storage()[b.offset() >> 3]) >> (7 - (b.offset() & 7))) & 1;
      if (x != y) return {0, x ? 1 : -1};
    }
    std::uint64_t at = 0;
    if (count >= 8 && (a.offset() & 7) == 0 && (b.offset() & 7) == 0) {
      at = key_detail::common_bytes(a.storage().data() + (a.offset() >> 3),
                                   b.storage().data() + (b.offset() >> 3), static_cast<std::size_t>(count >> 3)) << 3;
      if (count - at >= 8) {
        auto x = profile_detail::load_bits(a, at, 8), y = profile_detail::load_bits(b, at, 8);
        return {at + std::countl_zero(x ^ y) - 56, x < y ? -1 : 1};
      }
    }
    while (at != count) {
      auto width = unsigned(std::min<std::uint64_t>(count - at, 64));
      auto x = profile_detail::load_bits(a, at, width), y = profile_detail::load_bits(b, at, width);
      if (auto different = x ^ y)
        return {at + std::countl_zero(different) - (64 - width), x < y ? -1 : 1};
      at += width;
    }
    return {count, a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0};
  }
  inline int compare_bits(bit_view a, bit_view b) { return compare_common_bits(a, b).order; }
  template <class P> std::uint64_t common_prefix_units(bit_view a, bit_view b) {
    if ((a.size() & (P::bits_per_unit - 1)) || (b.size() & (P::bits_per_unit - 1)))
      error_detail::raise<std::invalid_argument>("key length does not match profile unit");
    return compare_common_bits(a, b).common_bits >> P::unit_shift;
  }

  template <class P> struct profile_anchor {
    bit_view prefix;
    std::uint64_t full_units = 0;
    static profile_anchor complete(bit_view key) {
      if (key.size() & (P::bits_per_unit - 1)) error_detail::raise<std::invalid_argument>("anchor unit mismatch");
      return {key, (key.size() >> P::unit_shift)};
    }
  };

  template <class P> int compare_profile_prefix(profile_anchor<P> key, bit_view query) {
    if ((query.size() & (P::bits_per_unit - 1)) || (key.prefix.size() & (P::bits_per_unit - 1)))
      error_detail::raise<std::invalid_argument>("query unit mismatch");
    auto query_units = (query.size() >> P::unit_shift);
    auto needed = profile_detail::multiply(std::min(key.full_units, query_units), P::bits_per_unit);
    if (key.prefix.size() < needed) error_detail::raise<std::invalid_argument>("query prefix is incomplete");
    auto order = compare_bits(key.prefix.prefix(needed), query.prefix(needed));
    if (order) return order;
    return key.full_units < query_units ? -1 : key.full_units > query_units ? 1 : 0;
  }

  enum class profile_count_code : std::uint8_t { varint, exp_golomb_zero };
  enum class profile_bit_order : std::uint8_t { msb_first };

  struct profile_metadata {
    std::uint32_t version = 2;
    profile_unit key_unit = profile_unit::byte;
    profile_unit value_unit = profile_unit::byte;
    profile_unit count_unit = profile_unit::byte;
    profile_unit offset_unit = profile_unit::byte;
    profile_count_code count_code = profile_count_code::varint;
    bit_backspace_code backspace_code = bit_backspace_code::exponential_golomb;
    std::uint64_t backspace_parameter = 0;
    profile_bit_order bit_order = profile_bit_order::msb_first;
    stream_role role = stream_role::native;
    bool policy_fixed_values = false;
    std::uint64_t policy_value_width = 0;
    std::optional<std::uint64_t> common_value_width = 0;
    std::uint64_t group_size = 15;
    std::uint64_t codec_block_size = 15;
    std::uint64_t terminal_key_units = 0;
    std::uint64_t record_count = 0;
    std::uint64_t extent = 0;
  };

  struct profile_record {
    bit_string key;
    bit_string value;
  };

  template <class P> struct profile_item {
    std::uint64_t ordinal = 0;
    profile_anchor<P> key;
    bit_view value;
  };

  struct profile_encoded_record {
    std::uint64_t retained = 0;
    std::uint64_t key_units = 0;
    std::uint64_t value_units = 0;
    bit_view suffix;
    bit_view value;
    std::uint64_t next_offset = 0;
  };

  template <class P> struct profile_decoded_record {
    bit_string prefix;
    std::uint64_t full_units = 0;
    bit_string value;
    profile_anchor<P> anchor() const & { return {prefix.view(), full_units}; }
    profile_anchor<P> anchor() const && = delete;
  };

  // A comparison is inseparable from its owned query. Agreement counts bits;
  // record lengths/backspaces still count P units. No prefix bytes are implied.
  template <class P> struct profile_query_context {
    explicit profile_query_context(bit_view query) {
      if (query.size() & (P::bits_per_unit - 1)) error_detail::raise<std::invalid_argument>("query unit mismatch");
      query_ = std::make_shared<bit_string const>(bit_string::copy(query));
      order_ = query.size() ? -1 : 0;
    }
    bit_view query() const {
      if (!query_) error_detail::raise<std::logic_error>("comparison context has no query");
      return query_->view();
    }
    std::uint64_t common_bits() const noexcept { return common_bits_; }
    // A repaired lower frontier needs only order and LCP. Its full length is
    // available after reading a frame or establishing equality with the query.
    std::optional<std::uint64_t> full_units() const noexcept {
      return length_known_ ? std::optional(full_units_) : std::nullopt;
    }
    int order() const noexcept { return order_; }

    // A heterogeneous record grammar can expose its logical suffix as a few
    // borrowed spans. Comparisons retain only order, length and agreement with
    // the query; inherited key bytes need not be reconstructed.
    std::uint64_t advance_parts(std::uint64_t retained_bits, std::uint64_t full_bits,
                               std::span<bit_view const> literal) {
      if ((full_bits & (P::bits_per_unit - 1)) || retained_bits > full_bits ||
          (length_known_ && retained_bits > (full_units_ << P::unit_shift)))
        error_detail::raise<std::invalid_argument>("invalid inherited comparison frame");
      std::uint64_t total = retained_bits;
      for (auto part : literal) total = profile_detail::add(total, part.size());
      if (total != full_bits) error_detail::raise<std::invalid_argument>("incomplete comparison literal");
      std::uint64_t compared = 0;
      if (common_bits_ >= retained_bits) {
        auto at = retained_bits;
        order_ = 0;
        for (auto part : literal) {
          auto available = query().size() - at;
          auto other = query().subview(at, available);
          auto result = compare_common_bits(part, other.prefix(std::min(part.size(), available)));
          compared += result.common_bits + (result.common_bits < std::min(part.size(), available));
          at += result.common_bits;
          if (result.order) { order_ = result.order; break; }
        }
        common_bits_ = at;
        if (!order_) order_ = full_bits < query().size() ? -1 : full_bits > query().size() ? 1 : 0;
      }
      full_units_ = full_bits >> P::unit_shift;
      length_known_ = true;
      return compared;
    }

    profile_query_context with_key(bit_view key) const {
      if (key.size() & (P::bits_per_unit - 1)) error_detail::raise<std::invalid_argument>("boundary key unit mismatch");
      auto result = *this;
      auto comparison = compare_common_bits(key, query());
      result.common_bits_ = comparison.common_bits;
      result.full_units_ = (key.size() >> P::unit_shift);
      result.length_known_ = true;
      result.order_ = comparison.order;
      return result;
    }

  private:
    template <class, stream_role> friend struct profile_view;
    template <class> friend struct profile_blob;
    template <class> friend struct profile_blob_view;
    template <class, class> friend struct cola_index_view;
    std::shared_ptr<bit_string const> query_;
    std::uint64_t common_bits_ = 0;
    std::uint64_t full_units_ = 0;
    int order_ = 0;
    bool length_known_ = true;

    std::uint64_t advance(profile_encoded_record const & record) {
      if (length_known_ && record.retained > full_units_)
        error_detail::raise<std::invalid_argument>("comparison anchor is too short");
      auto retained_bits = profile_detail::multiply(record.retained, P::bits_per_unit);
      std::uint64_t compared = 0;
      if (common_bits_ >= retained_bits) {
        auto suffix_query = query().subview(retained_bits, query().size() - retained_bits);
        auto comparison = compare_common_bits(record.suffix, suffix_query);
        common_bits_ = profile_detail::add(retained_bits, comparison.common_bits);
        order_ = comparison.order;
        compared = comparison.common_bits +
          (comparison.common_bits < std::min(record.suffix.size(), suffix_query.size()));
      }
      full_units_ = record.key_units;
      length_known_ = true;
      return compared;
    }

    profile_query_context predecessor(std::uint64_t lcp_bits) const {
      if (order_ > 0 || (length_known_ &&
          lcp_bits > profile_detail::multiply(full_units_, P::bits_per_unit)))
        error_detail::raise<std::invalid_argument>("invalid cut predecessor comparison");
      auto result = *this;
      result.common_bits_ = std::min(lcp_bits, common_bits_);
      // C <= boundary <= query. Matching all of query therefore means C=query;
      // a shorter match means C<query, without consulting C's full length.
      result.length_known_ = result.common_bits_ == query().size();
      result.full_units_ = result.length_known_ ? query().size() >> P::unit_shift : 0;
      result.order_ = result.length_known_ ? 0 : -1;
      return result;
    }
  };

  struct profile_comparison_work {
    std::uint64_t skipped_headers = 0;
    std::uint64_t visited_headers = 0;
    std::uint64_t compared_bits = 0;
  };

  template <class P> struct profile_comparison_item {
    std::uint64_t ordinal = 0;
    profile_query_context<P> const & comparison;
    bit_view value;
  };

  namespace profile_detail {
    inline void resize(bit_string & value, std::uint64_t bits) {
      if (bits > std::numeric_limits<std::uint64_t>::max() - 7)
        error_detail::raise<std::length_error>("profile bit string too large");
      auto bytes = byte_count(bits);
      if (bytes > value.bytes.max_size()) error_detail::raise<std::length_error>("profile bit string too large");
      value.bytes.resize(static_cast<std::size_t>(bytes));
      value.bit_size = bits;
      if (bits & 7) value.bytes.back() &= static_cast<std::byte>(0xffu << (8 - (bits & 7)));
    }
    inline void copy_into(bit_string & target, std::uint64_t first, bit_view source) {
      if (first > target.bit_size || source.size() > target.bit_size - first)
        error_detail::raise<std::out_of_range>("profile copy exceeds destination");
      copy_bits(target.bytes.data(), first, source);
    }

    inline void append(bit_string & target, bit_view source) {
      auto at = target.bit_size;
      auto begin = reinterpret_cast<std::uintptr_t>(target.bytes.data());
      auto input = reinterpret_cast<std::uintptr_t>(source.storage().data());
      if (source.size() && input >= begin && input - begin < target.bytes.size()) {
        auto saved = bit_string::copy(source);
        resize(target, add(at, saved.bit_size));
        copy_into(target, at, saved.view());
      } else {
        resize(target, add(at, source.size()));
        copy_into(target, at, source);
      }
    }
    inline void append_bit(bit_string & target, bool bit) {
      auto at = target.bit_size;
      resize(target, add(at, 1));
      if (bit) target.bytes[at >> 3] |= static_cast<std::byte>(1u << (7 - (at & 7)));
    }
    inline void append_byte(bit_string & target, unsigned value) {
      if ((target.bit_size & 7)) error_detail::raise<std::logic_error>("unaligned byte profile writer");
      target.bytes.push_back(static_cast<std::byte>(value));
      target.bit_size = add(target.bit_size, 8);
    }
    // Fixed-width fields are MSB-first. Width 64 is valid; no shift uses 64.
    inline void put_fixed(bit_string & target, std::uint64_t & at, std::uint64_t value, unsigned width) {
      if (width > 64 || at > target.bit_size || width > target.bit_size - at)
        error_detail::raise<std::out_of_range>("profile fixed field exceeds destination");
      store_bits(target.bytes.data(), at, value, width);
      at += width;
    }
    inline std::uint64_t read_fixed(bit_view data, std::uint64_t & at, unsigned width) {
      if (width > 64 || at > data.size() || width > data.size() - at)
        error_detail::raise<std::invalid_argument>("truncated backspace remainder");
      auto value = load_bits(data, at, width);
      at += width;
      return value;
    }
    inline std::uint64_t read_zero_run(bit_view data, std::uint64_t & at, std::uint64_t limit,
        char const * truncated, char const * overflow) {
      std::uint64_t zeros = 0;
      for (;;) {
        if (at >= data.size()) error_detail::raise<std::invalid_argument>(truncated);
        auto width = unsigned(std::min<std::uint64_t>(data.size() - at, 64));
        auto word = load_bits(data, at, width);
        auto leading = unsigned(std::countl_zero(word)) - (64 - width);
        if (leading > limit - zeros) {
          at += limit - zeros + 1;
          error_detail::raise<std::invalid_argument>(overflow);
        }
        zeros += leading; at += leading;
        if (leading != width) { ++at; return zeros; }
      }
    }
    template <class P> void write_count(bit_string & target, std::uint64_t value) {
      if constexpr (P::unit == profile_unit::byte) {
        while (value >= 128) { append_byte(target, unsigned(value & 127) | 128); value >>= 7; }
        append_byte(target, unsigned(value));
      } else {
        auto maximum = value == std::numeric_limits<std::uint64_t>::max();
        auto width = maximum ? 65u : unsigned(std::bit_width(value + 1));
        auto at = target.bit_size;
        resize(target, add(at, 2 * width - 1));
        if (maximum) { at += 64; put_fixed(target, at, 1, 1); put_fixed(target, at, 0, 64); }
        else { at += width - 1; put_fixed(target, at, value + 1, width); }
      }
    }

    template <class P> std::uint64_t read_count(bit_view data, std::uint64_t & offset) {
      if constexpr (P::unit == profile_unit::byte) {
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift <= 63; shift += 7) {
          if (offset >= (data.size() >> 3)) error_detail::raise<std::invalid_argument>("truncated profile count");
          auto part = std::to_integer<unsigned>(data.storage()[offset++]);
          if (shift == 63 && part > 1) error_detail::raise<std::invalid_argument>("overflowing profile count");
          value |= std::uint64_t(part & 127) << shift;
          if (!(part & 128)) {
            if (shift && !(part & 127)) error_detail::raise<std::invalid_argument>("noncanonical profile count");
            return value;
          }
        }
        error_detail::raise<std::invalid_argument>("overflowing profile count");
      } else {
        // A complete small code fits in the first fifteen bits. Decode its
        // prefix and suffix from one bounded field; long codes and short tails
        // retain the general decoder's exact offset and error behavior.
        if (offset <= data.size() && data.size() - offset >= 16) {
          auto word = load_bits(data, offset, 16);
          auto zeros = unsigned(std::countl_zero(word) - 48);
          if (zeros < 8) {
            offset += 2 * zeros + 1;
            return (word >> (15 - 2 * zeros)) - 1;
          }
        }
        auto zeros = unsigned(read_zero_run(data, offset, 64,
          "truncated exponential-Golomb count", "overflowing exponential-Golomb count"));
        if (zeros > data.size() - offset) {
          offset = data.size();
          error_detail::raise<std::invalid_argument>("truncated exponential-Golomb count");
        }
        auto suffix = read_fixed(data, offset, zeros);
        if (zeros == 64) {
          if (suffix) error_detail::raise<std::invalid_argument>("overflowing exponential-Golomb count");
          return std::numeric_limits<std::uint64_t>::max();
        }
        return ((std::uint64_t{1} << zeros) - 1) + suffix;
      }
    }
    template <class P> constexpr std::uint64_t golomb_cutoff() {
      constexpr auto width = std::bit_width(P::backspace_parameter - 1);
      if constexpr (width == 64) return std::uint64_t{0} - P::backspace_parameter;
      else return (std::uint64_t{1} << width) - P::backspace_parameter;
    }
    // Golomb length includes its unary quotient: a long predecessor can make
    // a short key's backspace expensive even at an LPFC literal restart. Charge
    // the actual codeword length as well as encoded distance from the anchor.
    template <class P> std::uint64_t backspace_bits(std::uint64_t value) {
      static_assert(P::unit == profile_unit::bit);
      if constexpr (P::backspace_code == bit_backspace_code::exponential_golomb) {
        auto quotient = value >> P::backspace_parameter;
        auto prefix = quotient == std::numeric_limits<std::uint64_t>::max()
          ? 129 : 2 * std::bit_width(quotient + 1) - 1;
        return prefix + P::backspace_parameter;
      } else {
        constexpr auto modulus = P::backspace_parameter;
        constexpr auto width = std::bit_width(modulus - 1);
        auto remainder_width = width - (value % modulus < golomb_cutoff<P>());
        return add(add(value / modulus, 1), remainder_width);
      }
    }
    template <class P> void write_backspace(bit_string & target, std::uint64_t value) {
      if constexpr (P::unit == profile_unit::byte) write_count<P>(target, value);
      else {
        // Check the complete length and allocate before emitting a unary run.
        // Fresh bytes and the existing canonical tail supply its zero bits.
        auto at = target.bit_size;
        resize(target, add(at, backspace_bits<P>(value)));
        if constexpr (P::backspace_code == bit_backspace_code::exponential_golomb) {
          auto quotient = value >> P::backspace_parameter;
          if (quotient == std::numeric_limits<std::uint64_t>::max()) {
            at += 64;
            put_fixed(target, at, 1, 1);
            put_fixed(target, at, 0, 64);
          } else {
            auto code = quotient + 1;
            auto width = unsigned(std::bit_width(code));
            at += width - 1;
            put_fixed(target, at, code, width);
          }
          put_fixed(target, at, value, unsigned(P::backspace_parameter));
        } else {
          constexpr auto modulus = P::backspace_parameter;
          constexpr auto width = unsigned(std::bit_width(modulus - 1));
          constexpr auto cutoff = golomb_cutoff<P>();
          at += value / modulus;
          put_fixed(target, at, 1, 1);
          auto remainder = value % modulus;
          if (remainder < cutoff) put_fixed(target, at, remainder, width - 1);
          else put_fixed(target, at, remainder + cutoff, width);
        }
      }
    }
    template <class P> std::uint64_t read_backspace(bit_view data, std::uint64_t & offset) {
      if constexpr (P::unit == profile_unit::byte) return read_count<P>(data, offset);
      else if constexpr (P::backspace_code == bit_backspace_code::exponential_golomb) {
        auto quotient = read_count<P>(data, offset);
        if (quotient > (std::numeric_limits<std::uint64_t>::max() >> P::backspace_parameter))
          error_detail::raise<std::invalid_argument>("overflowing exponential-Golomb backspace");
        auto remainder = read_fixed(data, offset, unsigned(P::backspace_parameter));
        return (quotient << P::backspace_parameter) | remainder;
      } else {
        constexpr auto modulus = P::backspace_parameter;
        constexpr auto width = unsigned(std::bit_width(modulus - 1));
        constexpr auto cutoff = golomb_cutoff<P>();
        constexpr auto limit = std::numeric_limits<std::uint64_t>::max() / modulus;
        auto quotient = read_zero_run(data, offset, limit,
          "truncated backspace remainder", "overflowing Golomb quotient");
        std::uint64_t remainder = 0;
        if constexpr (width != 0) {
          remainder = read_fixed(data, offset, width - 1);
          if (remainder >= cutoff) remainder = ((remainder << 1) | read_fixed(data, offset, 1)) - cutoff;
        }
        auto base = quotient * modulus;
        if (remainder > std::numeric_limits<std::uint64_t>::max() - base)
          error_detail::raise<std::invalid_argument>("overflowing Golomb backspace");
        return base + remainder;
      }
    }
    template <class P, stream_role Role> profile_metadata initial_metadata() {
      profile_metadata result;
      result.key_unit = result.value_unit = result.count_unit = result.offset_unit = P::unit;
      result.count_code = P::unit == profile_unit::byte ? profile_count_code::varint : profile_count_code::exp_golomb_zero;
      result.backspace_code = P::backspace_code;
      result.backspace_parameter = P::backspace_parameter;
      result.role = Role;
      result.group_size = P::group_size;
      result.codec_block_size = P::codec_block_size;
      result.policy_fixed_values = P::fixed_width;
      result.policy_value_width = P::value_width.value_or(0);
      result.common_value_width = Role == stream_role::borrowed ? 0 : P::value_width.value_or(0);
      return result;
    }
  }

  template <class P, stream_role Role = stream_role::native> struct profile_cursor;
  template <class P, stream_role Role = stream_role::native> struct profile_encoded_cursor;
  template <class P> struct profile_borrowed_writer;
  template <class P, class Native, class Output> struct index_builder;
  namespace profile_detail { template <class P> struct index_output; }
  template <class P> struct profile_native_writer;
  namespace profile_detail { template <class P> struct native_output; }

  // A view borrows both sections. Metadata and parsed counts are checked, but
  // complete semantic validation also requires traversing the stream. Physical
  // offsets and all encoded lengths count P units; bit_view lengths count bits.
  template <class P, stream_role Role = stream_role::native> struct profile_view {
    using policy_type = P;
    static constexpr stream_role role = Role;

    profile_view(std::span<std::byte const> bytes, elias_fano_view offsets, profile_metadata metadata)
      : profile_view(bytes, offsets, metadata, shape_only{}) {
      validate_contents();
    }

    // Metadata/shape checks only: no stream, EF-word or sample bytes are read.
    // Mapped owners may construct this view before touching any payload page.
    // External metadata must also pass validate_offset_metadata() before
    // navigation; trusted array builders establish that invariant themselves.
    static profile_view from_sections(std::span<std::byte const> bytes,
        elias_fano_view offsets, profile_metadata metadata) {
      return {bytes, offsets, metadata, shape_only{}};
    }

    // The existing constructor's local content checks. This validates padding
    // and the first/terminal offsets; it is not a complete framing, ordering or
    // EF semantic scan. Query parsing still bounds each accessed record.
    void validate_contents() const {
      validate_offset_metadata();
      auto bits = data_.size();
      if ((bits & 7) && (std::to_integer<unsigned>(bytes_.back()) & ((1u << (8 - (bits & 7))) - 1)))
        error_detail::raise<std::invalid_argument>("nonzero profile padding");
      if (block_offset(0) != 0 || block_offset(block_count()) != metadata_.extent)
        error_detail::raise<std::invalid_argument>("profile offset units or extent mismatch");
    }

    // External metadata admission checks fixed-stride representability once;
    // it reads neither the FC payload nor any Elias–Fano word or sample.
    // Trusted owning arrays establish this invariant while they are built.
    void validate_offset_metadata() const {
      auto stride = profile_detail::multiply(metadata_.record_count, metadata_.common_value_width.value_or(0));
      if (stride > metadata_.extent || offsets_.universe() != metadata_.extent - stride)
        error_detail::raise<std::invalid_argument>("profile offset universe or fixed stride mismatch");
    }

    std::uint64_t block_count() const noexcept {
      return metadata_.record_count / P::codec_block_size +
        (metadata_.record_count % P::codec_block_size != 0);
    }
    // The final sample is EOF at the actual record count. Fixed-stride
    // arithmetic is unchecked here after metadata admission; the generic EF
    // codec only selects the stored residual value.
    std::uint64_t block_offset(std::uint64_t block) const {
      auto blocks = block_count();
      if (block > blocks) error_detail::raise<std::out_of_range>("profile block ordinal");
      auto ordinal = block == blocks ? metadata_.record_count : block * P::codec_block_size;
      return offsets_.select(block) + ordinal * metadata_.common_value_width.value_or(0);
    }

    std::uint64_t size() const noexcept { return metadata_.record_count; }
    std::span<std::byte const> bytes() const noexcept { return bytes_; }
    profile_metadata const & metadata() const noexcept { return metadata_; }
    elias_fano_view group_offsets() const noexcept { return offsets_; }
    profile_cursor<P, Role> cursor() const;
    profile_encoded_cursor<P, Role> encoded_cursor() const;

    // Physical block starts encode retained positions directly. Other records
    // backspace from the preceding length, so only this block's controls need
    // replaying; no inherited key bytes are reconstructed or read.
    profile_encoded_record encoded_at(std::uint64_t ordinal) const {
      if (ordinal >= size()) error_detail::raise<std::out_of_range>("profile record ordinal");
      auto [at, retained] = locate(ordinal, nullptr);
      return parse_payload(at, retained);
    }

    // Compatibility length lookup: replay the predecessor's physical block.
    // A boundary can touch the preceding block, but never its literal bytes.
    // At EOF terminal metadata supplies the length without replay.
    std::uint64_t predecessor_units(std::uint64_t ordinal,
                                    profile_comparison_work * work = nullptr) const {
      if (ordinal > size()) error_detail::raise<std::out_of_range>("profile predecessor ordinal");
      if (!ordinal) return 0;
      if (ordinal == size()) return metadata_.terminal_key_units;
      auto [at, retained] = locate(ordinal - 1, work);
      auto record = parse_payload(at, retained);
      if (work) ++work->skipped_headers;
      return record.key_units;
    }

    // Only headers preceding the selected lane are parsed. The first candidate
    // must share its retained prefix with the supplied query-bound context.
    // Callback comparison references expire on the next call or return.
    template <class F> void compare_window(std::uint64_t first, std::uint64_t last,
      profile_query_context<P> context, F && callback, profile_comparison_work * work = nullptr,
      std::uint64_t * predecessor = nullptr) const {
      if (first > last || last > size() || last - first > P::group_size)
        error_detail::raise<std::out_of_range>("profile comparison window exceeds cascade group");
      if (first == last) {
        if (predecessor) *predecessor = predecessor_units(first, work);
        return;
      }
      auto [at, retained] = locate(first, work, predecessor);
      auto record = parse_payload(at, retained);
      for (auto i = first; i != last; ++i) {
        auto compared = context.advance(record);
        if (work) { ++work->visited_headers; work->compared_bits += compared; }
        if (!callback(profile_comparison_item<P>{i, context, record.value})) return;
        if (i + 1 != last) record = next_record(record, i + 1);
      }
    }

    // The supplied anchor must share the first record's retained prefix. An
    // anchor between its actual predecessor and that key suffices. The copied
    // prefix is not an exact LCP with that anchor. Callback key views expire
    // on the next call.
    template <class F> void visit_window(std::uint64_t first, std::uint64_t last,
      profile_anchor<P> anchor, std::uint64_t prefix_limit, F && callback) const {
      if (first > last || last > size() || last - first > P::group_size)
        error_detail::raise<std::out_of_range>("profile window exceeds the policy group size");
      if (first == last) return;
      if ((anchor.prefix.size() & (P::bits_per_unit - 1)) ||
          (anchor.prefix.size() >> P::unit_shift) > anchor.full_units)
        error_detail::raise<std::invalid_argument>("profile anchor units");
      auto prefix_bits = profile_detail::multiply(
        std::min(prefix_limit, (anchor.prefix.size() >> P::unit_shift)), P::bits_per_unit);
      auto scratch = bit_string::copy(anchor.prefix.prefix(prefix_bits));
      auto context = anchor.full_units;
      auto record = encoded_at(first);
      for (auto i = first; i != last; ++i) {
        decode_into(record, prefix_limit, scratch, context);
        if (!callback(profile_item<P>{i, {scratch.view(), context}, record.value})) return;
        if (i + 1 != last) record = next_record(record, i + 1);
      }
    }

    template <class F> void visit_all(F && callback) const {
      if (!size()) return;
      bit_string scratch;
      std::uint64_t context = 0;
      auto record = encoded_at(0);
      for (std::uint64_t i = 0; i != size(); ++i) {
        decode_into(record, std::numeric_limits<std::uint64_t>::max(), scratch, context);
        if (!callback(profile_item<P>{i, {scratch.view(), context}, record.value})) return;
        if (i + 1 != size()) record = next_record(record, i + 1);
      }
      if (record.next_offset != metadata_.extent) error_detail::raise<std::invalid_argument>("trailing profile data");
      if (record.key_units != metadata_.terminal_key_units)
        error_detail::raise<std::invalid_argument>("profile terminal length mismatch");
    }

    // Ordinary FC can traverse the entire prefix chain. LPFC bounds backward
    // encoded distance by the full key length, not by prefix_limit; locating a
    // record also scans at most W-1 headers before it. Values are copied in full.
    profile_decoded_record<P> reconstruct_at(std::uint64_t ordinal,
        std::uint64_t prefix_limit = std::numeric_limits<std::uint64_t>::max()) const {
      auto record = encoded_at(ordinal);
      profile_decoded_record<P> result;
      result.full_units = record.key_units;
      result.value = bit_string::copy(record.value);
      auto need = std::min(record.key_units, prefix_limit);
      profile_detail::resize(result.prefix, profile_detail::multiply(need, P::bits_per_unit));
      while (need) {
        if (record.key_units < need) error_detail::raise<std::invalid_argument>("profile predecessor prefix is too short");
        if (record.retained < need) {
          auto copy = profile_detail::multiply(need - record.retained, P::bits_per_unit);
          profile_detail::copy_into(result.prefix, profile_detail::multiply(record.retained, P::bits_per_unit),
                                    record.suffix.prefix(copy));
          need = record.retained;
        }
        if (!need) break;
        if (!ordinal) error_detail::raise<std::invalid_argument>("nonliteral first profile key");
        auto retained = record.retained;
        record = encoded_at(--ordinal);
        if (record.key_units < retained) error_detail::raise<std::invalid_argument>("profile predecessor prefix is too short");
      }
      return result;
    }

  private:
    struct shape_only {};
    profile_view(std::span<std::byte const> bytes, elias_fano_view offsets,
                 profile_metadata metadata, shape_only)
      : bytes_(bytes), offsets_(offsets), metadata_(metadata) {
      auto expected = profile_detail::initial_metadata<P, Role>();
      if (metadata.version != 2 || metadata.key_unit != P::unit || metadata.value_unit != P::unit ||
          metadata.count_unit != P::unit || metadata.offset_unit != P::unit ||
          metadata.backspace_code != P::backspace_code || metadata.backspace_parameter != P::backspace_parameter ||
          metadata.count_code != expected.count_code || metadata.bit_order != profile_bit_order::msb_first ||
          metadata.role != Role || metadata.group_size != P::group_size ||
          metadata.codec_block_size != P::codec_block_size)
        error_detail::raise<std::invalid_argument>("profile metadata does not match reader policy");
      if (!metadata.policy_fixed_values && metadata.policy_value_width)
        error_detail::raise<std::invalid_argument>("variable profile policy width must be zero");
      if constexpr (Role == stream_role::borrowed) {
        if (metadata.common_value_width != std::optional<std::uint64_t>(0))
          error_detail::raise<std::invalid_argument>("borrowed profile must have empty values");
      } else if (metadata.policy_fixed_values) {
        if (metadata.common_value_width != metadata.policy_value_width)
          error_detail::raise<std::invalid_argument>("fixed value width metadata mismatch");
      }
      if (!metadata.record_count && (metadata.extent || metadata.terminal_key_units))
        error_detail::raise<std::invalid_argument>("nonempty data for empty profile");
      auto bits = profile_detail::multiply(metadata.extent, P::bits_per_unit);
      auto blocks = block_count();
      if (bits > std::numeric_limits<std::uint64_t>::max() - 7 ||
          bytes.size() != profile_detail::byte_count(bits) ||
          blocks == std::numeric_limits<std::uint64_t>::max() || offsets.size() != blocks + 1)
        error_detail::raise<std::invalid_argument>("profile section length mismatch");
      data_ = {bytes, bits};
    }

    friend struct profile_cursor<P, Role>;
    friend struct profile_encoded_cursor<P, Role>;
    std::span<std::byte const> bytes_;
    elias_fano_view offsets_;
    profile_metadata metadata_;
    bit_view data_;

    std::pair<std::uint64_t, std::uint64_t> locate(std::uint64_t ordinal, profile_comparison_work * work,
                                                std::uint64_t * predecessor = nullptr) const {
      auto group = ordinal / P::codec_block_size;
      auto at = block_offset(group);
      auto retained = read_absolute(at);
      if (!group && retained) error_detail::raise<std::invalid_argument>("first profile key is not literal");
      std::uint64_t previous = 0;
      for (auto i = group * P::codec_block_size; i < ordinal; ++i) {
        auto [key_units, next_offset] = parse_payload<false>(at, retained);
        previous = key_units;
        at = next_offset;
        auto backspace = profile_detail::read_backspace<P>(data_, at);
        if (backspace > previous) error_detail::raise<std::invalid_argument>("profile backspace exceeds predecessor");
        retained = previous - backspace;
        if (work) ++work->skipped_headers;
      }
      if (predecessor) {
        if (ordinal && ordinal % P::codec_block_size == 0)
          previous = predecessor_units(ordinal, work);
        if (retained > previous)
          error_detail::raise<std::invalid_argument>("profile retained prefix exceeds predecessor");
        *predecessor = previous;
      }
      return {at, retained};
    }

    std::uint64_t read_absolute(std::uint64_t & at) const {
      auto start = at;
      auto retained = profile_detail::read_count<P>(data_, at);
      // Every retained unit appeared in an earlier literal. Use the restored
      // physical offset, including fixed-width values, rather than EF residuals.
      if (retained > start)
        error_detail::raise<std::invalid_argument>("profile retained prefix exceeds physical offset");
      return retained;
    }

    profile_encoded_record parse_absolute(std::uint64_t at) const {
      auto retained = read_absolute(at);
      return parse_payload(at, retained);
    }

    profile_encoded_record parse_relative(std::uint64_t at, std::uint64_t previous) const {
      auto backspace = profile_detail::read_backspace<P>(data_, at);
      if (backspace > previous) error_detail::raise<std::invalid_argument>("profile backspace exceeds predecessor");
      return parse_payload(at, previous - backspace);
    }

    template <bool Views = true>
    auto parse_payload(std::uint64_t at, std::uint64_t retained) const {
      auto suffix = profile_detail::read_count<P>(data_, at);
      auto value = metadata_.common_value_width ? *metadata_.common_value_width : profile_detail::read_count<P>(data_, at);
      if (at > metadata_.extent || suffix > metadata_.extent - at || value > metadata_.extent - at - suffix)
        error_detail::raise<std::invalid_argument>("truncated profile payload");
      // Absolute controls establish retained <= frame start. Relative controls
      // inherit key_units <= the previous frame's end. Thus retained <= at and
      // the bounded suffix gives key_units <= extent, whose bit size was admitted.
      auto key_units = retained + suffix;
      if constexpr (Views) {
        auto suffix_bits = profile_detail::multiply(suffix, P::bits_per_unit);
        auto value_bits = profile_detail::multiply(value, P::bits_per_unit);
        auto key_data = data_.subview(profile_detail::multiply(at, P::bits_per_unit), suffix_bits);
        at += suffix;
        auto value_data = data_.subview(profile_detail::multiply(at, P::bits_per_unit), value_bits);
        return profile_encoded_record{retained, key_units, value, key_data, value_data, at + value};
      } else {
        // Skipped lanes need only framing. Do not construct literal/value views
        // or return an encoded record when neither payload will be inspected.
        return std::pair<std::uint64_t, std::uint64_t>{key_units, at + suffix + value};
      }
    }

    profile_encoded_record next_record(profile_encoded_record const & previous, std::uint64_t ordinal) const {
      auto at = previous.next_offset;
      if (ordinal % P::codec_block_size == 0) {
        if (block_offset(ordinal / P::codec_block_size) != at)
          error_detail::raise<std::invalid_argument>("profile group offset mismatch");
        auto next = parse_absolute(at);
        if (next.retained > previous.key_units)
          error_detail::raise<std::invalid_argument>("profile retained prefix exceeds predecessor");
        return next;
      }
      return parse_relative(at, previous.key_units);
    }

    static void decode_into(profile_encoded_record const & record, std::uint64_t limit,
                            bit_string & scratch, std::uint64_t & context) {
      if (record.retained > context) error_detail::raise<std::invalid_argument>("profile anchor is too short");
      auto keep = std::min(record.retained, limit);
      auto keep_bits = profile_detail::multiply(keep, P::bits_per_unit);
      if (keep_bits > scratch.bit_size) error_detail::raise<std::invalid_argument>("profile anchor prefix is incomplete");
      profile_detail::resize(scratch, keep_bits);
      auto full = std::min(record.key_units, limit);
      profile_detail::append(scratch, record.suffix.prefix(profile_detail::multiply(full - keep, P::bits_per_unit)));
      context = record.key_units;
    }
  };

  // Sequential framing without reconstructed keys or payload copies. The
  // caller retains the view's sections. A copied frame's suffix/value views
  // remain valid while those sections live; the peek reference expires on
  // advance or destruction. Parsing checks lengths and block offsets, but
  // sorted order and maximal retention require semantic admission or a caller
  // that compares against the physical predecessor.
  template <class P, stream_role Role> struct profile_encoded_cursor {
    using policy_type = P;
    static constexpr stream_role role = Role;

    explicit profile_encoded_cursor(profile_view<P, Role> view) : view_(view) {
      if (!done()) record_ = view_.encoded_at(0);
    }

    bool done() const noexcept { return ordinal_ == view_.size(); }
    std::uint64_t ordinal() const noexcept { return ordinal_; }
    profile_encoded_record const & peek() const & {
      if (done()) error_detail::raise<std::out_of_range>("encoded profile cursor at end");
      return record_;
    }
    profile_encoded_record const & peek() const && = delete;

    void advance() {
      if (done()) error_detail::raise<std::out_of_range>("encoded profile cursor at end");
      if (ordinal_ + 1 == view_.size()) {
        if (record_.next_offset != view_.metadata_.extent)
          error_detail::raise<std::invalid_argument>("trailing profile data");
        if (record_.key_units != view_.metadata_.terminal_key_units)
          error_detail::raise<std::invalid_argument>("profile terminal length mismatch");
      } else record_ = view_.next_record(record_, ordinal_ + 1);
      ++ordinal_;
    }

  private:
    profile_view<P, Role> view_;
    profile_encoded_record record_;
    std::uint64_t ordinal_ = 0;
  };

  template <class P, stream_role Role>
  profile_encoded_cursor<P, Role> profile_view<P, Role>::encoded_cursor() const {
    return profile_encoded_cursor<P, Role>(*this);
  }

  // Resumable sequential traversal. Each encoded record is parsed once and
  // its value remains a view of the original payload. Only the current key is
  // reconstructed. The view's sections must outlive this cursor; peek's key
  // view additionally expires when this cursor advances or is destroyed.
  template <class P, stream_role Role> struct profile_cursor {
    using policy_type = P;
    static constexpr stream_role role = Role;

    explicit profile_cursor(profile_view<P, Role> view) : view_(view) {
      if (!done()) {
        record_ = view_.encoded_at(0);
        profile_view<P, Role>::decode_into(record_, std::numeric_limits<std::uint64_t>::max(), scratch_, context_);
      }
    }

    bool done() const noexcept { return ordinal_ == view_.size(); }
    std::uint64_t ordinal() const noexcept { return ordinal_; }
    profile_item<P> peek() const & {
      if (done()) error_detail::raise<std::out_of_range>("profile cursor at end");
      return {ordinal_, {scratch_.view(), context_}, record_.value};
    }
    profile_item<P> peek() const && = delete;

    void advance() { advance_impl<false>(nullptr); }

    // Compare the old and new keys from the retained FC prefix onward before
    // replacing scratch. Ordinary FC needs only its first differing unit;
    // redundant retained-prefix encodings remain correct. No result at EOF.
    std::optional<bit_comparison> advance_comparison() {
      bit_comparison result;
      advance_impl<true>(&result);
      return done() ? std::nullopt : std::optional<bit_comparison>{result};
    }

  private:
    template <bool Compare> void advance_impl(bit_comparison * comparison) {
      if (done()) error_detail::raise<std::out_of_range>("profile cursor at end");
      if (ordinal_ + 1 == view_.size()) {
        if (record_.next_offset != view_.metadata_.extent)
          error_detail::raise<std::invalid_argument>("trailing profile data");
        if (record_.key_units != view_.metadata_.terminal_key_units)
          error_detail::raise<std::invalid_argument>("profile terminal length mismatch");
        ++ordinal_;
        return;
      }
      auto next = view_.next_record(record_, ordinal_ + 1);
      if constexpr (Compare) {
        auto retained_bits = profile_detail::multiply(next.retained, P::bits_per_unit);
        auto previous = scratch_.view();
        if (retained_bits == previous.size() || next.suffix.empty()) {
          *comparison = {retained_bits, retained_bits != previous.size() ? 1 : next.suffix.empty() ? 0 : -1};
        } else {
          auto before = profile_detail::load_bits(previous, retained_bits, P::bits_per_unit);
          auto after = profile_detail::load_bits(next.suffix, 0, P::bits_per_unit);
          if (before != after) {
            auto common = unsigned(std::countl_zero(before ^ after)) - (64 - P::bits_per_unit);
            *comparison = {retained_bits + common, before < after ? -1 : 1};
          } else {
            auto suffix = compare_common_bits(
              previous.subview(retained_bits, previous.size() - retained_bits), next.suffix);
            *comparison = {retained_bits + suffix.common_bits, suffix.order};
          }
        }
      }
      profile_view<P, Role>::decode_into(next, std::numeric_limits<std::uint64_t>::max(), scratch_, context_);
      record_ = next;
      ++ordinal_;
    }

  private:
    profile_view<P, Role> view_;
    bit_string scratch_;
    std::uint64_t context_ = 0;
    profile_encoded_record record_;
    std::uint64_t ordinal_ = 0;
  };

  template <class P, stream_role Role>
  profile_cursor<P, Role> profile_view<P, Role>::cursor() const { return profile_cursor<P, Role>(*this); }

  template <class P, stream_role Role = stream_role::native> struct profile_array {
    using policy_type = P;
    static constexpr stream_role role = Role;

    static profile_array build(std::span<profile_record const> records,
      std::span<std::uint64_t const> prefix_ceilings = {}, std::uint64_t restart_factor = 0) {
      if (!prefix_ceilings.empty() && prefix_ceilings.size() != records.size())
        error_detail::raise<std::invalid_argument>("one profile prefix ceiling is required per record");
      if (restart_factor && restart_factor < 3) error_detail::raise<std::invalid_argument>("LPFC factor must be at least three");
      profile_array result;
      std::optional<std::uint64_t> common;
      if constexpr (Role == stream_role::borrowed) common = 0;
      else if constexpr (P::fixed_width) common = P::value_width;
      else if (records.empty()) common = 0;
      else common = (records.front().value.view().size() >> P::unit_shift);
      for (auto const & record : records) {
        auto key_bits = record.key.view().size();
        auto value_bits = record.value.view().size();
        if ((key_bits & (P::bits_per_unit - 1)) || (value_bits & (P::bits_per_unit - 1)))
          error_detail::raise<std::invalid_argument>("record length does not match profile unit");
        auto width = (value_bits >> P::unit_shift);
        if constexpr (Role == stream_role::borrowed) {
          if (width) error_detail::raise<std::invalid_argument>("borrowed profile cannot carry values");
        } else if constexpr (P::fixed_width) {
          if (width != *P::value_width) error_detail::raise<std::invalid_argument>("value does not match fixed policy width");
        } else {
          if (common && *common != width) common.reset();
        }
      }
      result.metadata_.common_value_width = common;
      result.metadata_.record_count = records.size();
      bit_string data;
      bit_view previous;
      std::uint64_t previous_units = 0;
      std::uint64_t anchor_offset = 0;
      std::vector<std::uint64_t> offsets;
      for (std::size_t i = 0; i != records.size(); ++i) {
        auto key = records[i].key.view();
        auto value = records[i].value.view();
        auto comparison = compare_common_bits(previous, key);
        if (i && comparison.order > 0) error_detail::raise<std::invalid_argument>("profile keys must be sorted");
        auto position = (data.bit_size >> P::unit_shift);
        if (i % P::codec_block_size == 0)
          offsets.push_back(position - profile_detail::multiply(i, common.value_or(0)));
        auto key_units = (key.size() >> P::unit_shift);
        auto retained = (comparison.common_bits >> P::unit_shift);
        if (!prefix_ceilings.empty()) retained = std::min(retained, prefix_ceilings[i]);
        auto start = (data.bit_size >> P::unit_shift);
        if (retained && restart_factor && key_units <= std::numeric_limits<std::uint64_t>::max() / restart_factor &&
            start - anchor_offset > restart_factor * key_units) retained = 0;
        if (!retained) anchor_offset = start;
        if (i % P::codec_block_size == 0) profile_detail::write_count<P>(data, retained);
        else profile_detail::write_backspace<P>(data, previous_units - retained);
        profile_detail::write_count<P>(data, key_units - retained);
        if (!common) profile_detail::write_count<P>(data, (value.size() >> P::unit_shift));
        profile_detail::append(data, key.subview(profile_detail::multiply(retained, P::bits_per_unit),
                                               key.size() - profile_detail::multiply(retained, P::bits_per_unit)));
        profile_detail::append(data, value);
        previous = key;
        previous_units = key_units;
      }
      result.metadata_.terminal_key_units = previous_units;
      result.metadata_.extent = (data.bit_size >> P::unit_shift);
      offsets.push_back(result.metadata_.extent - profile_detail::multiply(records.size(), common.value_or(0)));
      result.offsets_ = elias_fano::build(offsets);
      result.bytes_ = std::move(data.bytes);
      return result;
    }

    std::uint64_t size() const noexcept { return metadata_.record_count; }
    std::span<std::byte const> bytes() const noexcept { return bytes_; }
    profile_metadata const & metadata() const noexcept { return metadata_; }
    elias_fano const & group_offsets() const noexcept { return offsets_; }
    // These private sections come from the checked builders. Recheck their
    // shapes after copying/moving, without rereading payload or EF endpoints.
    profile_view<P, Role> view() const & {
      return profile_view<P, Role>::from_sections(bytes_, offsets_.view(), metadata_);
    }
    profile_view<P, Role> view() const && = delete;

  private:
    friend struct profile_borrowed_writer<P>;
    friend struct profile_native_writer<P>;
    friend struct profile_detail::native_output<P>;
    std::vector<std::byte> bytes_;
    // The profile's EOF marker is explicit; the generic codec defaults empty.
    elias_fano offsets_ = elias_fano::build(std::array<std::uint64_t, 1>{0});
    profile_metadata metadata_ = profile_detail::initial_metadata<P, Role>();
  };

  // Incremental modified-FC output. The caller supplies any boundary-dependent
  // prefix ceiling before appending that key. There is no all-keys staging:
  // retained state is the previous key, encoded bytes and one offset per group.
  // With the same ceilings this produces exactly the batch borrowed encoding
  // (restart_factor == 0), including block controls, tail padding and EF metadata.
  template <class P> struct profile_borrowed_writer {
    using policy_type = P;
    static constexpr stream_role role = stream_role::borrowed;

    std::uint64_t size() const noexcept { return count_; }
    bool finished() const noexcept { return finished_; }

    void append(bit_view key, std::uint64_t prefix_ceiling = std::numeric_limits<std::uint64_t>::max()) {
      if (finished_) error_detail::raise<std::logic_error>("borrowed profile writer is finished");
      if (key.size() & (P::bits_per_unit - 1)) error_detail::raise<std::invalid_argument>("key length does not match profile unit");
      auto previous = previous_.view();
      auto comparison = compare_common_bits(previous, key);
      if (count_ && comparison.order > 0) error_detail::raise<std::invalid_argument>("profile keys must be sorted");
      append_known(key, comparison.common_bits, prefix_ceiling);
    }

    // Finalizing EF takes work proportional to the staged group offsets.
    // This is not a bounded-byte or durable checkpoint operation.
    profile_array<P, stream_role::borrowed> finish() {
      if (finished_) error_detail::raise<std::logic_error>("borrowed profile writer is finished");
      profile_array<P, stream_role::borrowed> result;
      auto extent = (data_.bit_size >> P::unit_shift);
      offsets_.push_back(extent);
      try {
        result.offsets_ = elias_fano::build(offsets_);
      } catch (...) {
        offsets_.pop_back();
        throw;
      }
      result.metadata_.record_count = count_;
      result.metadata_.extent = extent;
      result.metadata_.terminal_key_units = (previous_.bit_size >> P::unit_shift);
      result.bytes_ = std::move(data_.bytes);
      data_.bit_size = 0;
      previous_ = {};
      offsets_ = std::vector<std::uint64_t>{};
      finished_ = true;
      return result;
    }

  private:
    template <class, class, class> friend struct index_builder;
    template <class, class, class> friend struct cola_index_builder;
    friend struct profile_detail::index_output<P>;
    // The index builders bypass the public comparison, using the exact
    // LCP already obtained when this borrowed key was accepted. Framing and
    // allocation rollback are shared with the public checked writer.
    void append_known(bit_view key, std::uint64_t exact_common_bits,
                      std::uint64_t prefix_ceiling = std::numeric_limits<std::uint64_t>::max()) {
      auto next_count = profile_detail::add(count_, 1);
      auto retained = std::min((exact_common_bits >> P::unit_shift), prefix_ceiling);
      auto previous_units = (previous_.bit_size >> P::unit_shift);
      auto key_units = (key.size() >> P::unit_shift);
      auto saved_bits = data_.bit_size;
      auto saved_offsets = offsets_.size();
      // Reserve before modifying output. The private predecessor retains its
      // logical contents until all potentially allocating output writes finish.
      auto next_bytes = profile_detail::byte_count(key.size());
      if (next_bytes > previous_.bytes.max_size()) error_detail::raise<std::length_error>("profile bit string too large");
      previous_.bytes.reserve(static_cast<std::size_t>(next_bytes));
      try {
        if (count_ % P::codec_block_size == 0) {
          offsets_.push_back((data_.bit_size >> P::unit_shift));
          profile_detail::write_count<P>(data_, retained);
        } else profile_detail::write_backspace<P>(data_, previous_units - retained);
        profile_detail::write_count<P>(data_, key_units - retained);
        auto retained_bits = profile_detail::multiply(retained, P::bits_per_unit);
        profile_detail::append(data_, key.subview(retained_bits, key.size() - retained_bits));
      } catch (...) {
        profile_detail::resize(data_, saved_bits);
        offsets_.resize(saved_offsets);
        throw;
      }
      // This resize cannot allocate after reserve. Keep the actual shared
      // prefix even when the encoding used a smaller boundary prefix ceiling.
      auto common_bits = exact_common_bits & ~std::uint64_t(P::bits_per_unit - 1);
      profile_detail::resize(previous_, key.size());
      profile_detail::copy_into(previous_, common_bits,
        key.subview(common_bits, key.size() - common_bits));
      count_ = next_count;
    }

    bit_string data_;
    bit_string previous_;
    std::vector<std::uint64_t> offsets_;
    std::uint64_t count_ = 0;
    bool finished_ = false;
  };
}
