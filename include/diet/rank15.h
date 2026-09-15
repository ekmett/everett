/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Diet's rank15 support.
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <diet/error_detail.h>

#include <diet/word_view.h>

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#if defined(__aarch64__) && defined(__ARM_NEON) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <arm_neon.h>
#endif
#if defined(__AVX2__) || (defined(__AVX512F__) && defined(__AVX512BW__))
#include <immintrin.h>
#endif

namespace diet {
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
                std::uint64_t virtual_count)
      : rank15_view(word_view(classes), word_view(checkpoints), virtual_count) {}

    template <class Words> requires std::is_same_v<Words, word_view>
    rank15_view(Words classes, Words checkpoints,
                std::uint64_t virtual_count)
      : classes_(classes), checkpoints_(checkpoints),
        virtual_count_(virtual_count),
        group_count_(virtual_count / 15 + (virtual_count % 15 != 0)) {
      auto groups = group_count();
      if (classes.size() != ((groups + 15) >> 4) ||
          checkpoints.size() != ((groups + 127) >> 7))
        error_detail::raise<std::invalid_argument>("invalid rank15 spans");
    }

    std::uint64_t size() const noexcept { return virtual_count_; }
    std::uint64_t group_count() const noexcept { return group_count_; }
    // Derived from the final real group; no endpoint checkpoint is stored.
    std::uint64_t count() const {
      if (!group_count()) return 0;
      auto last = group_count() - 1;
      auto population = class_at(last);
      if (population > virtual_count_ - last * 15)
        error_detail::raise<std::invalid_argument>("invalid rank15 final population");
      return add_prefix(rank(last), population, virtual_count_);
    }

    word_view class_words() const noexcept { return classes_; }
    word_view checkpoint_words() const noexcept { return checkpoints_; }

    unsigned class_at(std::uint64_t group) const {
      if (group >= group_count()) error_detail::raise<std::out_of_range>("rank15 class");
      return unsigned((classes_[group >> 4] >> (4 * (group & 15))) & 15);
    }

    // rank(group) counts entries before the start of an existing group.
    // An empty index has no valid rank query; count() handles its total.
    std::uint64_t rank(std::uint64_t group) const {
      if (group >= group_count()) error_detail::raise<std::out_of_range>("rank15 group");
      auto result = checkpoints_[group >> 7];
      auto limit = virtual_count_;
      if ((group & 127) == 0) return add_prefix(result, 0, limit);
      auto word = (group >> 7) << 3;
#if defined(__AVX512F__) && defined(__AVX512BW__)
      if (classes_.size() - word >= 8)
        return add_prefix(result, prefix128_avx512(classes_.bytes().data() + word * 8, unsigned(group & 127)), limit);
#elif defined(__AVX2__)
      if (classes_.size() - word >= 8)
        return add_prefix(result, prefix128_avx2(classes_.bytes().data() + word * 8, unsigned(group & 127)), limit);
#elif defined(__aarch64__) && defined(__ARM_NEON) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
      if (classes_.size() - word >= 8)
        return add_prefix(result, prefix128_neon(classes_.bytes().data() + word * 8, unsigned(group & 127)), limit);
#endif
      // Each word contributes at most 30 to each byte. Eight words fit in
      // byte lanes (240), so we can accumulate before the horizontal sum.
      std::uint64_t pairs = 0;
      for (; word < (group >> 4); ++word) pairs += pair_nibbles(classes_[word]);
      auto tail = unsigned(group & 15);
      // group is an existing class, so this word exists even for tail=0.
      pairs += pair_nibbles(classes_[word] & ((std::uint64_t{1} << (4 * tail)) - 1));
      return add_prefix(result, sum_bytes(pairs), limit);
    }

  private:
    static std::uint64_t add_prefix(std::uint64_t checkpoint, unsigned prefix, std::uint64_t limit) {
      if (checkpoint > limit || prefix > limit - checkpoint)
        error_detail::raise<std::invalid_argument>("invalid rank15 checkpoint or prefix");
      return checkpoint + prefix;
    }
