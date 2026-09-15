#pragma once

#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace everett {
  namespace select15_detail {
    inline std::uint64_t add(std::uint64_t a, std::uint64_t b) {
      if (b > std::numeric_limits<std::uint64_t>::max() - a)
        throw std::overflow_error("select15 addition");
      return a + b;
    }
    inline std::uint64_t multiply(std::uint64_t a, std::uint64_t b) {
      if (b && a > std::numeric_limits<std::uint64_t>::max() / b)
        throw std::overflow_error("select15 multiplication");
      return a * b;
    }
    inline std::uint64_t words(std::uint64_t bits) noexcept {
      return bits / 64 + (bits % 64 != 0);
    }
  }

  struct select15_sample {
    std::uint64_t first;
    // UINT64_MAX denotes a dense group. Other values index the exception
    // array, containing every one-bit position of this group of <=256 ones.
    std::uint64_t sparse;
  };

  // Elias--Fano over residual byte offsets at records 0,15,30,..., then an
  // actual-record-count sentinel. Residuals omit the fixed stride per record.
  // Equal residual offsets are valid, including an entirely zero sequence.
  //
  // Select on the high bitvector is bounded: sample every 256 ones; scan at
  // most 4096 bits (65 aligned words) for a dense group, or read an explicit
  // exception position for a sparse group. There is no binary search. The
  // exception array is O(n): disjoint sparse groups each span >4096 high bits,
  // and Elias--Fano's high vector has O(n) bits. This is a practical baseline,
  // not a tuned succinct select implementation.
  // Views check shapes and guard navigation bounds. They do not establish
  // semantic consistency of borrowed metadata: use a builder or validated
  // reader. These native-endian spans are not a portable file-format parser.
  struct select15_view {
    select15_view(std::span<std::uint64_t const> low,
                  std::span<std::uint64_t const> high,
                  std::span<select15_sample const> samples,
                  std::span<std::uint64_t const> sparse,
                  std::uint64_t record_count, std::uint64_t universe,
                  unsigned low_width)
      : low_(low), high_(high), samples_(samples), sparse_(sparse),
        record_count_(record_count), universe_(universe), low_width_(low_width) {
      if (low_width > 63) throw std::invalid_argument("select15 low width");
      auto entries = group_count() + 1;
      auto low_bits = select15_detail::multiply(entries, low_width);
      high_bits_ = select15_detail::add(universe >> low_width, entries);
      if (low.size() != select15_detail::words(low_bits) ||
          high.size() != select15_detail::words(high_bits_) ||
          samples.size() != entries / 256 + (entries % 256 != 0))
        throw std::invalid_argument("invalid select15 spans");
    }

    std::uint64_t size() const noexcept { return record_count_; }
    std::uint64_t group_count() const noexcept {
      return record_count_ / 15 + (record_count_ % 15 != 0);
    }

    std::uint64_t residual(std::uint64_t group) const {
      if (group > group_count()) throw std::out_of_range("select15 group");
      auto position = select_high(group);
      if (position < group) throw std::invalid_argument("invalid select15 high value");
      auto hi = position - group;
      if (hi > (universe_ >> low_width_)) throw std::invalid_argument("select15 high overflow");
      std::uint64_t lo = 0;
      if (low_width_) {
        auto bit = group * low_width_;
        auto word = bit / 64;
        unsigned shift = unsigned(bit % 64);
        lo = low_[word] >> shift;
        if (shift + low_width_ > 64) lo |= low_[word + 1] << (64 - shift);
        lo &= (std::uint64_t{1} << low_width_) - 1;
      }
      auto value = (hi << low_width_) | lo;
      if (value > universe_) throw std::invalid_argument("select15 value exceeds universe");
      return value;
    }

    std::uint64_t offset(std::uint64_t group, std::uint64_t fixed_bytes = 0) const {
      auto value = residual(group);
      auto ordinal = group == group_count() ? record_count_ : group * 15;
      return select15_detail::add(value, select15_detail::multiply(ordinal, fixed_bytes));
    }

  private:
    std::uint64_t select_high(std::uint64_t ordinal) const {
      auto sample = samples_[ordinal / 256];
      unsigned remaining = unsigned(ordinal % 256);
      if (sample.sparse != std::numeric_limits<std::uint64_t>::max()) {
        if (sample.sparse > sparse_.size() || remaining >= sparse_.size() - sample.sparse)
          throw std::invalid_argument("invalid select15 exception");
        auto position = sparse_[sample.sparse + remaining];
        if (position >= high_bits_ || !(high_[position / 64] & (std::uint64_t{1} << (position % 64))))
          throw std::invalid_argument("invalid select15 sparse position");
        return position;
      }
      if (sample.first >= high_bits_) throw std::invalid_argument("invalid select15 sample");
      auto word = sample.first / 64;
      auto value = high_[word] & (~std::uint64_t{0} << (sample.first % 64));
      for (unsigned scanned = 0; scanned < 65 && word < high_.size(); ++scanned, ++word) {
        if (scanned) value = high_[word];
        auto population = unsigned(std::popcount(value));
        if (remaining < population) {
          for (; remaining; --remaining) value &= value - 1;
          auto position = word * 64 + unsigned(std::countr_zero(value));
          if (position >= high_bits_ || position - sample.first >= 4096)
            throw std::invalid_argument("select15 dense span");
          return position;
        }
        remaining -= population;
      }
      throw std::invalid_argument("select15 missing high bit");
    }

    std::span<std::uint64_t const> low_;
    std::span<std::uint64_t const> high_;
    std::span<select15_sample const> samples_;
    std::span<std::uint64_t const> sparse_;
    std::uint64_t record_count_;
    std::uint64_t universe_;
    std::uint64_t high_bits_ = 0;
    unsigned low_width_;
  };

  struct select15_index {
    static select15_index build(std::span<std::uint64_t const> residuals,
                                std::uint64_t records) {
      auto groups = records / 15 + (records % 15 != 0);
      if (residuals.size() != groups + 1)
        throw std::invalid_argument("select15 residual count");
      for (std::size_t i = 1; i < residuals.size(); ++i)
        if (residuals[i] < residuals[i - 1])
          throw std::invalid_argument("select15 nonmonotone offsets");
      select15_index result;
      result.record_count = records;
      result.universe = residuals.back();
      auto quotient = result.universe / residuals.size();
      result.low_width = quotient ? unsigned(std::bit_width(quotient) - 1) : 0;
      auto low_bits = select15_detail::multiply(residuals.size(), result.low_width);
      auto high_bits = select15_detail::add(result.universe >> result.low_width, residuals.size());
      result.low.assign(select15_detail::words(low_bits), 0);
      result.high.assign(select15_detail::words(high_bits), 0);
      result.samples.clear();
      std::uint64_t low_mask = result.low_width ? (std::uint64_t{1} << result.low_width) - 1 : 0;
      for (std::uint64_t i = 0; i < residuals.size(); ++i) {
        auto value = residuals[i];
        if (result.low_width) {
          auto bit = i * result.low_width;
          unsigned shift = unsigned(bit % 64);
          result.low[bit / 64] |= (value & low_mask) << shift;
          if (shift + result.low_width > 64)
            result.low[bit / 64 + 1] |= (value & low_mask) >> (64 - shift);
        }
        auto position = (value >> result.low_width) + i;
        result.high[position / 64] |= std::uint64_t{1} << (position % 64);
      }
      for (std::uint64_t begin = 0; begin < residuals.size(); begin += 256) {
        auto end = residuals.size() - begin < 256 ? residuals.size() : begin + 256;
        auto first = (residuals[begin] >> result.low_width) + begin;
        auto last = (residuals[end - 1] >> result.low_width) + end - 1;
        auto sparse = std::numeric_limits<std::uint64_t>::max();
        if (last - first >= 4096) {
          sparse = result.sparse.size();
          for (auto i = begin; i < end; ++i)
            result.sparse.push_back((residuals[i] >> result.low_width) + i);
        }
        result.samples.push_back({first, sparse});
      }
      return result;
    }

    select15_view view() const & {
      return {low, high, samples, sparse, record_count, universe, low_width};
    }

    select15_view view() const && = delete;

    // Default represents the empty stream's offset-zero sentinel. Sections
    // are native-endian/aligned arrays, not a finalized portable file format.
    std::vector<std::uint64_t> low;
    std::vector<std::uint64_t> high{1};
    std::vector<select15_sample> samples{{0, std::numeric_limits<std::uint64_t>::max()}};
    std::vector<std::uint64_t> sparse;
    std::uint64_t record_count = 0;
    std::uint64_t universe = 0;
    unsigned low_width = 0;
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
 * \brief Declares Everett's select15 support.
 */
