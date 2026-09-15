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

namespace everett {
  // Population classes for virtual groups of K=2^n-1 entries. A class occupies
  // exactly n bits, including across word boundaries. One 64-bit checkpoint
  // every 128 classes bounds rank queries to 127 class reads (two words each
  // at most). No origin bitmap, arbitrary within-group rank, or select is kept.
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
