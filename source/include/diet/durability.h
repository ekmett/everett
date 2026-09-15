/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace diet {
  // Protocol events supplied by a backend, not wrappers around fsync's return
  // value. durable_verified includes content verification and all required
  // data, allocator, naming and recovery-root persistence for that operation.
  enum struct persistence_result { durable_verified, failure };
  enum struct recovery_evidence { unverified, durable_state_verified };
  // old_root_selected asserts that recovery durably selected the old root
  // AND reconciled candidate reachability while retaining uncertain objects.
  // It never grants reclamation of a failed output generation.
  enum struct selector_recovery { unresolved, old_root_selected };
  enum struct publication_stage { building, output_durable, manifest_pending, uncertain, published };

  struct merge_identity {
    // Recipe includes comparator/codec versions, precedence and any context
    // used to elide tombstones. Input order is semantically significant.
    std::string recipe;
    std::vector<std::string> input_versions;
    bool operator==(merge_identity const &) const = default;
  };

  struct merge_input_cursor {
    std::string input_version;
    std::uint64_t ordinal = 0;
    std::uint64_t byte_offset = 0;
    // This first checkpoint codec pauses at record boundaries and saves the
    // whole predecessor, so ordinary front coding can restart independently.
    std::string predecessor_key;
    bool operator==(merge_input_cursor const &) const = default;
  };

  struct sealed_merge_extent {
    std::string exact_id;
    std::uint64_t bytes = 0;
    // Opaque identity of a byte-integrity digest, separate from the algebraic
    // cola signature. The backend verifies the named bytes and their length.
    std::string checksum;
    bool operator==(sealed_merge_extent const &) const = default;
  };

  struct merge_checkpoint {
    std::string exact_id;
    merge_identity identity;
    std::string output_generation;
    std::vector<merge_input_cursor> inputs;
    std::vector<sealed_merge_extent> sealed_output;
    std::string previous_output_key;
    // Versioned serialized continuation of rank and Elias–Fano builders, partial
    // group counts/offsets, merge selection and accumulator state. This model
    // treats the bytes as opaque; the eventual codec validates them on load.
    std::string metadata_state;
    std::uint64_t output_records = 0;
    std::uint64_t completed_work = 0;
    bool operator==(merge_checkpoint const &) const = default;

    std::uint64_t sealed_bytes() const {
      std::uint64_t sum = 0;
      for (auto const & extent : sealed_output) {
        if (extent.bytes > std::numeric_limits<std::uint64_t>::max() - sum)
          throw std::overflow_error("merge checkpoint size");
        sum += extent.bytes;
      }
      return sum;
    }
  };

  // Executable protocol model only: no files, flushing, checksumming, durable
  // pin implementation, or thread synchronization occurs here. A backend
  // owns those operations and supplies truthful completion/recovery events.
  // One serialized publication owner uses the state machine for one recipe.
  struct merge_publication {
    merge_publication(merge_identity identity, std::string old_manifest,
                      std::string output_generation)
      : identity_(std::move(identity)), durable_manifest_(std::move(old_manifest)),
        generation_(std::move(output_generation)) {
      if (identity_.recipe.empty() || identity_.input_versions.empty() ||
          durable_manifest_.empty() || generation_.empty())
        throw std::invalid_argument("missing merge publication identity");
      for (std::size_t i = 0; i < identity_.input_versions.size(); ++i) {
        auto const & id = identity_.input_versions[i];
        if (id.empty()) throw std::invalid_argument("missing merge input identity");
        for (std::size_t j = 0; j < i; ++j)
          if (identity_.input_versions[j] == id)
            throw std::invalid_argument("duplicate merge input identity");
      }
      generations_.push_back(generation_);
    }

    publication_stage stage() const noexcept { return stage_; }
    std::string const & durable_manifest() const noexcept { return durable_manifest_; }
    std::string const & output_generation() const noexcept { return generation_; }
    std::string const & pending_manifest() const noexcept { return pending_manifest_; }
    std::optional<merge_checkpoint> const & checkpoint() const noexcept { return checkpoint_; }
    bool old_pins_retained() const noexcept { return old_pins_retained_; }
    bool can_release_old_pins() const noexcept { return stage_ == publication_stage::published; }

    // A failed checkpoint write never supersedes the previous durable one.
    void complete_checkpoint(merge_checkpoint candidate, persistence_result result) {
      require_stage(publication_stage::building);
      validate_checkpoint(candidate);
      if (candidate.output_generation != generation_)
        throw std::invalid_argument("checkpoint generation mismatch");
      if (checkpoint_) {
        if (candidate.exact_id == checkpoint_->exact_id ||
            candidate.output_records < checkpoint_->output_records ||
            candidate.completed_work < checkpoint_->completed_work)
          throw std::invalid_argument("checkpoint did not advance");
        for (std::size_t i = 0; i < candidate.inputs.size(); ++i)
          if (candidate.inputs[i].ordinal < checkpoint_->inputs[i].ordinal ||
              candidate.inputs[i].byte_offset < checkpoint_->inputs[i].byte_offset)
            throw std::invalid_argument("checkpoint cursor moved backwards");
      }
      if (result == persistence_result::failure) { stage_ = publication_stage::uncertain; return; }
      checkpoint_ = std::move(candidate);
    }

    void complete_output(persistence_result result) {
      require_stage(publication_stage::building);
      stage_ = result == persistence_result::durable_verified ?
               publication_stage::output_durable : publication_stage::uncertain;
    }

    void prepare_manifest(std::string exact_id) {
      require_stage(publication_stage::output_durable);
      if (exact_id.empty() || exact_id == durable_manifest_ ||
          std::find(manifests_.begin(), manifests_.end(), exact_id) != manifests_.end())
        throw std::invalid_argument("manifest identity must be fresh");
      manifests_.push_back(exact_id);
      pending_manifest_ = std::move(exact_id);
      stage_ = publication_stage::manifest_pending;
    }

    void complete_manifest(persistence_result result) {
      require_stage(publication_stage::manifest_pending);
      if (result == persistence_result::failure) { stage_ = publication_stage::uncertain; return; }
      durable_manifest_ = pending_manifest_;
      stage_ = publication_stage::published;
    }

    // Includes write, flush, verification, directory or allocator failures
    // before publication. A transient-looking error does not relax retention.
    void report_io_failure() {
      if (stage_ == publication_stage::published)
        throw std::logic_error("published merge requires separate cleanup handling");
      stage_ = publication_stage::uncertain;
    }

    // Fresh output storage is mandatory. Optional retained checkpoint must be
    // the exact last successful checkpoint and must have been independently
    // revalidated, together with its inputs and every sealed extent, by the
    // backend. Without it, regenerate from the original pinned inputs. If a
    // manifest was attempted, old-root selection must additionally be made
    // durable and all candidate reachability reconciled before restarting.
    void resume(std::string fresh_generation, recovery_evidence evidence,
                std::optional<merge_checkpoint> retained = std::nullopt,
                selector_recovery selector = selector_recovery::unresolved) {
      require_stage(publication_stage::uncertain);
      require_evidence(evidence);
      if (!pending_manifest_.empty() && selector != selector_recovery::old_root_selected)
        throw std::invalid_argument("uncertain manifest selector needs reconciliation");
      if (fresh_generation.empty() ||
          std::find(generations_.begin(), generations_.end(), fresh_generation) != generations_.end())
        throw std::invalid_argument("merge resumption needs a fresh generation");
      if (retained && (!checkpoint_ || *retained != *checkpoint_))
        throw std::invalid_argument("unrecognized recovery checkpoint");
      generations_.push_back(fresh_generation);
      generation_ = std::move(fresh_generation);
      checkpoint_ = std::move(retained);
      pending_manifest_.clear();
      stage_ = publication_stage::building;
    }

    // A failed manifest flush has an uncertain outcome, not a guaranteed
    // rollback. Accept it only when recovery independently verifies the
    // candidate root AND its complete immutable dependency graph as durable.
    void accept_recovered_publication(recovery_evidence evidence) {
      require_stage(publication_stage::uncertain);
      require_evidence(evidence);
      if (pending_manifest_.empty()) throw std::logic_error("no manifest to reconcile");
      durable_manifest_ = pending_manifest_;
      stage_ = publication_stage::published;
    }

    // This only grants/releases the model's merge-owned durable retention.
    // Other snapshots, readers, checkpoints and index dependencies keep pins.
    // The backend persists retirement before reclaiming bytes or extents.
    bool release_old_pins() {
      require_stage(publication_stage::published);
      return std::exchange(old_pins_retained_, false);
    }

  private:
    void require_stage(publication_stage wanted) const {
      if (stage_ != wanted) throw std::logic_error("invalid merge publication transition");
    }
    static void require_evidence(recovery_evidence evidence) {
      if (evidence != recovery_evidence::durable_state_verified)
        throw std::invalid_argument("durable recovery evidence required");
    }
    void validate_checkpoint(merge_checkpoint const & candidate) const {
      if (candidate.exact_id.empty() || candidate.identity != identity_ ||
          candidate.inputs.size() != identity_.input_versions.size())
        throw std::invalid_argument("checkpoint identity mismatch");
      for (std::size_t i = 0; i < candidate.inputs.size(); ++i)
        if (candidate.inputs[i].input_version != identity_.input_versions[i])
          throw std::invalid_argument("checkpoint input version mismatch");
      for (std::size_t i = 0; i < candidate.sealed_output.size(); ++i) {
        auto const & extent = candidate.sealed_output[i];
        if (extent.exact_id.empty() || extent.checksum.empty())
          throw std::invalid_argument("checkpoint extent identity missing");
        for (std::size_t j = 0; j < i; ++j)
          if (candidate.sealed_output[j].exact_id == extent.exact_id)
            throw std::invalid_argument("duplicate checkpoint extent");
      }
      (void)candidate.sealed_bytes();
    }

    merge_identity identity_;
    std::string durable_manifest_;
    std::string generation_;
    std::string pending_manifest_;
    std::vector<std::string> generations_;
    std::vector<std::string> manifests_;
    std::optional<merge_checkpoint> checkpoint_;
    publication_stage stage_ = publication_stage::building;
    bool old_pins_retained_ = true;
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Diet's durability support.
 */
