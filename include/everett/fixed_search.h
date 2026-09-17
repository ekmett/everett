/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Searches bounded windows of portable fixed-width keys with scalar or SIMD pivots.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/error_detail.h>
#include <everett/backend.h>
#include <simd/integer.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>

namespace everett {
  namespace fixed_search_detail {
    inline std::uint32_t load_word(std::byte const *source) noexcept {
      std::uint32_t value;
      std::memcpy(&value, source, sizeof(value));
      if constexpr (std::endian::native == std::endian::big) {
        value = ((value & 0x00ff00ffu) << 8) | ((value >> 8) & 0x00ff00ffu);
        value = (value << 16) | (value >> 16);
      }
      return value;
    }

    template <std::size_t Words, std::size_t Lanes>
    inline std::array<std::uint32_t, Lanes> repeated(std::array<std::uint32_t, Words> const &query) noexcept {
      std::array<std::uint32_t, Lanes> result{};
      for (std::size_t i = 0; i < Lanes; ++i) result[i] = query[i % Words];
      return result;
    }

    template <std::size_t Words, bool Upper>
    inline bool precedes(std::byte const *source, std::array<std::uint32_t, Words> const &query) noexcept {
      bool less = false, equal = true;
      for (std::size_t i = 0; i < Words; ++i) {
        auto value = load_word(source + (i << 2));
        less |= equal & (value < query[i]);
        equal &= value == query[i];
      }
      if constexpr (Upper) return less | equal;
      else return less;
    }

    template <bool Upper, simd::architecture Arch>
    simd_inline bool precedes_four(std::byte const *source, simd::vec<std::uint32_t, 4, Arch> query) noexcept {
      using V = simd::vec<std::uint32_t, 4, Arch>;
      auto value = V::load(reinterpret_cast<std::uint32_t const *>(source));
      // A more significant unequal word outweighs every following word.
      // Summing unsigned lane differences modulo 2^32 gives -15..15.
      auto order = mask_bits(value > query) - mask_bits(value < query);
      auto weighted = order * V(std::array<std::uint32_t, 4>{8, 4, 2, 1});
      auto comparison = std::bit_cast<std::int32_t>(std::uint32_t(reduce_add_widened(weighted)));
      if constexpr (Upper) return comparison >= 0;
      else return comparison > 0;
    }

    // Count keys satisfying < query (or <= query), not individual word lanes.
    // The first word of each key is the most significant comparison word.
    // Callers supply at most four complete keys; no padding is required.
    template <std::size_t Words, bool Upper, simd::architecture Arch>
    simd_inline unsigned population(std::byte const *source, unsigned count,
        std::array<std::uint32_t, Words> const &query) noexcept {
      if (!count) return 0;
      if constexpr (std::is_same_v<Arch, simd::scalar> || std::endian::native != std::endian::little) {
        unsigned result = 0;
        for (unsigned i = 0; i < count; ++i) result += precedes<Words, Upper>(source + i * Words * 4, query);
        return result;
      } else if constexpr (std::is_same_v<Arch, simd::neon>) {
        if constexpr (Words == 1) {
          using V = simd::vec<std::uint32_t, 4, Arch>;
          auto values = V::load_partial(reinterpret_cast<std::uint32_t const *>(source), count);
          auto queries = V(query[0]);
          auto active = V(std::array<std::uint32_t, 4>{0, 1, 2, 3}) < V(count);
          auto matches = Upper ? values <= queries : values < queries;
          return unsigned(reduce_add_widened(select(matches & active, V(1), V(0))));
        } else if constexpr (Words == 2) {
          using W = simd::vec<std::uint32_t, 4, Arch>;
          using V = simd::vec<std::uint64_t, 2, Arch>;
          auto queries = V((std::uint64_t(query[0]) << 32) | query[1]);
          unsigned result = 0;
          for (unsigned at = 0; at < 4; at += 2) {
            if (at >= count) break;
            if (count - at == 1) result += precedes<Words, Upper>(source + (at << 3), query);
            else {
              auto words = W::load(reinterpret_cast<std::uint32_t const *>(source + (at << 3)));
              W swapped = words.yxwz;
              auto values = reinterpret_bits<std::uint64_t>(swapped);
              auto matches = Upper ? values <= queries : values < queries;
              auto counts = mask_bits(matches).template right<63>();
              result += unsigned(reduce_add_widened(reinterpret_bits<std::uint32_t>(counts)));
            }
          }
          return result;
        } else {
          auto queries = simd::vec<std::uint32_t, 4, Arch>::load(query.data());
          unsigned result = 0;
          for (unsigned at = 0; at < 4; ++at) {
            if (at >= count) break;
            result += precedes_four<Upper, Arch>(source + (at << 4), queries);
          }
          return result;
        }
      } else {
        unsigned less = 0, equal = 0;
        unsigned word_count = count * unsigned(Words);
        constexpr unsigned lanes = unsigned(backend_detail::register_bytes<Arch> / sizeof(std::uint32_t));
        using V = simd::vec<std::uint32_t, lanes, Arch>;
        auto queries = V(repeated<Words, lanes>(query));
        for (unsigned at = 0; at < Words * 4; at += lanes) {
          if (at >= word_count) break;
          auto values = V::load_partial(reinterpret_cast<std::uint32_t const *>(source + (at << 2)),
            std::min(lanes, word_count - at));
          less |= unsigned((values < queries).to_bitset()) << at;
          equal |= unsigned((values == queries).to_bitset()) << at;
        }
        constexpr unsigned starts = Words == 1 ? 0xffffu : Words == 2 ? 0x5555u : 0x1111u;
        unsigned prefix = starts & ((1u << word_count) - 1);
        unsigned matches = prefix, result = 0;
        for (unsigned word = 0; word < Words; ++word) {
          result |= matches & (less >> word);
          matches &= equal >> word;
        }
        if constexpr (Upper) result |= matches;
        return unsigned(std::popcount(result));
      }
    }
  }

