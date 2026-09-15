/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/profile.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace everett {
  // Incremental ordinary-FC native output. A common value width, if any, is
  // fixed before the first record. Otherwise every value carries its length.
  // Keeps encoded output, one residual offset per physical block, and the last
  // key; it does not stage a vector of full records. Output remains in memory.
  template <class P> struct profile_native_writer {
    using policy_type = P;
    static constexpr stream_role role = stream_role::native;

    explicit profile_native_writer(std::optional<std::uint64_t> common_value_width = P::value_width)
      : common_(common_value_width) {
      if constexpr (P::fixed_width)
        if (common_ != P::value_width) throw std::invalid_argument("native writer width disagrees with fixed policy");
      if (common_) (void)profile_detail::multiply(*common_, P::bits_per_unit);
    }
    profile_native_writer(profile_native_writer const &) = delete;
    profile_native_writer & operator=(profile_native_writer const &) = delete;
    profile_native_writer(profile_native_writer && other) noexcept
      : data_(std::move(other.data_)), previous_(std::move(other.previous_)),
        offsets_(std::move(other.offsets_)), common_(other.common_),
        count_(std::exchange(other.count_, 0)), finished_(std::exchange(other.finished_, true)) {}
    profile_native_writer & operator=(profile_native_writer && other) noexcept {
      if (this != &other) {
        data_ = std::move(other.data_); previous_ = std::move(other.previous_);
        offsets_ = std::move(other.offsets_); common_ = other.common_;
        count_ = std::exchange(other.count_, 0); finished_ = std::exchange(other.finished_, true);
      }
      return *this;
    }

    std::uint64_t size() const noexcept { return count_; }
    bool finished() const noexcept { return finished_; }
    std::optional<std::uint64_t> common_value_width() const noexcept { return common_; }

    // Keys must be strictly increasing. Input views need only survive this
    // call. A rejected append leaves the preceding committed records intact.
    void append(bit_view key, bit_view value) {
      require_active();
      if (key.size() % P::bits_per_unit || value.size() % P::bits_per_unit)
        throw std::invalid_argument("native writer record length disagrees with policy units");
      auto value_units = value.size() / P::bits_per_unit;
      if (common_ && value_units != *common_)
        throw std::invalid_argument("native writer value disagrees with common width");
      auto comparison = compare_common_bits(previous_.view(), key);
      if (count_ && comparison.order >= 0)
        throw std::invalid_argument("native writer keys must be strictly increasing");
      auto next_count = profile_detail::add(count_, 1);
      auto retained = comparison.common_bits / P::bits_per_unit;
      auto previous_units = previous_.bit_size / P::bits_per_unit;
      auto key_units = key.size() / P::bits_per_unit;
      auto saved_bits = data_.bit_size;
      auto saved_offsets = offsets_.size();
      auto next_previous = bit_string::copy(key);
      try {
        if (count_ % P::codec_block_size == 0) {
          auto stride = profile_detail::multiply(count_, common_.value_or(0));
          offsets_.push_back(data_.bit_size / P::bits_per_unit - stride);
          profile_detail::write_count<P>(data_, previous_units);
        }
        profile_detail::write_backspace<P>(data_, previous_units - retained);
        profile_detail::write_count<P>(data_, key_units - retained);
        if (!common_) profile_detail::write_count<P>(data_, value_units);
        auto retained_bits = profile_detail::multiply(retained, P::bits_per_unit);
        profile_detail::append(data_, key.subview(retained_bits, key.size() - retained_bits));
        profile_detail::append(data_, value);
      } catch (...) {
        profile_detail::resize(data_, saved_bits);
        offsets_.resize(saved_offsets);
        throw;
      }
      previous_ = std::move(next_previous);
      count_ = next_count;
    }
    void append(profile_record const & record) { append(record.key.view(), record.value.view()); }

    // Finalizing Elias–Fano visits the staged block offsets. This operation
    // is neither byte-budgeted nor a durable checkpoint.
    profile_array<P> finish() {
      require_active();
      profile_array<P> result;
      auto extent = data_.bit_size / P::bits_per_unit;
      auto stride = profile_detail::multiply(count_, common_.value_or(0));
      offsets_.push_back(extent - stride);
      try {
        result.offsets_ = select_groups<P::codec_block_size>::build(offsets_, count_);
      } catch (...) {
        offsets_.pop_back();
        throw;
      }
      result.metadata_.common_value_width = common_;
      result.metadata_.record_count = count_;
      result.metadata_.extent = extent;
      result.metadata_.terminal_key_units = previous_.bit_size / P::bits_per_unit;
      result.bytes_ = std::move(data_.bytes);
      data_.bit_size = 0;
      previous_ = {};
      offsets_ = {};
      finished_ = true;
      return result;
    }

  private:
    void require_active() const {
      if (finished_) throw std::logic_error("native profile writer is finished");
    }
    bit_string data_;
    bit_string previous_;
    std::vector<std::uint64_t> offsets_;
    std::optional<std::uint64_t> common_;
    std::uint64_t count_ = 0;
    bool finished_ = false;
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Builds native front-coded profiles incrementally with explicit value widths.
 */
