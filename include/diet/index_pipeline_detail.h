/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Shares bounded front-coded sample handoff between index pipelines.
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <diet/error_detail.h>

#include <diet/index_builder.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace diet::index_detail {
  // Build indexes from the target outward. Each stage retains one input and
  // one output sample; sample keys flow directly into the next stage without
  // materializing an intermediate catalog or rescanning a completed index.
  // Handoffs carry a policy-unit backspace count and suffix relative to the
  // preceding sample from that same producer, with a literal first sample.
  //
  // A step quantum consumes one augmented occurrence, transfers one sample,
  // advances the source sampler by at most K occurrences, or communicates EOF.
  // This is an entry-work budget, not a bound on bytes or wall-clock time.
  // Each owner separately finalizes its stages and binds exact target identities.
  template <class P, class Target, class Stage>
  struct pipeline_driver {
    using policy_type = P;
    using blob_type = Target;
    using pair_type = std::shared_ptr<blob_type const>;

    // Stages are stable owners, ordered nearest the source first.
    pipeline_driver(pair_type target, std::vector<std::unique_ptr<Stage>> stages)
      : target_(std::move(target)), source_(target_), stages_(std::move(stages)) {
      emitted_.resize(stages_.size());
      emitted_suffix_units_.resize(stages_.size());
      consumed_.resize(stages_.size());
    }

    pipeline_driver(pipeline_driver const &) = delete;
    pipeline_driver & operator=(pipeline_driver const &) = delete;
    pipeline_driver(pipeline_driver &&) = default;
    pipeline_driver & operator=(pipeline_driver &&) = default;

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

    std::span<std::uint64_t const> emitted_samples() const & noexcept { return emitted_; }
    std::span<std::uint64_t const> emitted_samples() const && = delete;
    std::span<std::uint64_t const> consumed_entries() const & noexcept { return consumed_; }
    std::span<std::uint64_t const> consumed_entries() const && = delete;
    std::span<std::uint64_t const> emitted_suffix_units() const & noexcept { return emitted_suffix_units_; }
    std::span<std::uint64_t const> emitted_suffix_units() const && = delete;
    std::uint64_t source_suffix_units() const noexcept { return source_suffix_units_; }
    sampling_work const & source_work() const & noexcept { return source_.counters(); }
    sampling_work const & source_work() const && = delete;

  protected:
    pair_type target_;
    sample_cursor<P, Target> source_;
    profile_sample_encoder<P> source_encoder_;
    std::vector<std::unique_ptr<Stage>> stages_;
    std::vector<std::uint64_t> emitted_;
    std::vector<std::uint64_t> consumed_;
    std::vector<std::uint64_t> emitted_suffix_units_;
    std::uint64_t source_suffix_units_ = 0;
    bool finished_ = false;
    bool failed_ = false;

  private:
    bool advance_one() {
      // Give consumers the first opportunity to release backpressure.
      for (std::size_t end = stages_.size(); end; --end) {
        auto i = end - 1;
        auto & stage = *stages_[i];
        if (stage.has_output()) {
          if (i + 1 == stages_.size()) {
            auto sample = stage.take_coded_output();
            emitted_suffix_units_[i] = profile_detail::add(
              emitted_suffix_units_[i], (sample.suffix.bit_size >> P::unit_shift));
          } else {
            auto & next = *stages_[i + 1];
            if (!next.needs_input()) continue;
            auto sample = stage.take_coded_output();
            next.push(sample);
            emitted_suffix_units_[i] = profile_detail::add(
              emitted_suffix_units_[i], (sample.suffix.bit_size >> P::unit_shift));
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
                source_suffix_units_, (coded.suffix.bit_size >> P::unit_shift));
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
