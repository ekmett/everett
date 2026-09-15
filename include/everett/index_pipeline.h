/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/error_detail.h>

#include <everett/index_builder.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace everett {
  // Build indexes from the target outward. Each stage retains one input and
  // one output sample; sample keys flow directly into the next stage without
  // materializing an intermediate catalog or rescanning a completed index.
  // Handoffs carry a policy-unit backspace count and suffix relative to the
  // preceding sample from that same producer, with a literal first sample.
  //
  // A step quantum consumes one augmented occurrence, transfers one sample,
  // advances the source sampler by at most K occurrences, or communicates EOF.
  // This is an entry-work budget, not a bound on bytes or wall-clock time.
  // finish() separately builds the offset/rank directories and binds each
  // completed pair to its exact target. No durable publication is performed.
  template <class P>
  struct index_pipeline {
    using policy_type = P;
    using blob_type = profile_blob<P>;
    using pair_type = std::shared_ptr<blob_type const>;

    // Native stages are ordered nearest the target first, new head last.
    index_pipeline(pair_type target, std::vector<pair_type> native_stages)
      : target_(std::move(target)), source_(target_) {
      stages_.reserve(native_stages.size());
      for (auto const & source : native_stages) {
        if (!source) error_detail::raise<std::invalid_argument>("null pipeline native source");
        stages_.push_back(std::make_unique<index_builder<P>>(*source));
      }
      results_.resize(stages_.size());
      emitted_.resize(stages_.size());
      emitted_suffix_units_.resize(stages_.size());
      consumed_.resize(stages_.size());
    }

    index_pipeline(index_pipeline const &) = delete;
    index_pipeline & operator=(index_pipeline const &) = delete;
    index_pipeline(index_pipeline &&) = default;
    index_pipeline & operator=(index_pipeline &&) = default;

    bool done() const noexcept {
      return stages_.empty() || stages_.back()->done();
    }
    bool finished() const noexcept { return finished_; }
    bool failed() const noexcept { return failed_; }
    std::size_t size() const noexcept { return stages_.size(); }

    std::uint64_t step(std::uint64_t quanta) {
      if (finished_ || failed_) error_detail::raise<std::logic_error>("pipeline is no longer active");
      std::uint64_t work = 0;
      try {
        while (work != quanta && !done()) {
          if (!advance_one()) error_detail::raise<std::logic_error>("index pipeline made no progress");
          ++work;
        }
      } catch (...) {
        failed_ = true;
        throw;
      }
      return work;
    }

    pair_type finish() {
      if (failed_) error_detail::raise<std::logic_error>("index pipeline has failed");
      if (!done()) error_detail::raise<std::logic_error>("index pipeline still has input");
      if (!finished_) {
        try {
          auto target = target_;
          for (std::size_t i = 0; i != stages_.size(); ++i) {
            results_[i] = std::make_shared<blob_type const>(stages_[i]->finish(target));
            target = results_[i];
          }
          finished_ = true;
        } catch (...) {
          failed_ = true;
          throw;
        }
      }
      return results_.empty() ? target_ : results_.back();
    }

    std::span<std::uint64_t const> emitted_samples() const & noexcept { return emitted_; }
    std::span<std::uint64_t const> emitted_samples() const && = delete;
    std::span<std::uint64_t const> consumed_entries() const & noexcept { return consumed_; }
    std::span<std::uint64_t const> consumed_entries() const && = delete;
    std::span<std::uint64_t const> emitted_suffix_units() const & noexcept { return emitted_suffix_units_; }
    std::span<std::uint64_t const> emitted_suffix_units() const && = delete;
    std::uint64_t source_suffix_units() const noexcept { return source_suffix_units_; }
    sampling_work const & source_work() const & noexcept { return source_.counters(); }
    sampling_work const & source_work() const && = delete;

  private:
    pair_type target_;
    sample_cursor<P> source_;
    profile_sample_encoder<P> source_encoder_;
    std::vector<std::unique_ptr<index_builder<P>>> stages_;
    std::vector<pair_type> results_;
    std::vector<std::uint64_t> emitted_;
    std::vector<std::uint64_t> consumed_;
    std::vector<std::uint64_t> emitted_suffix_units_;
    std::uint64_t source_suffix_units_ = 0;
    bool finished_ = false;
    bool failed_ = false;

    bool advance_one() {
      // Give consumers the first opportunity to release backpressure.
      for (std::size_t end = stages_.size(); end; --end) {
        auto i = end - 1;
        auto & stage = *stages_[i];
        if (stage.has_output()) {
          if (i + 1 == stages_.size()) {
            auto sample = stage.take_coded_output();
            emitted_suffix_units_[i] = profile_detail::add(
              emitted_suffix_units_[i], sample.suffix.bit_size / P::bits_per_unit);
          } else {
            auto & next = *stages_[i + 1];
            if (!next.needs_input()) continue;
            auto sample = stage.take_coded_output();
            next.push(sample);
            emitted_suffix_units_[i] = profile_detail::add(
              emitted_suffix_units_[i], sample.suffix.bit_size / P::bits_per_unit);
          }
          ++emitted_[i];
          return true;
        }
        if (stage.done()) continue;
        if (stage.needs_input()) {
          if (!i) {
            if (source_.done()) {
              stage.close_input();
            } else {
              auto sample = source_.peek();
              auto coded = source_encoder_.encode(sample.key, sample.target_ordinal);
              stage.push(coded);
              source_suffix_units_ = profile_detail::add(
                source_suffix_units_, coded.suffix.bit_size / P::bits_per_unit);
              source_.advance();
            }
            return true;
          }
          if (stages_[i - 1]->done()) {
            stage.close_input();
            return true;
          }
          continue;
        }
        auto consumed = stage.step(1);
        if (consumed) {
          consumed_[i] += consumed;
          return true;
        }
      }
      return false;
    }
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Builds fractional-index chains through bounded streaming queues.
 */
