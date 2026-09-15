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

#include <everett/sampling.h>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace everett {
  // Incremental ordinary-FC index construction over an unchanged native array.
  // One incoming and one outgoing sample provide explicit backpressure. The
  // source wrapper may expire: native_ retains its actual encoded allocation.
  // step() budgets merged occurrences, not key bytes, allocations or final EF
  // construction. Inputs must come from the exact target's trusted sampler;
  // finish() checks its count but does not rescan it to authenticate every key.
  // Coded handoff queues a backspace count, suffix and target ordinal. Backspace
  // counts P units relative to the preceding emitted sample. Decoder, lookahead,
  // pending-writer and outgoing-encoder contexts still retain full current keys.
  template <class P, class Native>
  struct index_builder {
    using policy_type = P;
    using native_type = Native;
    static_assert(std::same_as<typename Native::policy_type, P>);
    using blob_type = profile_blob<P>;
    using sample_type = profile_sample<P>;
    using coded_sample_type = profile_coded_sample<P>;
    static constexpr std::uint64_t group_size = P::group_size;

    explicit index_builder(blob_type const & source)
      requires std::same_as<Native, typename blob_type::native_array>
      : index_builder(source.native_) {}

    explicit index_builder(std::shared_ptr<Native const> native)
      : native_(checked_native(std::move(native))), native_cursor_(native_->view()) {}

    index_builder(index_builder const &) = delete;
    index_builder & operator=(index_builder const &) = delete;
    index_builder(index_builder &&) = default;
    index_builder & operator=(index_builder &&) = default;

    bool needs_input() const noexcept {
      return native_ && !finished_ && !failed_ && !input_closed_ && !incoming_;
    }
    bool has_output() const noexcept { return bool(outgoing_); }
    bool done() const noexcept {
      return native_ && !failed_ && input_closed_ && !incoming_ && native_cursor_.done() && !outgoing_;
    }
    bool finished() const noexcept { return finished_; }
    std::uint64_t size() const noexcept { return virtual_count_; }
    std::uint64_t received_samples() const noexcept { return received_; }

    void push(bit_view key, std::uint64_t target_ordinal) {
      check_active();
      if (input_mode_ == input_mode::coded) error_detail::raise<std::logic_error>("cannot mix coded and full index inputs");
      push_key(key, target_ordinal);
      input_mode_ = input_mode::full;
    }

    void push(coded_sample_type const & sample) {
      check_active();
      if (input_mode_ == input_mode::full) error_detail::raise<std::logic_error>("cannot mix coded and full index inputs");
      check_input_slot(sample.target_ordinal);
      auto [key, comparison] = incoming_decoder_.accept_compared(sample);
      try {
        // The decoder's current key is copied once into the existing lookahead
        // state. Failure after advancing that context makes this stage unusable.
        accept_key(key, comparison);
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
      if (input_closed_) error_detail::raise<std::logic_error>("index input is already closed");
      input_closed_ = true;
    }

    std::uint64_t step(std::uint64_t budget_entries) {
      check_active();
      std::uint64_t consumed = 0;
      try {
        while (consumed < budget_entries && !outgoing_) {
          if ((!incoming_ && !input_closed_) || (!incoming_ && native_cursor_.done())) break;
          auto comparison = compare_heads();
          auto take_borrowed = comparison.order > 0;
          if (incoming_ && !comparison.order) incoming_false_ = true;
          auto key = take_borrowed ? incoming_->view() : native_cursor_.peek().key.prefix;
          auto edge = take_borrowed ? incoming_common_ : native_common_;
          outgoing_common_ = std::min(outgoing_common_, edge);
          if (pending_) pending_common_ = std::min(pending_common_, edge);
          auto boundary = virtual_count_ % group_size == 0;
          if (boundary) {
            outgoing_.emplace(outgoing_encoder_.encode_known(key, virtual_count_, outgoing_common_));
            outgoing_common_ = key.size();
            classes_.push_back(0);
            cut_lcps_.push_back(pending_ ? pending_common_ : 0);
          }
          if (take_borrowed) {
            flush_pending();
            pending_ = std::move(incoming_);
            incoming_.reset();
            pending_false_ = incoming_false_;
            incoming_false_ = false;
            pending_retained_ = incoming_borrowed_common_;
            pending_common_ = key.size();
            native_common_ = comparison.common_bits;
            auto ordinal = borrowed_count_++;
            if ((ordinal & 7) == 0) false_borrows_.push_back(std::byte{0});
            if (pending_false_) false_borrows_.back() |= std::byte(1u << (ordinal & 7));
            ++classes_.back();
          } else {
            incoming_common_ = comparison.common_bits;
            auto next = native_cursor_.advance_comparison();
            native_common_ = next ? next->common_bits : 0;
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
      if (!outgoing_) error_detail::raise<std::logic_error>("index builder has no outgoing sample");
      // Compatibility path: materialize a full queued key only when requested.
      sample_type result{bit_string::copy(outgoing_encoder_.key()), outgoing_->target_ordinal};
      outgoing_.reset();
      return result;
    }

    // Frames depend on every preceding emitted sample, including any retrieved
    // through the full-key compatibility interface. Consume coded frames in order.
    coded_sample_type take_coded_output() {
      check_active();
      if (!outgoing_) error_detail::raise<std::logic_error>("index builder has no outgoing sample");
      auto result = std::move(*outgoing_);
      outgoing_.reset();
      return result;
    }

    // Finalizes encoded offset/rank metadata; this can perform linear work.
    // A nonempty sampled stream requires the exact completed target pair.
    // Empty targets may be retained too. No durable publication is implied.
    blob_type finish(std::shared_ptr<blob_type const> target = {})
      requires std::same_as<Native, typename blob_type::native_array> {
      auto index = finish_index(target ? target->virtual_size() : 0);
      try {
        blob_type result;
        result.native_ = native_;
        result.borrowed_ = std::move(index.borrowed_);
        result.interleave_ = std::move(index.interleave_);
        result.false_borrows_ = std::move(index.false_borrows_);
        result.virtual_count_ = index.virtual_count_;
        result.cut_lcps_ = std::move(index.cut_lcps_);
        result.target_ = std::move(target);
        return result;
      } catch (...) {
        failed_ = true;
        throw;
      }
    }

    // Keep native bytes in their original storage. The extent authenticates
    // only the sample count; the caller supplies samples from the exact target
    // and retains source/target pins until the resulting index is published.
    profile_index<P> finish_index(std::uint64_t target_count) {
      check_active();
      if (!done()) error_detail::raise<std::logic_error>("index builder is not drained at EOF");
      auto expected = target_count / group_size + (target_count % group_size != 0);
      if (expected != received_) error_detail::raise<std::invalid_argument>("index samples disagree with target extent");
      try {
        flush_pending();
        auto borrowed = writer_.finish();
        auto interleave = rank_groups<group_size>::build(classes_, virtual_count_);
        profile_index<P> result(std::move(borrowed), std::move(interleave),
          std::move(false_borrows_), std::move(cut_lcps_), virtual_count_);
        finished_ = true;
        return result;
      } catch (...) {
        failed_ = true;
        throw;
      }
    }

  private:
    enum class input_mode { unset, full, coded };

    std::shared_ptr<Native const> native_;
    profile_cursor<P, stream_role::native> native_cursor_;
    profile_borrowed_writer<P> writer_;
    std::optional<bit_string> incoming_;
    std::optional<bit_string> pending_;
    std::optional<coded_sample_type> outgoing_;
    profile_sample_encoder<P> outgoing_encoder_;
    profile_sample_decoder<P> incoming_decoder_;
    std::vector<std::uint64_t> classes_;
    std::vector<std::byte> false_borrows_;
    std::vector<std::uint64_t> cut_lcps_;
    std::uint64_t virtual_count_ = 0;
    std::uint64_t borrowed_count_ = 0;
    std::uint64_t received_ = 0;
    // Exact bit LCPs from the last consumed merged key to each live head.
    std::uint64_t native_common_ = 0, incoming_common_ = 0;
    // Minima of adjacent merged-key LCPs since each retained anchor. Sorted
    // strings make these the exact LCPs with the last consumed merged key.
    std::uint64_t outgoing_common_ = 0, pending_common_ = 0;
    std::uint64_t incoming_borrowed_common_ = 0, pending_retained_ = 0;
    input_mode input_mode_ = input_mode::unset;
    bool incoming_false_ = false;
    bool pending_false_ = false;
    bool input_closed_ = false;
    bool finished_ = false;
    bool failed_ = false;

    void check_active() const {
      if (!native_ || finished_ || failed_) error_detail::raise<std::logic_error>("index builder is no longer active");
    }
    static std::shared_ptr<Native const> checked_native(std::shared_ptr<Native const> native) {
      if (!native) error_detail::raise<std::invalid_argument>("index builder requires a pinned native source");
      return native;
    }
    void check_input_slot(std::uint64_t target_ordinal) const {
      if (!needs_input()) error_detail::raise<std::logic_error>("index builder cannot accept another lookahead");
      if (received_ > std::numeric_limits<std::uint64_t>::max() / group_size ||
          target_ordinal != received_ * group_size)
        error_detail::raise<std::invalid_argument>("index samples must name consecutive target groups");
      if (received_ == std::numeric_limits<std::uint64_t>::max() - native_->size())
        error_detail::raise<std::length_error>("index augmented count overflows");
    }
    void push_key(bit_view key, std::uint64_t target_ordinal) {
      check_input_slot(target_ordinal);
      if (key.size() & (P::bits_per_unit - 1))
        error_detail::raise<std::invalid_argument>("index sample key disagrees with policy units");
      auto comparison = pending_ ? compare_common_bits(pending_->view(), key) : bit_comparison{0, -1};
      if (comparison.order > 0) error_detail::raise<std::invalid_argument>("index samples must be sorted");
      accept_key(key, comparison);
    }
    // The coded decoder's previous accepted key is exactly pending_: receiving
    // another key requires consuming the old incoming occurrence first. Reuse
    // its already checked suffix comparison instead of scanning the full key.
    void accept_key(bit_view key, bit_comparison comparison) {
      auto copy = bit_string::copy(key);
      incoming_.emplace(std::move(copy));
      incoming_false_ = !comparison.order && pending_false_;
      incoming_common_ = incoming_borrowed_common_ = comparison.common_bits;
      ++received_;
    }
    bit_comparison compare_heads() const {
      if (!incoming_) return {0, -1};
      if (native_cursor_.done()) return {0, 1};
      if (native_common_ != incoming_common_)
        return {std::min(native_common_, incoming_common_), native_common_ > incoming_common_ ? -1 : 1};
      auto native = native_cursor_.peek().key.prefix;
      auto incoming = incoming_->view();
      // Revisit at most seven already equal bits to keep byte-aligned loads.
      auto start = native_common_ & ~std::uint64_t{7};
      if (!start) return compare_common_bits(native, incoming);
      auto comparison = compare_common_bits(native.subview(start, native.size() - start),
        incoming.subview(start, incoming.size() - start));
      comparison.common_bits += start;
      return comparison;
    }
    void flush_pending() {
      if (pending_) {
        writer_.append_known(pending_->view(), pending_retained_);
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
