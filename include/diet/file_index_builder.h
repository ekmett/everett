/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Builds fractional indexes over pinned native storage into immutable files.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <diet/index_builder.h>
#include <diet/profile_file_output.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace diet {
  namespace profile_detail {
    template <class P, class Ops> struct file_index_output {
      using policy_type = P;
      file_index_output(std::filesystem::path root, object_id id, object_attempt_id attempt,
          object_id native_id, std::optional<blob_identity> target_id)
        : native_id_(checked(std::move(native_id))), target_id_(checked(std::move(target_id))),
          output_(std::move(root), std::move(id), std::move(attempt)) {}
      file_index_output(std::filesystem::path root, object_id id, object_attempt_id attempt,
          object_id native_id, std::optional<blob_identity> target_id, Ops & ops)
        : native_id_(checked(std::move(native_id))), target_id_(checked(std::move(target_id))),
          output_(std::move(root), std::move(id), std::move(attempt), 0, ops) {}
      std::uint64_t size() const noexcept { return output_.size(); }
      bool finished() const noexcept { return output_.finished(); }
      bool failed() const noexcept { return output_.failed(); }
      object_write_paths const & paths() const & noexcept { return output_.paths(); }
      object_write_paths const & paths() const && = delete;
      void append_known(bit_view key, std::uint64_t common_bits) {
        auto retained = common_bits >> P::unit_shift;
        auto first = retained << P::unit_shift;
        output_.append(retained, key.subview(first, key.size() - first), {});
      }
      object_seal_receipt finish(index_metadata<P> const & metadata) {
        index_file_sections<P> sections{native_id_, target_id_, metadata.interleave,
          metadata.false_borrows, metadata.cut_lcps, metadata.virtual_count};
        return output_.finish(sections);
      }
    private:
      static object_id checked(object_id id) {
        if (id.hex().size() != 32)
          error_detail::raise<std::invalid_argument>("file index requires valid dependency identities");
        return id;
      }
      static std::optional<blob_identity> checked(std::optional<blob_identity> target) {
        if (target && (target->native.hex().size() != 32 || target->index.hex().size() != 32))
          error_detail::raise<std::invalid_argument>("file index requires valid target identities");
        return target;
      }
      object_id native_id_;
      std::optional<blob_identity> target_id_;
      profile_file_output<P, stream_role::borrowed, Ops> output_;
    };
    template <class P, class Ops> struct file_index_output_ref {
      using policy_type = P;
      file_index_output<P, Ops> * output;
      std::uint64_t size() const noexcept { return output->size(); }
      bool finished() const noexcept { return output->finished(); }
      bool failed() const noexcept { return output->failed(); }
      void append_known(bit_view key, std::uint64_t common_bits) { output->append_known(key, common_bits); }
      object_seal_receipt finish(index_metadata<P> const & metadata) { return output->finish(metadata); }
    };
  }

  // The ordinary index builder with a bounded borrowed-FC file sink. Native
  // storage is pinned, not copied. The caller supplies trusted samples and
  // exact native/target identities, retaining those dependencies through sealing
  // and publication. Counts do not authenticate sample contents or identities.
  // Each step budgets occurrences; key bytes and final navigation construction
  // remain additional work. Only finish seals a complete IX02 object: there is
  // no durable partial-output checkpoint or resumed-job API. An external Ops
  // object supplied by reference must outlive the builder.
  template <class P, class Native = profile_array<P>, class Ops = posix_object_ops>
  struct file_index_builder {
    using policy_type = P;
    using source_pointer = std::shared_ptr<Native const>;
    using builder_type = index_builder<P, Native, profile_detail::file_index_output_ref<P, Ops>>;
    using sample_type = profile_sample<P>;
    using coded_sample_type = profile_coded_sample<P>;
    file_index_builder(std::filesystem::path root, object_id id, object_attempt_id attempt,
        source_pointer native, object_id native_id, std::optional<blob_identity> target_id = std::nullopt)
      : output_(std::move(root), std::move(id), std::move(attempt), std::move(native_id), std::move(target_id)),
        builder_({&output_}, std::move(native)) {}
    file_index_builder(std::filesystem::path root, object_id id, object_attempt_id attempt,
        source_pointer native, object_id native_id, std::optional<blob_identity> target_id, Ops & ops)
      : output_(std::move(root), std::move(id), std::move(attempt), std::move(native_id), std::move(target_id), ops),
        builder_({&output_}, std::move(native)) {}
    file_index_builder(file_index_builder const &) = delete;
    file_index_builder & operator=(file_index_builder const &) = delete;
    file_index_builder(file_index_builder &&) = delete;
    file_index_builder & operator=(file_index_builder &&) = delete;
    bool needs_input() const noexcept { return builder_.needs_input(); }
    bool has_output() const noexcept { return builder_.has_output(); }
    bool done() const noexcept { return builder_.done(); }
    bool failed() const noexcept { return builder_.failed(); }
    bool finished() const noexcept { return builder_.finished(); }
    std::uint64_t size() const noexcept { return builder_.size(); }
    std::uint64_t received_samples() const noexcept { return builder_.received_samples(); }
    void push(bit_view key, std::uint64_t target_ordinal) { builder_.push(key, target_ordinal); }
    void push(coded_sample_type const & sample) { builder_.push(sample); }
    void close_input() { builder_.close_input(); }
    std::uint64_t step(std::uint64_t budget_entries) { return builder_.step(budget_entries); }
    sample_type take_output() { return builder_.take_output(); }
    coded_sample_type take_coded_output() { return builder_.take_coded_output(); }
    object_seal_receipt finish(std::uint64_t target_count) { return builder_.finish_index(target_count); }
    object_write_paths const & paths() const & noexcept { return output_.paths(); }
    object_write_paths const & paths() const && = delete;
  private:
    profile_detail::file_index_output<P, Ops> output_;
    builder_type builder_;
  };
}
