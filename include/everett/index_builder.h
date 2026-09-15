/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/sampling.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace everett {
  // Incremental shared-cut index construction over an unchanged native array.
  // One incoming and one outgoing sample provide explicit backpressure. The
  // source wrapper may expire: native_ retains its actual encoded allocation.
  // step() budgets merged occurrences, not key bytes, allocations or final EF
  // construction. Inputs must come from the exact target's trusted sampler;
  // finish() checks its count but does not rescan it to authenticate every key.
  // Coded handoff queues a backspace count, suffix and target ordinal. Backspace
  // counts P units relative to the preceding emitted sample. Decoder, lookahead,
  // pending-writer and outgoing-encoder contexts still retain full current keys.
  template <class P>
  struct index_builder {
    using policy_type = P;
    using blob_type = profile_blob<P>;
    using sample_type = profile_sample<P>;
    using coded_sample_type = profile_coded_sample<P>;
    static constexpr std::uint64_t group_size = P::group_size;

    explicit index_builder(blob_type const & source)
      : native_(source.native_), native_cursor_(native_->view()) {}

    index_builder(index_builder const &) = delete;
    index_builder & operator=(index_builder const &) = delete;
    index_builder(index_builder &&) = default;
    index_builder & operator=(index_builder &&) = default;

    bool needs_input() const noexcept {
      return !finished_ && !failed_ && !input_closed_ && !incoming_;
    }
    bool has_output() const noexcept { return bool(outgoing_); }
    bool done() const noexcept {
      return !failed_ && input_closed_ && !incoming_ && native_cursor_.done() && !outgoing_;
    }
    bool finished() const noexcept { return finished_; }
    std::uint64_t size() const noexcept { return virtual_count_; }
    std::uint64_t received_samples() const noexcept { return received_; }

    void push(bit_view key, std::uint64_t target_ordinal) {
      check_active();
      if (input_mode_ == input_mode::coded) throw std::logic_error("cannot mix coded and full index inputs");
      push_key(key, target_ordinal);
      input_mode_ = input_mode::full;
    }

    void push(coded_sample_type const & sample) {
      check_active();
      if (input_mode_ == input_mode::full) throw std::logic_error("cannot mix coded and full index inputs");
      check_input_slot(sample.target_ordinal);
      auto key = incoming_decoder_.accept(sample);
      try {
        // The decoder's current key is copied once into the existing lookahead
        // state. Failure after advancing that context makes this stage unusable.
        push_key(key, sample.target_ordinal);
      } catch (...) {
        failed_ = true;
        outgoing_.reset();
        throw;
      }
      input_mode_ = input_mode::coded;
    }

    // EOF can accompany a final queued sample. An empty queue alone never
    // authorizes consuming native keys: a later borrowed key may precede them.
    void close_input() {
      check_active();
      if (input_closed_) throw std::logic_error("index input is already closed");
      input_closed_ = true;
    }

    std::uint64_t step(std::uint64_t budget_entries) {
      check_active();
      std::uint64_t consumed = 0;
      try {
        while (consumed < budget_entries && !outgoing_) {
          if ((!incoming_ && !input_closed_) || (!incoming_ && native_cursor_.done())) break;
          auto take_borrowed = native_cursor_.done();
          if (incoming_ && !native_cursor_.done()) {
            auto order = compare_bits(incoming_->view(), native_cursor_.peek().key.prefix);
            take_borrowed = order < 0;
            if (!order) incoming_false_ = true;
          }
          auto key = take_borrowed ? incoming_->view() : native_cursor_.peek().key.prefix;
          auto boundary = virtual_count_ % group_size == 0;
          if (boundary) {
            outgoing_.emplace(outgoing_encoder_.encode(key, virtual_count_));
            classes_.push_back(0);
            if (pending_) pending_ceiling_ = std::min(pending_ceiling_,
              common_prefix_units<P>(pending_->view(), key));
          }
          if (take_borrowed) {
            flush_pending();
            pending_ = std::move(incoming_);
            incoming_.reset();
            pending_ceiling_ = std::numeric_limits<std::uint64_t>::max();
            pending_false_ = incoming_false_;
            incoming_false_ = false;
            auto ordinal = borrowed_count_++;
            if (ordinal % 8 == 0) false_borrows_.push_back(std::byte{0});
            if (pending_false_) false_borrows_.back() |= std::byte(1u << (ordinal % 8));
            ++classes_.back();
          } else {
            native_cursor_.advance();
          }
          ++virtual_count_;
          ++consumed;
        }
      } catch (...) {
        // Partial encoding after an allocation/codec failure is private and
        // cannot be resumed or published as though the failed step committed.
        failed_ = true;
        outgoing_.reset();
        throw;
      }
      return consumed;
    }

    sample_type take_output() {
      check_active();
      if (!outgoing_) throw std::logic_error("index builder has no outgoing sample");
      // Compatibility path: materialize a full queued key only when requested.
      sample_type result{bit_string::copy(outgoing_encoder_.key()), outgoing_->target_ordinal};
      outgoing_.reset();
      return result;
    }

    // Frames depend on every preceding emitted sample, including any retrieved
    // through the full-key compatibility interface. Consume coded frames in order.
    coded_sample_type take_coded_output() {
      check_active();
      if (!outgoing_) throw std::logic_error("index builder has no outgoing sample");
      auto result = std::move(*outgoing_);
      outgoing_.reset();
      return result;
    }

    // Finalizes encoded offset/rank metadata; this can perform linear work.
    // A nonempty sampled stream requires the exact completed target pair.
    // Empty targets may be retained too. No durable publication is implied.
    blob_type finish(std::shared_ptr<blob_type const> target = {}) {
      check_active();
      if (!done()) throw std::logic_error("index builder is not drained at EOF");
      auto target_count = target ? target->virtual_size() : 0;
      auto expected = target_count / group_size + (target_count % group_size != 0);
      if (expected != received_) throw std::invalid_argument("index samples disagree with target extent");
      try {
        flush_pending();
        blob_type result;
        result.native_ = native_;
        result.borrowed_ = writer_.finish();
        result.interleave_ = rank_groups<group_size>::build(classes_, virtual_count_);
        result.false_borrows_ = std::move(false_borrows_);
        result.virtual_count_ = virtual_count_;
        result.borrowed_policy_ = profile_borrowed_policy::shared_boundaries;
        result.target_ = std::move(target);
        finished_ = true;
        return result;
      } catch (...) {
        failed_ = true;
        throw;
      }
    }

  private:
    enum class input_mode { unset, full, coded };

    std::shared_ptr<typename blob_type::native_array const> native_;
    profile_cursor<P, stream_role::native> native_cursor_;
    profile_borrowed_writer<P> writer_;
    std::optional<bit_string> incoming_;
    std::optional<bit_string> pending_;
    std::optional<coded_sample_type> outgoing_;
    profile_sample_encoder<P> outgoing_encoder_;
    profile_sample_decoder<P> incoming_decoder_;
    std::vector<std::uint64_t> classes_;
    std::vector<std::byte> false_borrows_;
    std::uint64_t pending_ceiling_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t virtual_count_ = 0;
    std::uint64_t borrowed_count_ = 0;
    std::uint64_t received_ = 0;
    input_mode input_mode_ = input_mode::unset;
    bool incoming_false_ = false;
    bool pending_false_ = false;
    bool input_closed_ = false;
    bool finished_ = false;
    bool failed_ = false;

    void check_active() const {
      if (finished_ || failed_) throw std::logic_error("index builder is no longer active");
    }
    void check_input_slot(std::uint64_t target_ordinal) const {
      if (!needs_input()) throw std::logic_error("index builder cannot accept another lookahead");
      if (received_ > std::numeric_limits<std::uint64_t>::max() / group_size ||
          target_ordinal != received_ * group_size)
        throw std::invalid_argument("index samples must name consecutive target groups");
      if (received_ == std::numeric_limits<std::uint64_t>::max() - native_->size())
        throw std::length_error("index augmented count overflows");
    }
    void push_key(bit_view key, std::uint64_t target_ordinal) {
      check_input_slot(target_ordinal);
      if (key.size() % P::bits_per_unit)
        throw std::invalid_argument("index sample key disagrees with policy units");
      auto order = pending_ ? compare_bits(pending_->view(), key) : -1;
      if (order > 0) throw std::invalid_argument("index samples must be sorted");
      auto copy = bit_string::copy(key);
      incoming_.emplace(std::move(copy));
      incoming_false_ = !order && pending_false_;
      ++received_;
    }
    void flush_pending() {
      if (pending_) {
        writer_.append(pending_->view(), pending_ceiling_);
        pending_.reset();
      }
    }
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Everett's incremental fractional-index builder.
 */
