/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Merges sort-owned native records directly into a streamed immutable file.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <diet/sort_profile_file_writer.h>
#include <diet/sort_profile_merge.h>

namespace diet {
  namespace sort_profile_detail {
    template <class P, class Selector, class Ops> struct file_output_ref {
      sort_profile_file_writer<P, Selector, Ops> * output;
      bool failed() const noexcept { return output->failed(); }
      void append_frame(sort_profile_frame const & frame, std::span<bit_view const> spans, std::uint64_t common, bit_view value) {
        output->append_frame(frame, spans, common, value);
      }
      auto finish() { return output->finish(); }
    };
  }
  template <class P, class Native = sort_profile_array<P>, class Compose = replace_native_value,
            class Selector = typename Native::stream_family::selector_type, class Ops = posix_object_ops>
  struct sort_profile_file_merge {
    using source_pointer = std::shared_ptr<Native const>;
    using output_type = sort_profile_file_writer<P, Selector, Ops>;
    using builder_type = sort_profile_merge_builder<P, Native, Compose, Selector, sort_profile_detail::file_output_ref<P, Selector, Ops>>;
    sort_profile_file_merge(std::filesystem::path root, object_id id, object_attempt_id attempt,
        source_pointer older, source_pointer newer, Compose compose = {})
      : output_(std::move(root), std::move(id), std::move(attempt)),
        builder_({&output_}, std::move(older), std::move(newer), std::move(compose)) {}
    sort_profile_file_merge(std::filesystem::path root, object_id id, object_attempt_id attempt,
        source_pointer older, source_pointer newer, Ops & ops, Compose compose = {})
      : output_(std::move(root), std::move(id), std::move(attempt), ops),
        builder_({&output_}, std::move(older), std::move(newer), std::move(compose)) {}
    sort_profile_file_merge(sort_profile_file_merge const &) = delete;
    sort_profile_file_merge & operator=(sort_profile_file_merge const &) = delete;
    sort_profile_file_merge(sort_profile_file_merge &&) = delete;
    sort_profile_file_merge & operator=(sort_profile_file_merge &&) = delete;
    bool done() const noexcept { return builder_.done(); }
    bool failed() const noexcept { return builder_.failed(); }
    bool finished() const noexcept { return builder_.finished(); }
    native_merge_progress progress() const noexcept { return builder_.progress(); }
    std::uint64_t materialized_keys() const noexcept { return builder_.materialized_keys(); }
    native_merge_progress step(std::uint64_t budget = 1) { return builder_.step(budget); }
    object_seal_receipt finish() { return builder_.finish(); }
    object_write_paths const & paths() const & noexcept { return output_.paths(); }
    object_write_paths const & paths() const && = delete;
  private:
    output_type output_;
    builder_type builder_;
  };
}
