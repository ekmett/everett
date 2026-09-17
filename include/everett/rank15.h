/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Everett's rank15 support.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/backend.h>
#include <everett/error_detail.h>
#include <simd/integer.h>

#include <everett/word_view.h>

#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>


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
    template <simd::architecture Arch = simd::scalar>
    std::uint64_t count() const {
      if (!group_count()) return 0;
      auto last = group_count() - 1;
      auto population = class_at(last);
      if (population > virtual_count_ - last * 15)
        error_detail::raise<std::invalid_argument>("invalid rank15 final population");
      return add_prefix(rank<Arch>(last), population, virtual_count_);
    }

    word_view class_words() const noexcept { return classes_; }
    word_view checkpoint_words() const noexcept { return checkpoints_; }

    unsigned class_at(std::uint64_t group) const {
      if (group >= group_count()) error_detail::raise<std::out_of_range>("rank15 class");
      return unsigned((classes_[group >> 4] >> (4 * (group & 15))) & 15);
    }

    // rank(group) counts entries before the start of an existing group.
    // An empty index has no valid rank query; count() handles its total.
    template <simd::architecture Arch = simd::scalar>
    std::uint64_t rank(std::uint64_t group) const {
      if (group >= group_count()) error_detail::raise<std::out_of_range>("rank15 group");
      auto result = checkpoints_[group >> 7];
      auto limit = virtual_count_;
      if ((group & 127) == 0) return add_prefix(result, 0, limit);
      auto word = (group >> 7) << 3;
      if constexpr (!std::same_as<Arch, simd::scalar> && std::endian::native == std::endian::little)
        if (classes_.size() - word >= 8)
          return add_prefix(result, unsigned(reduce_add_widened(prefix_vectors<Arch>(
            classes_.bytes().data() + word * 8, unsigned(group & 127)))), limit);
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
    template <simd::architecture Arch, unsigned Vector = 0>
    static simd_inline auto prefix_vectors(std::byte const * words, unsigned count) noexcept {
      constexpr auto width = backend_detail::register_bytes<Arch>;
      using V = simd::vec<std::uint8_t, width, Arch>;
      constexpr auto positions = [=] {
        std::array<std::uint8_t, width> result{};
        for (unsigned i = 0; i < result.size(); ++i) result[i] = 2 * (Vector * width + i);
        return result;
      }();
      auto packed = V::loadu(reinterpret_cast<std::uint8_t const *>(words) + Vector * width);
      auto low = packed & V(15);
      auto high = packed.template right<4>();
      auto boundary = V(count);
      auto pairs = select(V(positions) < boundary, low, V(0)) +
                   select(V(positions) + V(1) < boundary, high, V(0));
      // At most four registers contribute: each byte sum is <=120. The
      // caller widens exactly once before reducing the <=1920 total.
      if constexpr ((Vector + 1) * width == 64) return pairs;
      else return pairs + prefix_vectors<Arch, Vector + 1>(words, count);
    }

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
