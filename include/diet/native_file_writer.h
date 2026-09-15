/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <diet/profile_file_output.h>

namespace diet {
  namespace profile_detail {
    template <class P, class Ops = posix_object_ops>
    using native_file_output = profile_file_output<P, stream_role::native, Ops>;
  }

  // Checked incremental native output to a private immutable-object attempt.
  // Retains only the preceding full key, bounded FC/control scratch, and block
  // offsets; values are consumed synchronously. Invalid input is retryable.
  // Output I/O failure poisons this attempt and preserves surviving names.
  // Finalization builds EF and seals the existing portable native container;
  // this is not a durable partial-output checkpoint or a resumable job.
  template <class P, class Ops = posix_object_ops> struct native_file_writer {
    using policy_type = P;
    native_file_writer(std::filesystem::path root, object_id id, object_attempt_id attempt,
        std::optional<std::uint64_t> common = P::value_width)
      : output_(std::move(root), std::move(id), std::move(attempt), common) {}
    native_file_writer(std::filesystem::path root, object_id id, object_attempt_id attempt,
        std::optional<std::uint64_t> common, Ops & ops)
      : output_(std::move(root), std::move(id), std::move(attempt), common, ops) {}
    native_file_writer(native_file_writer const &) = delete;
    native_file_writer & operator=(native_file_writer const &) = delete;
    native_file_writer(native_file_writer &&) = delete;
    native_file_writer & operator=(native_file_writer &&) = delete;
    std::uint64_t size() const noexcept { return output_.size(); }
    bool failed() const noexcept { return output_.failed(); }
    bool finished() const noexcept { return output_.finished(); }
    std::optional<std::uint64_t> common_value_width() const noexcept { return output_.common_value_width(); }
    object_write_paths const & paths() const & noexcept { return output_.paths(); }
    object_write_paths const & paths() const && = delete;

    void append(bit_view key, bit_view value) {
      output_.require_active();
      if ((key.size() & (P::bits_per_unit - 1)) || (value.size() & (P::bits_per_unit - 1)))
        error_detail::raise<std::invalid_argument>("native file record disagrees with policy units");
      auto common = output_.common_value_width();
      if (common && (value.size() >> P::unit_shift) != *common)
        error_detail::raise<std::invalid_argument>("native file value disagrees with common width");
      auto comparison = compare_common_bits(previous_.view(), key);
      if (size() && comparison.order >= 0)
        error_detail::raise<std::invalid_argument>("native file keys must be strictly increasing");
      auto retained = comparison.common_bits >> P::unit_shift;
      auto retained_bits = retained << P::unit_shift;
      if (key.size() > std::numeric_limits<std::uint64_t>::max() - 7)
        error_detail::raise<std::length_error>("native file key is too large");
      auto bytes = profile_detail::byte_count(key.size());
      if (bytes > previous_.bytes.max_size())
        error_detail::raise<std::length_error>("native file key is too large");
      if (bytes > previous_.bytes.capacity()) {
        auto capacity = previous_.bytes.capacity(), maximum = previous_.bytes.max_size();
        auto grown = capacity > maximum / 2 ? maximum : 2 * capacity;
        previous_.bytes.reserve(std::max(static_cast<std::size_t>(bytes), grown));
      }
      auto literal = key.subview(retained_bits, key.size() - retained_bits);
      output_.append(retained, literal, value);
      profile_detail::resize(previous_, key.size());
      profile_detail::copy_into(previous_, retained_bits, literal);
    }
    void append(profile_record const & record) { append(record.key.view(), record.value.view()); }
    object_seal_receipt finish() {
      auto receipt = output_.finish(); previous_ = {}; return receipt;
    }
  private:
    profile_detail::native_file_output<P, Ops> output_;
    bit_string previous_;
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Streams ordinary front-coded native objects with bounded payload buffering.
 */
