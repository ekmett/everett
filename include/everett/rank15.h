#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace everett {
  // Separate packed-class codec: 15-entry groups do not align with the
  // 512/2048 source-bit runs of rank.h. We retain four bits per class and one
  // 64-bit checkpoint every 128 classes, with at most eight word sums/query.
  // There is no full origin bitvector and no arbitrary within-group rank.
  // Views check section shapes, not the semantic consistency of borrowed
  // classes/checkpoints; those must come from a builder or validated reader.
  struct rank15_view {
    rank15_view(std::span<std::uint64_t const> classes,
                std::span<std::uint64_t const> checkpoints,
                std::uint64_t virtual_count, std::uint64_t total)
      : classes_(classes), checkpoints_(checkpoints),
        virtual_count_(virtual_count), total_(total) {
      auto groups = group_count();
      if (classes.size() != groups / 16 + (groups % 16 != 0) ||
          checkpoints.size() != groups / 128 + (groups % 128 != 0) ||
          total > virtual_count)
        throw std::invalid_argument("invalid rank15 spans");
    }

    std::uint64_t size() const noexcept { return virtual_count_; }
    std::uint64_t group_count() const noexcept {
      return virtual_count_ / 15 + (virtual_count_ % 15 != 0);
    }
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
      auto word = (group / 128) * 8;
      for (; word < group / 16; ++word) result += sum_nibbles(classes_[word]);
      auto tail = unsigned(group % 16);
      if (tail) result += sum_nibbles(classes_[word] & ((std::uint64_t{1} << (4 * tail)) - 1));
      return result;
    }

  private:
    static unsigned sum_nibbles(std::uint64_t value) noexcept {
      value = (value & 0x0f0f0f0f0f0f0f0full) + ((value >> 4) & 0x0f0f0f0f0f0f0f0full);
      value = (value & 0x00ff00ff00ff00ffull) + ((value >> 8) & 0x00ff00ff00ff00ffull);
      value = (value & 0x0000ffff0000ffffull) + ((value >> 16) & 0x0000ffff0000ffffull);
      return unsigned((value & 0xffffffffu) + (value >> 32));
    }

    std::span<std::uint64_t const> classes_;
    std::span<std::uint64_t const> checkpoints_;
    std::uint64_t virtual_count_;
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
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-Everett-All-Rights-Reserved
 * \endlicense
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Everett's rank15 support.
 */
