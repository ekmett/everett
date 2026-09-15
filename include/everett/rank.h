/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace everett {
  // Three independent ten-bit population counts, not cumulative ranks: the
  // latter would need eleven bits to represent the first three 512-bit runs.
  struct rank_block {
    std::uint32_t before;
    std::uint32_t runs;
  };
  static_assert(sizeof(rank_block) == 8);

  namespace rank_detail {
    // Read only words contributing to the prefix, including a masked tail.
    inline unsigned prefix512_portable(std::uint64_t const * words, unsigned bits) noexcept {
      unsigned result = 0;
      for (unsigned word = 0; word < bits / 64; ++word)
        result += unsigned(std::popcount(words[word]));
      if (bits % 64)
        result += unsigned(std::popcount(words[bits / 64] & ((std::uint64_t{1} << (bits % 64)) - 1)));
      return result;
    }

#if defined(__aarch64__) && defined(__ARM_NEON)
    template <unsigned Vector> inline uint8x16_t prefix512_vector(
        std::uint64_t const * words, unsigned bits) noexcept {
      uint64x2_t positions{2 * Vector, 2 * Vector + 1};
      auto boundary = vdupq_n_u64(bits / 64);
      auto tail = vdupq_n_u64((std::uint64_t{1} << (bits % 64)) - 1);
      auto mask = vorrq_u64(vcltq_u64(positions, boundary),
                            vandq_u64(vceqq_u64(positions, boundary), tail));
      return vcntq_u8(vreinterpretq_u8_u64(vandq_u64(vld1q_u64(words + 2 * Vector), mask)));
    }

    // Exactly eight readable words, with arbitrary uint64_t alignment.
    inline unsigned prefix512_neon(std::uint64_t const * words, unsigned bits) noexcept {
      auto a = prefix512_vector<0>(words, bits);
      auto b = prefix512_vector<1>(words, bits);
      auto c = prefix512_vector<2>(words, bits);
      auto d = prefix512_vector<3>(words, bits);
      return vaddlvq_u8(vaddq_u8(vaddq_u8(a, b), vaddq_u8(c, d)));
    }
#endif

    // Exactly eight readable words; no alignment beyond uint64_t is required.
    inline unsigned popcount512_portable(std::uint64_t const * words) noexcept {
      unsigned total = 0;
      for (unsigned i = 0; i < 8; ++i) total += unsigned(std::popcount(words[i]));
      return total;
    }

    inline unsigned popcount512(std::uint64_t const * words) noexcept {
#if defined(__aarch64__) && defined(__ARM_NEON)
      auto bytes = reinterpret_cast<std::uint8_t const *>(words);
      auto a = vcntq_u8(vld1q_u8(bytes));
      auto b = vcntq_u8(vld1q_u8(bytes + 16));
      auto c = vcntq_u8(vld1q_u8(bytes + 32));
      auto d = vcntq_u8(vld1q_u8(bytes + 48));
      // Each byte lane sums to at most 32; the final horizontal sum needs
      // sixteen bits because a completely full 512-bit run contains 512 ones.
      return vaddlvq_u8(vaddq_u8(vaddq_u8(a, b), vaddq_u8(c, d)));
#else
      return popcount512_portable(words);
#endif
    }

    // Caller supplies 0 <= run <= 3 and independent counts in [0,512].
    // Widen to eleven-bit lanes before summing: 512+512+512 needs eleven
    // bits, although each stored population needs only ten.
    constexpr unsigned run_prefix(std::uint32_t packed, unsigned run) noexcept {
      std::uint64_t selected = packed & ((std::uint64_t{1} << (10 * run)) - 1);
      auto widened = (selected & 0x3ffu) |
                     ((selected & 0xffc00u) << 1) |
                     ((selected & 0x3ff00000u) << 2);
      return unsigned(((widened * 0x400801ull) >> 22) & 2047u);
    }

    // The builder calls this at each 2048-bit block. Kept independent of
    // payload allocation so counter transitions can be checked at 2^32 bits.
    struct directory_cursor {
      static constexpr bool starts_epoch(std::uint64_t block) noexcept {
        return block % (std::uint64_t{1} << 21) == 0;
      }
      std::uint32_t before(std::uint64_t block, std::uint64_t total) {
        if (starts_epoch(block)) epoch_base = total;
        if (total < epoch_base || total - epoch_base > std::numeric_limits<std::uint32_t>::max())
          throw std::overflow_error("rank relative count");
        return std::uint32_t(total - epoch_base);
      }
      std::uint64_t epoch_base = 0;
    };
  }

  // Borrowed, native-endian/aligned spans. A future file-format reader owns
  // endian conversion and mapping lifetime. The directory has 64 bits per
  // 2048 source bits, plus one 64-bit absolute rank per 2^32 source bits.
  // This structure supports rank only; it contains no select directory.
  // Construction checks section shapes, not directory contents. Borrowed
  // metadata must come from the builder or an independently validated reader.
  struct rank_view {
    rank_view(std::span<std::uint64_t const> words,
              std::span<rank_block const> blocks,
              std::span<std::uint64_t const> supers,
              std::uint64_t bit_count, std::uint64_t total)
      : words_(words), blocks_(blocks), supers_(supers),
        bit_count_(bit_count), total_(total) {
      if (words.size() != bit_count / 64 + (bit_count % 64 != 0) ||
          blocks.size() != bit_count / 2048 + (bit_count % 2048 != 0) ||
          supers.size() != (bit_count >> 32) + ((bit_count & 0xffffffffu) != 0) ||
          total > bit_count)
        throw std::invalid_argument("invalid rank spans");
    }

    std::uint64_t size() const noexcept { return bit_count_; }
    std::uint64_t count() const noexcept { return total_; }

    // Exclusive rank. The endpoint is valid; positions beyond it are errors.
    std::uint64_t rank(std::uint64_t position) const {
      if (position > bit_count_) throw std::out_of_range("rank position");
      if (position == bit_count_) return total_;
      auto block = blocks_[position / 2048];
      unsigned run = unsigned((position / 512) % 4);
      std::uint64_t result = supers_[position >> 32] + block.before;
      result += rank_detail::run_prefix(block.runs, run);
      auto word = (position / 512) * 8;
      auto bits = unsigned(position % 512);
      if (!bits) return result;
#if defined(__aarch64__) && defined(__ARM_NEON)
      if (words_.size() - word >= 8)
        return result + rank_detail::prefix512_neon(words_.data() + word, bits);
#endif
      return result + rank_detail::prefix512_portable(words_.data() + word, bits);
    }

  private:
    std::span<std::uint64_t const> words_;
    std::span<rank_block const> blocks_;
    std::span<std::uint64_t const> supers_;
    std::uint64_t bit_count_;
    std::uint64_t total_;
  };

  struct rank_index {
    static rank_index build(std::span<std::uint64_t const> source, std::uint64_t bits) {
      if (source.size() != bits / 64 + (bits % 64 != 0))
        throw std::invalid_argument("rank source length");
      rank_index result;
      result.bit_count = bits;
      result.words.assign(source.begin(), source.end());
      if (bits % 64) result.words.back() &= (std::uint64_t{1} << (bits % 64)) - 1;
      result.blocks.resize(bits / 2048 + (bits % 2048 != 0));
      rank_detail::directory_cursor cursor;
      auto begin_block = [&](std::uint64_t block) -> rank_block & {
        auto & entry = result.blocks[block];
        entry.before = cursor.before(block, result.total);
        if (cursor.starts_epoch(block)) result.supers.push_back(cursor.epoch_base);
        return entry;
      };
      auto full_blocks = bits / 2048;
      for (std::uint64_t block = 0; block < full_blocks; ++block) {
        auto & entry = begin_block(block);
        auto words = result.words.data() + block * 32;
        auto a = rank_detail::popcount512(words);
        auto b = rank_detail::popcount512(words + 8);
        auto c = rank_detail::popcount512(words + 16);
        auto d = rank_detail::popcount512(words + 24);
        entry.runs = a | (b << 10) | (c << 20);
        result.total += a + b + c + d;
      }
      if (full_blocks != result.blocks.size()) {
        auto & entry = begin_block(full_blocks);
        unsigned counts[4]{};
        auto first = full_blocks * 32;
        // Only the final partial block needs bounded word loads. The owning
        // copy has already cleared unused bits in its final word.
        for (auto word = first; word < result.words.size(); ++word)
          counts[(word - first) / 8] += unsigned(std::popcount(result.words[word]));
        entry.runs = counts[0] | (counts[1] << 10) | (counts[2] << 20);
        result.total += counts[0] + counts[1] + counts[2] + counts[3];
      }
      return result;
    }

    rank_view view() const & { return {words, blocks, supers, bit_count, total}; }

    rank_view view() const && = delete;

    // Exposed sections allow a checked reader to supply mapped spans without
    // copying through this owning build representation.
    std::vector<std::uint64_t> words;
    std::vector<rank_block> blocks;
    std::vector<std::uint64_t> supers;
    std::uint64_t bit_count = 0;
    std::uint64_t total = 0;
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Everett's rank support.
 */
