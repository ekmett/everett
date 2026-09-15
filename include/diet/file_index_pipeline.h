/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Streams complete fractional-index chains over pinned mapped inputs.
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <diet/file_index_builder.h>
#include <diet/index_pipeline_detail.h>
#include <diet/mapped_blob.h>

#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace diet {
  // A reserved new index over an existing immutable native object. The caller
  // authenticates that source belongs to identity.native, as for mapped_blob.
  template <class P> struct file_index_stage {
    std::shared_ptr<mapped_native<P> const> source;
    blob_identity identity;
    object_attempt_id attempt;
  };

  // Stages are ordered nearest the existing target first, new head last.
  // Sample the exact mapped target once and pass front-coded samples directly
  // between stages. All native objects are retained unchanged. Each stage has
  // bounded payload buffering, current key contexts and compact navigation.
  //
  // Construction opens private attempts under caller-reserved identities.
  // step budgets the same entry events as index_pipeline. After it completes,
  // seal_next finalizes one stage, target outward. It does not publish a catalog
  // root: record each receipt and register the dependencies before publication.
  // Failures preserve earlier receipts and surviving names for reconciliation.
  // A supplied Ops owner must outlive this nonmovable pipeline.
  template <class P, class Ops = posix_object_ops>
  struct file_index_pipeline
      : index_detail::pipeline_driver<P, mapped_blob<P>, file_index_builder<P, mapped_native<P>, Ops>> {
    using policy_type = P;
    using blob_type = mapped_blob<P>;
    using pair_type = std::shared_ptr<blob_type const>;
    using stage_type = file_index_stage<P>;

    file_index_pipeline(std::filesystem::path root, pair_type target, std::vector<stage_type> stages)
      : base(target, make_stages<false>(root, target, stages, nullptr)),
        identities_(identities(stages)), head_(identities_.empty() ? target->identity() : identities_.back()) {
      receipts_.reserve(stages.size());
    }
    file_index_pipeline(std::filesystem::path root, pair_type target, std::vector<stage_type> stages, Ops & ops)
      : base(target, make_stages<true>(root, target, stages, &ops)),
        identities_(identities(stages)), head_(identities_.empty() ? target->identity() : identities_.back()) {
      receipts_.reserve(stages.size());
    }
    file_index_pipeline(file_index_pipeline const &) = delete;
    file_index_pipeline & operator=(file_index_pipeline const &) = delete;
    file_index_pipeline(file_index_pipeline &&) = delete;
    file_index_pipeline & operator=(file_index_pipeline &&) = delete;

    // Returns true after sealing one object, false when all are already sealed.
    // The returned receipt is appended to completed_receipts before success.
    bool seal_next() {
      if (this->failed_) error_detail::raise<std::logic_error>("file index pipeline has failed");
      if (!this->done()) error_detail::raise<std::logic_error>("file index pipeline still has input");
      auto i = receipts_.size();
      if (i == this->stages_.size()) { this->finished_ = true; return false; }
      try {
        auto count = i ? this->stages_[i - 1]->size() : this->target_->virtual_size();
        receipts_.push_back(this->stages_[i]->finish(count));
        this->finished_ = receipts_.size() == this->stages_.size();
        return true;
      } catch (...) {
        this->failed_ = true;
        throw;
      }
    }

    std::span<object_seal_receipt const> finish() & {
      while (seal_next()) {}
      return receipts_;
    }
    std::span<object_seal_receipt const> finish() && = delete;
    std::span<object_seal_receipt const> completed_receipts() const & noexcept { return receipts_; }
    std::span<object_seal_receipt const> completed_receipts() const && = delete;
    std::span<blob_identity const> stage_identities() const & noexcept { return identities_; }
    std::span<blob_identity const> stage_identities() const && = delete;
    blob_identity const & planned_head() const & noexcept { return head_; }
    blob_identity const & planned_head() const && = delete;
    object_write_paths const & paths(std::size_t stage) const & {
      if (stage >= this->stages_.size())
        error_detail::raise<std::out_of_range>("file index pipeline stage is out of range");
      return this->stages_[stage]->paths();
    }
    object_write_paths const & paths(std::size_t) const && = delete;

  private:
    using builder_type = file_index_builder<P, mapped_native<P>, Ops>;
    using base = index_detail::pipeline_driver<P, blob_type, builder_type>;
    std::vector<blob_identity> identities_;
    blob_identity head_;
    std::vector<object_seal_receipt> receipts_;

    static std::vector<blob_identity> identities(std::span<stage_type const> stages) {
      std::vector<blob_identity> result;
      result.reserve(stages.size());
      for (auto const & stage : stages) result.push_back(stage.identity);
      return result;
    }
    template <bool ExternalOps>
    static std::vector<std::unique_ptr<builder_type>> make_stages(std::filesystem::path const & root,
        pair_type const & target, std::span<stage_type const> stages, Ops * ops) {
      if (!target) error_detail::raise<std::invalid_argument>("null file index pipeline target");
      std::unordered_set<std::string> ids;
      ids.insert(target->identity().index.hex());
      for (auto const & stage : stages) {
        if (!stage.source || stage.identity.native.hex().size() != 32 ||
            stage.identity.index.hex().size() != 32 || stage.attempt.hex().size() != 32)
          error_detail::raise<std::invalid_argument>("invalid file index pipeline stage");
        if (!ids.insert(stage.identity.index.hex()).second)
          error_detail::raise<std::invalid_argument>("file index pipeline reuses an index identity");
      }
      std::vector<std::unique_ptr<builder_type>> result;
      result.reserve(stages.size());
      auto downstream = target->identity();
      for (auto const & stage : stages) {
        if constexpr (ExternalOps) result.push_back(std::make_unique<builder_type>(root, stage.identity.index, stage.attempt,
          stage.source, stage.identity.native, downstream, *ops));
        else result.push_back(std::make_unique<builder_type>(root, stage.identity.index, stage.attempt,
          stage.source, stage.identity.native, downstream));
        downstream = stage.identity;
      }
      return result;
    }
  };
}
