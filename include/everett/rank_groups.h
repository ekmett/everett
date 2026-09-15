/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/rank15.h>

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace everett {
  namespace rank_groups_detail {
    template <unsigned Bits, unsigned Word, unsigned Plane>
    constexpr std::uint64_t plane_mask() noexcept {
      std::uint64_t mask = 0;
      for (unsigned bit = 0; bit < 64; ++bit)
        if ((Word * 64 + bit) % Bits == Plane) mask |= std::uint64_t{1} << bit;
      return mask;
    }

    // The caller supplies at most 128 classes and exactly the words containing
    // them. Weighted bit populations preserve fields split across word cuts.
    template <unsigned Bits> inline unsigned prefix_portable(
        std::uint64_t const * words, unsigned count) noexcept {
      static_assert(Bits == 2 || Bits == 3 || Bits == 5);
      constexpr auto masks = [] {
        std::array<std::array<std::uint64_t, Bits>, Bits> result{};
        for (unsigned word = 0; word < Bits; ++word)
          for (unsigned bit = 0; bit < 64; ++bit)
            result[word][(word * 64 + bit) % Bits] |= std::uint64_t{1} << bit;
        return result;
      }();
      unsigned result = 0;
      std::uint64_t pairs = 0;
      auto bits = count * Bits;
      for (unsigned word = 0; word * 64 < bits; ++word) {
        auto value = words[word];
        auto remaining = bits - word * 64;
        if (remaining < 64) value &= (std::uint64_t{1} << remaining) - 1;
        if constexpr (Bits == 2) {
          value = (value & 0x3333333333333333ull) + ((value >> 2) & 0x3333333333333333ull);
          pairs += (value & 0x0f0f0f0f0f0f0f0full) + ((value >> 4) & 0x0f0f0f0f0f0f0f0full);
        } else {
          for (unsigned plane = 0; plane < Bits; ++plane)
            result += unsigned(std::popcount(value & masks[word % Bits][plane])) << plane;
        }
      }
      if constexpr (Bits == 2) {
        // Four two-bit classes per byte contribute at most twelve per word;
        // four words fit in byte lanes. Widen before summing their total.
        pairs = (pairs & 0x00ff00ff00ff00ffull) + ((pairs >> 8) & 0x00ff00ff00ff00ffull);
        return unsigned((pairs * 0x0001000100010001ull) >> 48);
      }
      return result;
    }

#if defined(__aarch64__) && defined(__ARM_NEON) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    template <unsigned Bits, unsigned Vector, unsigned Plane = 0>
    inline uint8x16_t weighted_bits(uint8x16_t value) noexcept {
      if constexpr (Bits == 2) {
        auto pairs = vaddq_u8(vandq_u8(value, vdupq_n_u8(0x33)),
                              vandq_u8(vshrq_n_u8(value, 2), vdupq_n_u8(0x33)));
        return vaddq_u8(vandq_u8(pairs, vdupq_n_u8(15)), vshrq_n_u8(pairs, 4));
      }
      uint64x2_t mask{plane_mask<Bits, 2 * Vector, Plane>(),
                     plane_mask<Bits, 2 * Vector + 1, Plane>()};
      auto population = vshlq_n_u8(vcntq_u8(vandq_u8(value, vreinterpretq_u8_u64(mask))), Plane);
      if constexpr (Plane + 1 == Bits) return population;
      else return vaddq_u8(population, weighted_bits<Bits, Vector, Plane + 1>(value));
    }

    template <unsigned Bits, unsigned Vector = 0>
    inline uint16x8_t prefix_vectors(std::uint64_t const * words, unsigned bits) noexcept {
      uint64x2_t positions{2 * Vector, 2 * Vector + 1};
      auto boundary = vdupq_n_u64(bits / 64);
      auto tail = vdupq_n_u64((std::uint64_t{1} << (bits % 64)) - 1);
      auto mask = vorrq_u64(vcltq_u64(positions, boundary),
                            vandq_u64(vceqq_u64(positions, boundary), tail));
      auto selected = vandq_u64(vld1q_u64(words + 2 * Vector), mask);
      // Widen before adding vectors: five-bit classes can otherwise overflow
      // an eight-bit lane. The complete checkpoint sums to at most 128*31.
      auto counts = vpaddlq_u8(weighted_bits<Bits, Vector>(vreinterpretq_u8_u64(selected)));
      if constexpr (Vector + 1 == Bits) return counts;
      else return vaddq_u16(counts, prefix_vectors<Bits, Vector + 1>(words, bits));
    }

    // A complete checkpoint has exactly 2*Bits words. Each selected bit
    // contributes its class-place weight, even when a class straddles words.
    template <unsigned Bits> inline unsigned prefix_neon(
        std::uint64_t const * words, unsigned count) noexcept {
      return vaddvq_u16(prefix_vectors<Bits>(words, count * Bits));
    }
#endif
  }

  // Population classes for virtual groups of K=2^n-1 entries. A class occupies
  // exactly n bits, including across word boundaries. One 64-bit checkpoint
  // every 128 classes bounds general rank queries to 127 class reads. K=3,7,31
  // reduce packed words; K=15 shares rank15's packed-word/SIMD reduction.
  // No origin bitmap, arbitrary within-group rank, or select is kept.
  // Views check section shapes; a reader must validate borrowed metadata.
  template <std::uint64_t K> struct rank_groups_view {
    static_assert(K >= 3 && K < std::numeric_limits<std::uint64_t>::max() && std::has_single_bit(K + 1),
                  "group size must be 2^n-1 and at least three");
    static constexpr std::uint64_t group_size = K;
    static constexpr unsigned class_bits = unsigned(std::countr_zero(K + 1));

    rank_groups_view(std::span<std::uint64_t const> classes,
                     std::span<std::uint64_t const> checkpoints,
                     std::uint64_t virtual_count, std::uint64_t total)
      : classes_(classes), checkpoints_(checkpoints), virtual_count_(virtual_count), total_(total) {
      auto groups = group_count();
      if (groups > std::numeric_limits<std::uint64_t>::max() / class_bits)
        throw std::overflow_error("rank groups packed size");
      auto bits = groups * class_bits;
      if (classes.size() != bits / 64 + (bits % 64 != 0) ||
          checkpoints.size() != groups / 128 + (groups % 128 != 0) || total > virtual_count)
        throw std::invalid_argument("invalid rank groups spans");
    }

    std::uint64_t size() const noexcept { return virtual_count_; }
    std::uint64_t count() const noexcept { return total_; }
    std::uint64_t group_count() const noexcept {
      return virtual_count_ / K + (virtual_count_ % K != 0);
    }
    std::uint64_t class_at(std::uint64_t group) const {
      if (group >= group_count()) throw std::out_of_range("rank groups class");
      return read_class(group);
    }
    // Exclusive prefix at K*group, with the final endpoint clamped to actual N.
    std::uint64_t rank(std::uint64_t group) const {
      if (group > group_count()) throw std::out_of_range("rank groups boundary");
      if (group == group_count()) return total_;
      auto result = checkpoints_[group / 128];
      if constexpr (K == 3 || K == 7 || K == 31) {
        auto count = unsigned(group % 128);
        if (!count) return result;
        auto word = (group / 128) * (2 * class_bits);
#if defined(__aarch64__) && defined(__ARM_NEON) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        // Two-bit broadword sums have lower dependent query latency. Wider
        // classes favor NEON for the rank-plus-class projection operation.
        if constexpr (K != 3)
          if (classes_.size() - word >= 2 * class_bits)
            return result + rank_groups_detail::prefix_neon<class_bits>(classes_.data() + word, count);
#endif
        return result + rank_groups_detail::prefix_portable<class_bits>(classes_.data() + word, count);
      }
      for (auto i = (group / 128) * 128; i < group; ++i) result += read_class(i);
      return result;
    }

  private:
    std::uint64_t read_class(std::uint64_t group) const noexcept {
      auto bit = group * class_bits;
      auto word = bit / 64;
      unsigned shift = unsigned(bit % 64);
      auto value = classes_[word] >> shift;
      if (shift + class_bits > 64) value |= classes_[word + 1] << (64 - shift);
      return value & K;
    }
    std::span<std::uint64_t const> classes_;
    std::span<std::uint64_t const> checkpoints_;
    std::uint64_t virtual_count_;
    std::uint64_t total_;
  };

  template <> struct rank_groups_view<15> {
    static constexpr std::uint64_t group_size = 15;
    static constexpr unsigned class_bits = 4;

    rank_groups_view(std::span<std::uint64_t const> classes,
                     std::span<std::uint64_t const> checkpoints,
                     std::uint64_t virtual_count, std::uint64_t total)
      : view_(classes, checkpoints, virtual_count, total) {}

    std::uint64_t size() const noexcept { return view_.size(); }
    std::uint64_t count() const noexcept { return view_.count(); }
    std::uint64_t group_count() const noexcept { return view_.group_count(); }
    std::uint64_t class_at(std::uint64_t group) const { return view_.class_at(group); }
    std::uint64_t rank(std::uint64_t group) const { return view_.rank(group); }

  private:
    rank15_view view_;
  };

  template <std::uint64_t K> struct rank_groups {
    static_assert(K >= 3 && K < std::numeric_limits<std::uint64_t>::max() && std::has_single_bit(K + 1),
                  "group size must be 2^n-1 and at least three");
    static constexpr std::uint64_t group_size = K;
    static constexpr unsigned class_bits = unsigned(std::countr_zero(K + 1));

    static rank_groups build(std::span<std::uint64_t const> source, std::uint64_t count) {
      auto groups = count / K + (count % K != 0);
      if (source.size() != groups) throw std::invalid_argument("rank groups class length");
      if (groups > std::numeric_limits<std::uint64_t>::max() / class_bits)
        throw std::overflow_error("rank groups packed size");
      auto bits = groups * class_bits;
      rank_groups result;
      result.virtual_count = count;
      result.classes.resize(bits / 64 + (bits % 64 != 0));
      for (std::uint64_t i = 0; i < groups; ++i) {
        auto limit = i + 1 == groups && count % K ? count % K : K;
        auto value = source[i];
        if (value > limit) throw std::invalid_argument("rank groups population");
        if (i % 128 == 0) result.checkpoints.push_back(result.total);
        auto bit = i * class_bits;
        unsigned shift = unsigned(bit % 64);
        result.classes[bit / 64] |= value << shift;
        if (shift + class_bits > 64) result.classes[bit / 64 + 1] |= value >> (64 - shift);
        result.total += value;
      }
      return result;
    }

    rank_groups_view<K> view() const & { return {classes, checkpoints, virtual_count, total}; }
    rank_groups_view<K> view() const && = delete;

    std::vector<std::uint64_t> classes;
    std::vector<std::uint64_t> checkpoints;
    std::uint64_t virtual_count = 0;
    std::uint64_t total = 0;
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Everett's rank groups support.
 */