  // Borrows sorted keys encoded as one, two, or four little-endian uint32 words.
  // Word zero is compared first; this is not a native-endian uint128 array.
  // The owner must outlive the view. Construction checks extent, not sortedness.
  // Bounds use local ordinals, permit duplicate keys and never allocate.
  template <std::size_t Words, simd::architecture Arch = simd::scalar> struct fixed_key_view {
    static_assert(Words == 1 || Words == 2 || Words == 4, "fixed keys have 1, 2, or 4 words");
    using architecture = Arch;
    using key_type = std::array<std::uint32_t, Words>;
    static constexpr std::size_t key_bytes = Words * 4;
    static constexpr std::size_t explicit_simd_limit = 32;
    fixed_key_view() = default;
    explicit fixed_key_view(std::span<std::byte const> bytes) : bytes_(bytes) {
      if (bytes.size() % key_bytes)
        error_detail::raise<std::invalid_argument>("fixed-key extent is not a whole number of keys");
    }
    std::size_t size() const noexcept { return bytes_.size() / key_bytes; }
    bool empty() const noexcept { return bytes_.empty(); }
    std::span<std::byte const> bytes() const noexcept { return bytes_; }
    key_type key_at(std::size_t ordinal) const {
      if (ordinal >= size()) error_detail::raise<std::out_of_range>("fixed-key ordinal");
      key_type result;
      auto source = bytes_.data() + ordinal * key_bytes;
      for (std::size_t i = 0; i < Words; ++i) result[i] = fixed_search_detail::load_word(source + (i << 2));
      return result;
    }
    fixed_key_view subview(std::size_t first, std::size_t count) const {
      if (first > size() || count > size() - first)
        error_detail::raise<std::out_of_range>("fixed-key subview");
      return fixed_key_view(bytes_.subspan(first * key_bytes, count * key_bytes));
    }
    std::size_t lower_bound(key_type const &query) const noexcept {
      return prefer_simd() ? simd_bound<false>(query) : binary_bound<false>(query);
    }
    std::size_t upper_bound(key_type const &query) const noexcept {
      return prefer_simd() ? simd_bound<true>(query) : binary_bound<true>(query);
    }
    std::size_t lower_bound_binary(key_type const &query) const noexcept { return binary_bound<false>(query); }
    std::size_t upper_bound_binary(key_type const &query) const noexcept { return binary_bound<true>(query); }
    std::size_t lower_bound_simd(key_type const &query) const noexcept { return simd_bound<false>(query); }
    std::size_t upper_bound_simd(key_type const &query) const noexcept { return simd_bound<true>(query); }
  private:
    std::span<std::byte const> bytes_;
    bool prefer_simd() const noexcept {
      // Measured lower-bound choices; explicit methods remain available for
      // other hosts/workloads. Complete 2^m-1 scalar windows are especially cheap.
      if constexpr (std::is_same_v<Arch, simd::neon> && std::endian::native == std::endian::little) {
        if constexpr (Words == 4)
          return (size() >= 2 && size() <= 4) || size() == 15 || size() == 16 || size() == 32;
        else return size() == 4;
      } else if constexpr (std::is_same_v<Arch, simd::avx2> && std::endian::native == std::endian::little) {
        if constexpr (Words == 1) return size() == 4;
        else if constexpr (Words == 2) return size() >= 2 && size() <= 4;
        else return size() == 2 || size() == 4;
      } else return false;
    }
    template <bool Upper> std::size_t binary_bound(key_type const &query) const noexcept {
      auto count = size();
      std::size_t first = 0;
      auto step = std::bit_floor(count);
      if (std::has_single_bit(count + 1)) {
        // Exactly log2(count+1) comparisons for a complete 2^m-1 window.
        for (; step; step >>= 1)
          first += step * fixed_search_detail::precedes<Words, Upper>(
            bytes_.data() + (first + step - 1) * key_bytes, query);
      } else {
        for (; step; step >>= 1) {
          auto next = first + step;
          auto probe = std::min(next, count) - 1;
          auto below = fixed_search_detail::precedes<Words, Upper>(bytes_.data() + probe * key_bytes, query);
          first += step * ((next <= count) & below);
        }
      }
      return first;
    }
    template <bool Upper> simd_inline void pivot_step(key_type const &query, std::size_t &first, std::size_t &count) const noexcept {
      // Called only with at least three keys, giving three distinct pivots.
      std::array<std::size_t, 3> positions{count >> 2, count >> 1, (3 * count) >> 2};
      unsigned rank;
      if constexpr (Words == 4 && std::is_same_v<Arch, simd::neon> && std::endian::native == std::endian::little) {
        auto queries = simd::vec<std::uint32_t, 4, Arch>::load(query.data());
        rank = 0;
        for (unsigned i = 0; i < 3; ++i)
          rank += fixed_search_detail::precedes_four<Upper, Arch>(bytes_.data() + (first + positions[i]) * key_bytes, queries);
      } else
      {
        std::array<std::byte, 3 * key_bytes> pivots;
        for (unsigned i = 0; i < 3; ++i)
          std::memcpy(pivots.data() + i * key_bytes, bytes_.data() + (first + positions[i]) * key_bytes, key_bytes);
        rank = fixed_search_detail::population<Words, Upper, Arch>(pivots.data(), 3, query);
      }
      auto start = ((rank * count) >> 2) + (rank != 0);
      auto end = ((rank + 1) * count) >> 2;
      first += start;
      count = end - start;
    }
    template <bool Upper, unsigned Bucket> simd_inline std::size_t small_bound(key_type const &query) const noexcept {
      std::size_t first = 0, count = size();
      if constexpr (Bucket > 4) pivot_step<Upper>(query, first, count);
      // Original sizes 17..32 leave 3..8 keys after the first pivot step.
      if constexpr (Bucket > 16) pivot_step<Upper>(query, first, count);
      if (!count) return first;
      return first + fixed_search_detail::population<Words, Upper, Arch>(bytes_.data() + first * key_bytes, unsigned(count), query);
    }
    template <bool Upper> simd_inline std::size_t simd_bound(key_type const &query) const noexcept {
      if (size() <= 4) return small_bound<Upper, 4>(query);
      if (size() <= 8) return small_bound<Upper, 8>(query);
      if (size() <= 16) return small_bound<Upper, 16>(query);
      if (size() <= explicit_simd_limit) return small_bound<Upper, 32>(query);
      return binary_bound<Upper>(query);
    }
  };
}
