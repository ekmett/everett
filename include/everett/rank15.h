/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#if defined(__aarch64__) && defined(__ARM_NEON) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <arm_neon.h>
#endif

namespace everett {
  // Separate packed-class codec: 15-entry groups do not align with the
  // 512/2048 source-bit runs of rank.h. We retain four bits per class and one
  // 64-bit checkpoint every 128 classes. Queries accumulate at most eight
  // packed words and perform one horizontal sum.
  // There is no full origin bitvector and no arbitrary within-group rank.
  // Views check section shapes, not the semantic consistency of borrowed
  // classes/checkpoints; those must come from a builder or validated reader.
  struct rank15_view {
    rank15_view(std::span<std::uint64_t const> classes,
                std::span<std::uint64_t const> checkpoints,
                std::uint64_t virtual_count, std::uint64_t total)
      : classes_(classes), checkpoints_(checkpoints),
        virtual_count_(virtual_count),
        group_count_(virtual_count / 15 + (virtual_count % 15 != 0)), total_(total) {
      auto groups = group_count();
      if (classes.size() != groups / 16 + (groups % 16 != 0) ||
          checkpoints.size() != groups / 128 + (groups % 128 != 0) ||
          total > virtual_count)
        throw std::invalid_argument("invalid rank15 spans");
    }

    std::uint64_t size() const noexcept { return virtual_count_; }
    std::uint64_t group_count() const noexcept { return group_count_; }
    std::uint64_t count() const noexcept { return total_; }

    unsigned class_at(std::uint64_t group) const {
      if (group >= group_count()) throw std::out_of_range("rank15 class");
      return unsigned((classes_[group / 16] >> (4 * (group % 16))) & 15);
    }

    // rank(group) counts borrowed entries before virtual position 15*group;
    // group_count() is the actual-length endpoint of a partial final group.
    std::uint64_t rank(std::uint64_t group) const {
      if (group > group_count()) throw std::out_of_range("rank15 group");
      if (group == group_count()) return total_;
      auto result = checkpoints_[group / 128];
      if (group % 128 == 0) return result;
      auto word = (group / 128) * 8;
#if defined(__aarch64__) && defined(__ARM_NEON) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
      if (classes_.size() - word >= 8)
        return result + prefix128_neon(classes_.data() + word, unsigned(group % 128));
#endif
      // Each word contributes at most 30 to each byte. Eight words fit in
      // byte lanes (240), so we can accumulate before the horizontal sum.
      std::uint64_t pairs = 0;
      for (; word < group / 16; ++word) pairs += pair_nibbles(classes_[word]);
      auto tail = unsigned(group % 16);
      // The endpoint returned above, so this word exists even for tail=0.
      pairs += pair_nibbles(classes_[word] & ((std::uint64_t{1} << (4 * tail)) - 1));
      // Widen before reducing: four 16-bit lanes and their total (at most
      // 1920) fit without carries into the product's high 16 bits.
      pairs = (pairs & 0x00ff00ff00ff00ffull) + ((pairs >> 8) & 0x00ff00ff00ff00ffull);
      return result + ((pairs * 0x0001000100010001ull) >> 48);
    }

  private:
#if defined(__aarch64__) && defined(__ARM_NEON) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    static unsigned prefix128_neon(std::uint64_t const * words, unsigned count) noexcept {
      uint8x16_t const positions{0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30};
      auto boundary = vdupq_n_u8(count);
      auto selected_pairs = [&](uint8x16_t packed, unsigned base) {
        auto low_positions = vaddq_u8(positions, vdupq_n_u8(base));
        auto high_positions = vaddq_u8(low_positions, vdupq_n_u8(1));
        auto low = vandq_u8(packed, vdupq_n_u8(15));
        auto high = vshrq_n_u8(packed, 4);
        return vaddq_u8(vandq_u8(low, vcltq_u8(low_positions, boundary)),
                        vandq_u8(high, vcltq_u8(high_positions, boundary)));
      };
      auto bytes = reinterpret_cast<std::uint8_t const *>(words);
      auto a = selected_pairs(vld1q_u8(bytes), 0);
      auto b = selected_pairs(vld1q_u8(bytes + 16), 32);
      auto c = selected_pairs(vld1q_u8(bytes + 32), 64);
      auto d = selected_pairs(vld1q_u8(bytes + 48), 96);
      // Four vectors contribute at most 120 per byte. Widen the horizontal
      // reduction, whose maximum is 1920. The caller guarantees 64 readable bytes.
      return vaddlvq_u8(vaddq_u8(vaddq_u8(a, b), vaddq_u8(c, d)));
    }
#endif

    static std::uint64_t pair_nibbles(std::uint64_t value) noexcept {
      return (value & 0x0f0f0f0f0f0f0f0full) + ((value >> 4) & 0x0f0f0f0f0f0f0f0full);
    }

    std::span<std::uint64_t const> classes_;
    std::span<std::uint64_t const> checkpoints_;
    std::uint64_t virtual_count_;
    std::uint64_t group_count_;
    std::uint64_t total_;
  };

  struct rank15_index {
    static rank15_index build(std::span<std::uint8_t const> source, std::uint64_t count) {
      auto groups = count / 15 + (count % 15 != 0);
      if (source.size() != groups) throw std::invalid_argument("rank15 class length");
      rank15_index result;
      result.virtual_count = count;
      result.classes.resize(groups / 16 + (groups % 16 != 0));
      for (std::uint64_t i = 0; i < groups; ++i) {
        auto limit = i + 1 == groups && count % 15 ? count % 15 : 15;
        if (source[i] > limit) throw std::invalid_argument("rank15 class population");
        if (i % 128 == 0) result.checkpoints.push_back(result.total);
        result.classes[i / 16] |= std::uint64_t(source[i]) << (4 * (i % 16));
        result.total += source[i];
      }
      return result;
    }

    rank15_view view() const & { return {classes, checkpoints, virtual_count, total}; }

    rank15_view view() const && = delete;

    std::vector<std::uint64_t> classes;
    std::vector<std::uint64_t> checkpoints;
    std::uint64_t virtual_count = 0;
    std::uint64_t total = 0;
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Everett's rank15 support.
 */
