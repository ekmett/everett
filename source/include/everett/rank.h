/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Everett's rank support.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/backend.h>
#include <simd/integer.h>

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>


namespace everett {
  // Three ten-bit populations at bits 0, 11 and 22. The zero spacers
  // let a multiply sum up to 1536 without carrying between lanes.
  struct rank_block {
    std::uint32_t before;
    std::uint32_t runs;
  };
  static_assert(sizeof(rank_block) == 8);

  namespace rank_detail {
    // Prefix helpers take 0..512 bits. Vector variants require eight readable
    // words; rank_view keeps shorter allocation tails on the bounded path.
    // Read only words contributing to the prefix, including a masked tail.
    inline unsigned prefix512_portable(std::uint64_t const * words, unsigned bits) noexcept {
      unsigned result = 0;
      for (unsigned word = 0; word < (bits >> 6); ++word)
        result += unsigned(std::popcount(words[word]));
      if (bits & 63)
        result += unsigned(std::popcount(words[bits >> 6] & ((std::uint64_t{1} << (bits & 63)) - 1)));
      return result;
    }

    // Native kernels keep every intermediate in registers. Each byte contains
    // at most 32 population bits across the complete 512-bit input.
    template <simd::architecture Arch, unsigned Vector>
    simd_inline auto prefix_vector(std::uint64_t const * words, unsigned bits) noexcept {
      constexpr auto bytes = backend_detail::register_bytes<Arch>;
      using W = simd::vec<std::uint64_t, bytes / 8, Arch>;
      constexpr auto positions = [=] {
        std::array<std::uint64_t, bytes / 8> result{};
        for (unsigned i = 0; i < result.size(); ++i) result[i] = Vector * result.size() + i;
        return result;
      }();
      auto boundary = W(bits >> 6);
      auto mask = select(W(positions) < boundary, W(~std::uint64_t{0}),
        select(W(positions) == boundary, W((std::uint64_t{1} << (bits & 63)) - 1), W(0)));
      return popcount(reinterpret_bits<std::uint8_t>(W::loadu(words + Vector * (bytes / 8)) & mask));
    }

    template <simd::architecture Arch, unsigned Vector = 0>
    simd_inline auto prefix_vectors(std::uint64_t const * words, unsigned bits) noexcept {
      auto counts = prefix_vector<Arch, Vector>(words, bits);
      if constexpr ((Vector + 1) * backend_detail::register_bytes<Arch> == 64) return counts;
      else return counts + prefix_vectors<Arch, Vector + 1>(words, bits);
    }

    // Exactly eight readable words, with arbitrary uint64_t alignment.
    template <simd::architecture Arch = simd::scalar>
    simd_inline unsigned prefix512(std::uint64_t const * words, unsigned bits) noexcept {
      if constexpr (std::same_as<Arch, simd::scalar>) return prefix512_portable(words, bits);
      else return unsigned(reduce_add_widened(prefix_vectors<Arch>(words, bits)));
    }

    inline unsigned popcount512_portable(std::uint64_t const * words) noexcept {
      unsigned total = 0;
      for (unsigned i = 0; i < 8; ++i) total += unsigned(std::popcount(words[i]));
      return total;
    }

    template <simd::architecture Arch, unsigned Vector = 0>
    simd_inline auto population_vectors(std::uint8_t const * bytes) noexcept {
      constexpr auto width = backend_detail::register_bytes<Arch>;
      using V = simd::vec<std::uint8_t, width, Arch>;
      auto counts = popcount(V::loadu(bytes + Vector * width));
      if constexpr ((Vector + 1) * width == 64) return counts;
      else return counts + population_vectors<Arch, Vector + 1>(bytes);
    }

    template <simd::architecture Arch = simd::scalar>
    simd_inline unsigned popcount512(std::uint64_t const * words) noexcept {
      if constexpr (std::same_as<Arch, simd::scalar>) return popcount512_portable(words);
      else return unsigned(reduce_add_widened(population_vectors<Arch>(
        reinterpret_cast<std::uint8_t const *>(words))));
    }

    // Caller supplies 0 <= run <= 3 and independent counts in [0,512].
    // The stored spacer bits already leave eleven-bit lanes for the sum.
    constexpr unsigned run_prefix(std::uint32_t packed, unsigned run) noexcept {
      std::uint64_t selected = packed & ((std::uint64_t{1} << (11 * run)) - 1);
      return unsigned(((selected * 0x400801ull) >> 22) & 2047u);
    }

