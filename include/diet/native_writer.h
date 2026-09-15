/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Builds native front-coded profiles incrementally with explicit value widths.
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <diet/profile.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace diet {
  namespace profile_detail {
    // Internal ordinary-FC framing shared by the checked public writer and
    // the merge frontier. Retention and strict order are supplied by callers;
    // no full key is stored here, only the preceding key's length.
    template <class P> struct native_output {
      using policy_type = P;
      explicit native_output(std::optional<std::uint64_t> common_value_width)
        : common_(common_value_width) {
        if constexpr (P::fixed_width)
          if (common_ != P::value_width)
            error_detail::raise<std::invalid_argument>("native writer width disagrees with fixed policy");
        if (common_) (void)multiply(*common_, P::bits_per_unit);
      }
      native_output(native_output const &) = delete;
      native_output & operator=(native_output const &) = delete;
      native_output(native_output && other) noexcept
        : data_(std::exchange(other.data_, {})), offsets_(std::move(other.offsets_)), common_(other.common_),
          count_(std::exchange(other.count_, 0)), previous_units_(std::exchange(other.previous_units_, 0)),
          finished_(std::exchange(other.finished_, true)) {}
      native_output & operator=(native_output && other) noexcept {
        if (this != &other) {
          data_ = std::exchange(other.data_, {}); offsets_ = std::move(other.offsets_); common_ = other.common_;
          count_ = std::exchange(other.count_, 0);
          previous_units_ = std::exchange(other.previous_units_, 0);
          finished_ = std::exchange(other.finished_, true);
        }
        return *this;
      }
      std::uint64_t size() const noexcept { return count_; }
      bool finished() const noexcept { return finished_; }
      bool failed() const noexcept { return false; }
      std::optional<std::uint64_t> common_value_width() const noexcept { return common_; }
      void require_active() const {
        if (finished_) error_detail::raise<std::logic_error>("native profile writer is finished");
      }

      // The caller splits a representable full key at its exact retained
      // prefix and proves strict order. Bounds and value framing remain checked.
      // Input spans need only survive this call and must not alias private output.
      void append(std::uint64_t retained, bit_view literal, bit_view value) {
        require_active();
        if ((literal.size() & (P::bits_per_unit - 1)) || (value.size() & (P::bits_per_unit - 1)))
          error_detail::raise<std::invalid_argument>("native writer record length disagrees with policy units");
        auto value_units = value.size() >> P::unit_shift;
        if (common_ && value_units != *common_)
          error_detail::raise<std::invalid_argument>("native writer value disagrees with common width");
        if (retained > previous_units_ || (count_ && literal.empty()))
          error_detail::raise<std::invalid_argument>("native writer invalid known prefix");
        auto literal_units = literal.size() >> P::unit_shift;
        auto key_units = retained + literal_units;
        auto next_count = add(count_, 1);
        auto saved_bits = data_.bit_size;
        auto saved_offsets = offsets_.size();
        try {
          if (count_ % P::codec_block_size == 0) {
            auto stride = multiply(count_, common_.value_or(0));
            offsets_.push_back((data_.bit_size >> P::unit_shift) - stride);
            write_count<P>(data_, retained);
          } else write_backspace<P>(data_, previous_units_ - retained);
          write_count<P>(data_, literal_units);
          if (!common_) write_count<P>(data_, value_units);
          profile_detail::append(data_, literal);
          profile_detail::append(data_, value);
        } catch (...) {
          resize(data_, saved_bits);
          offsets_.resize(saved_offsets);
          throw;
        }
        previous_units_ = key_units;
        count_ = next_count;
      }

      profile_array<P> finish() {
        require_active();
        profile_array<P> result;
        auto extent = data_.bit_size >> P::unit_shift;
        auto stride = multiply(count_, common_.value_or(0));
        offsets_.push_back(extent - stride);
        try {
          result.offsets_ = elias_fano::build(offsets_);
        } catch (...) {
          offsets_.pop_back();
          throw;
        }
        result.metadata_.common_value_width = common_;
        result.metadata_.record_count = count_;
        result.metadata_.extent = extent;
        result.metadata_.terminal_key_units = previous_units_;
        result.bytes_ = std::move(data_.bytes);
        data_.bit_size = 0;
        offsets_ = {};
        finished_ = true;
        return result;
      }

    private:
      bit_string data_;
      std::vector<std::uint64_t> offsets_;
      std::optional<std::uint64_t> common_;
      std::uint64_t count_ = 0;
      std::uint64_t previous_units_ = 0;
      bool finished_ = false;
    };
  }

  // Incremental ordinary-FC native output. A common value width, if any, is
  // fixed before the first record. Otherwise every value carries its length.
  // Keeps encoded output, one residual offset per physical block, and the last
  // key; it does not stage a vector of full records. Output remains in memory.
  template <class P> struct profile_native_writer {
    using policy_type = P;
    static constexpr stream_role role = stream_role::native;

    explicit profile_native_writer(std::optional<std::uint64_t> common_value_width = P::value_width)
      : output_(common_value_width) {}
    profile_native_writer(profile_native_writer const &) = delete;
    profile_native_writer & operator=(profile_native_writer const &) = delete;
    profile_native_writer(profile_native_writer && other) noexcept
      : output_(std::move(other.output_)), previous_(std::exchange(other.previous_, {})) {}
    profile_native_writer & operator=(profile_native_writer && other) noexcept {
      if (this != &other) {
        output_ = std::move(other.output_); previous_ = std::exchange(other.previous_, {});
      }
      return *this;
    }

    std::uint64_t size() const noexcept { return output_.size(); }
    bool finished() const noexcept { return output_.finished(); }
    std::optional<std::uint64_t> common_value_width() const noexcept { return output_.common_value_width(); }

    // Keys must be strictly increasing. Input views need only survive this
    // call. A rejected append leaves the preceding committed records intact.
    void append(bit_view key, bit_view value) {
      output_.require_active();
      if ((key.size() & (P::bits_per_unit - 1)) || (value.size() & (P::bits_per_unit - 1)))
        error_detail::raise<std::invalid_argument>("native writer record length disagrees with policy units");
      auto common = output_.common_value_width();
      if (common && (value.size() >> P::unit_shift) != *common)
        error_detail::raise<std::invalid_argument>("native writer value disagrees with common width");
      auto comparison = compare_common_bits(previous_.view(), key);
      if (output_.size() && comparison.order >= 0)
        error_detail::raise<std::invalid_argument>("native writer keys must be strictly increasing");
      auto retained = comparison.common_bits >> P::unit_shift;
      auto retained_bits = retained * P::bits_per_unit;
      // Reserve before fallible output writes. Once the frame commits, updating
      // the logical predecessor cannot allocate or leave a partial bit string.
      auto next_bytes = profile_detail::byte_count(key.size());
      if (next_bytes > previous_.bytes.max_size())
        error_detail::raise<std::length_error>("profile bit string too large");
      previous_.bytes.reserve(static_cast<std::size_t>(next_bytes));
      auto literal = key.subview(retained_bits, key.size() - retained_bits);
      output_.append(retained, literal, value);
      profile_detail::resize(previous_, key.size());
      profile_detail::copy_into(previous_, retained_bits, literal);
    }
    void append(profile_record const & record) { append(record.key.view(), record.value.view()); }

    // Finalizing Elias–Fano visits the staged block offsets. This operation
    // is neither byte-budgeted nor a durable checkpoint.
    profile_array<P> finish() {
      auto result = output_.finish();
      previous_ = {};
      return result;
    }

  private:
    profile_detail::native_output<P> output_;
    bit_string previous_;
  };
}
