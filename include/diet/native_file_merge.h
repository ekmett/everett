/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Merges pinned native inputs into a streamed immutable object.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <diet/native_file_writer.h>
#include <diet/native_merge.h>

namespace diet {
  namespace profile_detail {
    template <class P, class Ops> struct native_file_output_ref {
      using policy_type = P;
      native_file_output<P, Ops> * output;
      std::uint64_t size() const noexcept { return output->size(); }
      bool finished() const noexcept { return output->finished(); }
      bool failed() const noexcept { return output->failed(); }
      auto common_value_width() const noexcept { return output->common_value_width(); }
      void append(std::uint64_t retained, bit_view literal, bit_view value) { output->append(retained, literal, value); }
      object_seal_receipt finish() { return output->finish(); }
    };
  }

  // Owns a streaming output and the ordinary incremental merge frontier.
  // Sources stay pinned through pauses and failures. The default/value-only
  // composition path forwards encoded literals without a third full-key buffer.
  // One step is measured in distinct keys, not bytes or durable progress.
  // Only finish seals the output; interrupted attempts cannot be resumed.
  template <class P, class Native = profile_array<P>, class Compose = replace_native_value,
            class Ops = posix_object_ops> struct native_file_merge {
    using policy_type = P;
    using source_pointer = std::shared_ptr<Native const>;
    using builder_type = native_merge_builder<P, Native, Compose, profile_detail::native_file_output_ref<P, Ops>>;
    native_file_merge(std::filesystem::path root, object_id id, object_attempt_id attempt,
        source_pointer older, source_pointer newer, Compose compose = {},
        std::optional<std::uint64_t> common = P::value_width)
      : output_(std::move(root), std::move(id), std::move(attempt), common),
        builder_({&output_}, std::move(older), std::move(newer), std::move(compose)) {}
    native_file_merge(std::filesystem::path root, object_id id, object_attempt_id attempt,
        source_pointer older, source_pointer newer, Ops & ops, Compose compose = {},
        std::optional<std::uint64_t> common = P::value_width)
      : output_(std::move(root), std::move(id), std::move(attempt), common, ops),
        builder_({&output_}, std::move(older), std::move(newer), std::move(compose)) {}
    native_file_merge(native_file_merge const &) = delete;
    native_file_merge & operator=(native_file_merge const &) = delete;
    native_file_merge(native_file_merge &&) = delete;
    native_file_merge & operator=(native_file_merge &&) = delete;
    bool done() const noexcept { return builder_.done(); }
    bool failed() const noexcept { return builder_.failed(); }
    bool finished() const noexcept { return builder_.finished(); }
    native_merge_progress progress() const noexcept { return builder_.progress(); }
    native_merge_progress step(std::uint64_t key_budget = 1) { return builder_.step(key_budget); }
    object_seal_receipt finish() { return builder_.finish(); }
    object_write_paths const & paths() const & noexcept { return output_.paths(); }
    object_write_paths const & paths() const && = delete;
  private:
    profile_detail::native_file_output<P, Ops> output_;
    builder_type builder_;
  };
}