    // The builder calls this at each 2048-bit block. Kept independent of
    // payload allocation so counter transitions can be checked at 2^32 bits.
    struct directory_cursor {
      static constexpr bool starts_epoch(std::uint64_t block) noexcept {
        return (block & ((std::uint64_t{1} << 21) - 1)) == 0;
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
              std::uint64_t bit_count)
      : words_(words), blocks_(blocks), supers_(supers),
        bit_count_(bit_count) {
      if (bit_count > std::numeric_limits<std::uint64_t>::max() - 0xffffffffu ||
          words.size() != ((bit_count + 63) >> 6) ||
          blocks.size() != ((bit_count + 2047) >> 11) ||
          supers.size() != ((bit_count + 0xffffffffu) >> 32))
        throw std::invalid_argument("invalid rank spans");
    }

    std::uint64_t size() const noexcept { return bit_count_; }
    // The final bit belongs to an existing word, including a partial tail.
    template <simd::architecture Arch = simd::scalar>
    std::uint64_t count() const {
      if (!bit_count_) return 0;
      auto last = bit_count_ - 1;
      return rank<Arch>(last) + ((words_[last >> 6] >> (last & 63)) & 1);
    }

    // Exclusive rank at an existing bit. Use count() for the total population.
    template <simd::architecture Arch = simd::scalar>
    std::uint64_t rank(std::uint64_t position) const {
      if (position >= bit_count_) throw std::out_of_range("rank position");
      auto block = blocks_[position >> 11];
      unsigned run = unsigned((position >> 9) & 3);
      std::uint64_t result = supers_[position >> 32] + block.before;
      result += rank_detail::run_prefix(block.runs, run);
      auto word = (position >> 9) << 3;
      auto bits = unsigned(position & 511);
      if (!bits) return result;
      if constexpr (!std::same_as<Arch, simd::scalar>)
        if (words_.size() - word >= 8)
          return result + rank_detail::prefix512<Arch>(words_.data() + word, bits);
      return result + rank_detail::prefix512_portable(words_.data() + word, bits);
    }

  private:
    std::span<std::uint64_t const> words_;
    std::span<rank_block const> blocks_;
    std::span<std::uint64_t const> supers_;
    std::uint64_t bit_count_;
  };

  struct rank_index {
    template <simd::architecture Arch = simd::scalar>
    static rank_index build(std::span<std::uint64_t const> source, std::uint64_t bits) {
      if (bits > std::numeric_limits<std::uint64_t>::max() - 0xffffffffu ||
          source.size() != ((bits + 63) >> 6))
        throw std::invalid_argument("rank source length");
      rank_index result;
      result.bit_count = bits;
      result.words.assign(source.begin(), source.end());
      if (bits & 63) result.words.back() &= (std::uint64_t{1} << (bits & 63)) - 1;
      result.blocks.resize((bits + 2047) >> 11);
      rank_detail::directory_cursor cursor;
      std::uint64_t total = 0;
      auto begin_block = [&](std::uint64_t block) -> rank_block & {
        auto & entry = result.blocks[block];
        entry.before = cursor.before(block, total);
        if (cursor.starts_epoch(block)) result.supers.push_back(cursor.epoch_base);
        return entry;
      };
      auto full_blocks = bits >> 11;
      for (std::uint64_t block = 0; block < full_blocks; ++block) {
        auto & entry = begin_block(block);
        auto words = result.words.data() + block * 32;
        auto a = rank_detail::popcount512<Arch>(words);
        auto b = rank_detail::popcount512<Arch>(words + 8);
        auto c = rank_detail::popcount512<Arch>(words + 16);
        auto d = rank_detail::popcount512<Arch>(words + 24);
        entry.runs = a | (b << 11) | (c << 22);
        total += a + b + c + d;
      }
      if (full_blocks != result.blocks.size()) {
        auto & entry = begin_block(full_blocks);
        unsigned counts[4]{};
        auto first = full_blocks * 32;
        // Only the final partial block needs bounded word loads. The owning
        // copy has already cleared unused bits in its final word.
        for (auto word = first; word < result.words.size(); ++word)
          counts[(word - first) >> 3] += unsigned(std::popcount(result.words[word]));
        entry.runs = counts[0] | (counts[1] << 11) | (counts[2] << 22);
        total += counts[0] + counts[1] + counts[2] + counts[3];
      }
      return result;
    }

    rank_view view() const & { return {words, blocks, supers, bit_count}; }

    rank_view view() const && = delete;

    // Exposed sections allow a checked reader to supply mapped spans without
    // copying through this owning build representation.
    std::vector<std::uint64_t> words;
    std::vector<rank_block> blocks;
    std::vector<std::uint64_t> supers;
    std::uint64_t bit_count = 0;
  };
}