#if defined(__AVX512F__) && defined(__AVX512BW__)
    static unsigned prefix128_avx512(std::byte const * words, unsigned count) noexcept {
      auto positions = _mm512_set_epi8(
        126, 124, 122, 120, 118, 116, 114, 112, 110, 108, 106, 104, 102, 100, 98, 96,
        94, 92, 90, 88, 86, 84, 82, 80, 78, 76, 74, 72, 70, 68, 66, 64,
        62, 60, 58, 56, 54, 52, 50, 48, 46, 44, 42, 40, 38, 36, 34, 32,
        30, 28, 26, 24, 22, 20, 18, 16, 14, 12, 10, 8, 6, 4, 2, 0);
      auto boundary = _mm512_set1_epi8(char(count));
      auto mask = _mm512_set1_epi8(15);
      auto packed = _mm512_loadu_si512(words);
      auto low = _mm512_and_si512(packed, mask);
      // The word shift moves neighboring-byte bits too; the mask removes them.
      auto high = _mm512_and_si512(_mm512_srli_epi16(packed, 4), mask);
      auto selected_low = _mm512_cmplt_epu8_mask(positions, boundary);
      auto selected_high = _mm512_cmplt_epu8_mask(_mm512_add_epi8(positions, _mm512_set1_epi8(1)), boundary);
      auto pairs = _mm512_add_epi8(_mm512_maskz_mov_epi8(selected_low, low),
                                   _mm512_maskz_mov_epi8(selected_high, high));
      // Each byte is at most 30. SAD widens groups of eight bytes before the
      // qword reduction, whose maximum is 1920. Exactly 64 bytes are readable.
      return unsigned(_mm512_reduce_add_epi64(_mm512_sad_epu8(pairs, _mm512_setzero_si512())));
    }
#elif defined(__AVX2__)
    static unsigned prefix128_avx2(std::byte const * words, unsigned count) noexcept {
      auto positions = _mm256_setr_epi8(
        0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30,
        32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62);
      auto boundary = _mm256_set1_epi8(char(count));
      auto mask = _mm256_set1_epi8(15);
      auto selected_pairs = [&](auto packed, unsigned base) {
        auto low_positions = _mm256_add_epi8(positions, _mm256_set1_epi8(char(base)));
        auto high_positions = _mm256_add_epi8(low_positions, _mm256_set1_epi8(1));
        auto low = _mm256_and_si256(packed, mask);
        auto high = _mm256_and_si256(_mm256_srli_epi16(packed, 4), mask);
        // Positions and count are in [0,127], so signed byte comparisons suffice.
        return _mm256_add_epi8(_mm256_and_si256(low, _mm256_cmpgt_epi8(boundary, low_positions)),
                              _mm256_and_si256(high, _mm256_cmpgt_epi8(boundary, high_positions)));
      };
      auto a = selected_pairs(_mm256_loadu_si256(reinterpret_cast<__m256i const *>(words)), 0);
      auto b = selected_pairs(_mm256_loadu_si256(reinterpret_cast<__m256i const *>(words + 32)), 64);
      // Two vectors contribute at most 60 per byte. Widen before reducing the
      // total, which fits in 32 bits (at most 1920). Exactly 64 bytes are readable.
      auto totals = _mm256_sad_epu8(_mm256_add_epi8(a, b), _mm256_setzero_si256());
      auto halves = _mm_add_epi64(_mm256_castsi256_si128(totals), _mm256_extracti128_si256(totals, 1));
      return unsigned(_mm_cvtsi128_si32(_mm_add_epi64(halves, _mm_srli_si128(halves, 8))));
    }
#endif

#if defined(__aarch64__) && defined(__ARM_NEON) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    static unsigned prefix128_neon(std::byte const * words, unsigned count) noexcept {
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

    static unsigned sum_bytes(std::uint64_t value) noexcept {
      // Byte lanes are at most 240. Widen before the horizontal sum: four
      // 16-bit lanes and their total (at most 1920) fit without carries.
      value = (value & 0x00ff00ff00ff00ffull) + ((value >> 8) & 0x00ff00ff00ff00ffull);
      return unsigned((value * 0x0001000100010001ull) >> 48);
    }

    word_view classes_;
    word_view checkpoints_;
    std::uint64_t virtual_count_;
    std::uint64_t group_count_;
  };

  struct rank15_index {
    static rank15_index build(std::span<std::uint8_t const> source, std::uint64_t count) {
      auto groups = count / 15 + (count % 15 != 0);
      if (source.size() != groups) error_detail::raise<std::invalid_argument>("rank15 class length");
      rank15_index result;
      result.virtual_count = count;
      result.classes.resize((groups + 15) >> 4);
      std::uint64_t total = 0;
      for (std::uint64_t i = 0; i < groups; ++i) {
        auto limit = i + 1 == groups && count % 15 ? count % 15 : 15;
        if (source[i] > limit) error_detail::raise<std::invalid_argument>("rank15 class population");
        if ((i & 127) == 0) result.checkpoints.push_back(total);
        result.classes[i >> 4] |= std::uint64_t(source[i]) << (4 * (i & 15));
        total += source[i];
      }
      return result;
    }

    rank15_view view() const & { return {classes, checkpoints, virtual_count}; }

    rank15_view view() const && = delete;

    std::vector<std::uint64_t> classes;
    std::vector<std::uint64_t> checkpoints;
    std::uint64_t virtual_count = 0;
  };
}
