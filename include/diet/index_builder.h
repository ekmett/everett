/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <diet/error_detail.h>

#include <diet/sampling.h>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace diet {
  // Incremental ordinary-FC index construction over an unchanged native array.
  // One incoming and one outgoing sample provide explicit backpressure. The
  // source wrapper may expire: native_ retains its actual encoded allocation.
  // step() budgets merged occurrences, not key bytes, allocations or final EF
  // construction. Inputs must come from the exact target's trusted sampler;
  // finish() checks its count but does not rescan it to authenticate every key.
  // Coded handoff queues a backspace count, suffix and target ordinal. Backspace
  // counts P units relative to the preceding emitted sample. The incoming decoder
  // owns one reusable key: queued input until consumed, then the preceding
  // borrowed key until the next push. The outgoing encoder owns its own context.
  // Output has nonthrowing move assignment, starts empty/active and consumes
  // append_known(key, exact_bit_lcp)
  // synchronously. Its finish takes owning navigation metadata and may return
  // an artifact or receipt. Any step/finalization failure poisons this builder.
  template <class P, class Native, class Output>
  struct index_builder {
    using policy_type = P;
    using native_type = Native;
    using output_type = Output;
    static_assert(std::same_as<typename Output::policy_type, P>);
    static_assert(std::is_nothrow_move_assignable_v<Output>,
      "index output move assignment must not throw");
    static_assert(noexcept(std::declval<Output const &>().finished()));
    static_assert(noexcept(std::declval<Output const &>().failed()));
    static_assert(std::same_as<typename Native::policy_type, P>);
    using blob_type = profile_blob<P>;
    using sample_type = profile_sample<P>;
    using coded_sample_type = profile_coded_sample<P>;
    static constexpr std::uint64_t group_size = P::group_size;

    explicit index_builder(blob_type const & source)
      requires (std::same_as<Native, typename blob_type::native_array> &&
                std::same_as<Output, profile_detail::index_output<P>>)
      : index_builder(source.native_) {}

    explicit index_builder(std::shared_ptr<Native const> native)
      requires std::is_default_constructible_v<Output>
      : index_builder(Output{}, std::move(native)) {}
    index_builder(Output output, std::shared_ptr<Native const> native)
      : native_(checked_native(std::move(native))), native_cursor_(native_->view()),
        writer_(checked_output(std::move(output))) {}

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
    bool failed() const noexcept { return failed_; }
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
      auto comparison = incoming_decoder_.accept_compared(sample).second;
      accept_key(comparison);
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
          auto key = take_borrowed ? incoming_decoder_.key() : native_cursor_.peek().key.prefix;
          auto edge = take_borrowed ? incoming_common_ : native_common_;
          outgoing_common_ = std::min(outgoing_common_, edge);
          if (borrowed_count_) pending_common_ = std::min(pending_common_, edge);
          auto boundary = virtual_count_ % group_size == 0;
          if (boundary) {
            outgoing_.emplace(outgoing_encoder_.encode_known(key, virtual_count_, outgoing_common_));
            outgoing_common_ = key.size();
            if (virtual_count_) classes_.append(current_class_);
            current_class_ = 0;
            cut_lcps_.push_back(borrowed_count_ ? pending_common_ : 0);
          }
          if (take_borrowed) {
            // Output borrows this key only for the call. It is encoded now,
            // before the decoder can replace its suffix on a later push.
            writer_.append_known(key, incoming_borrowed_common_);
            incoming_ = false;
            previous_false_ = incoming_false_;
            incoming_false_ = false;
            pending_common_ = key.size();
            native_common_ = comparison.common_bits;
            auto ordinal = borrowed_count_++;
            if ((ordinal & 7) == 0) false_borrows_.push_back(std::byte{0});
            if (previous_false_) false_borrows_.back() |= std::byte(1u << (ordinal & 7));
            ++current_class_;
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
      requires (std::same_as<Native, typename blob_type::native_array> &&
                std::same_as<Output, profile_detail::index_output<P>>) {
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
    auto finish_index(std::uint64_t target_count)
      -> decltype(std::declval<Output &>().finish(std::declval<profile_detail::index_metadata<P>>())) {
      check_active();
      if (!done()) error_detail::raise<std::logic_error>("index builder is not drained at EOF");
      auto expected = target_count / group_size + (target_count % group_size != 0);
      if (expected != received_) error_detail::raise<std::invalid_argument>("index samples disagree with target extent");
      try {
        // Completed groups are already packed. Admit the final partial group
        // before irreversible final output writes.
        if (virtual_count_) {
          auto tail = virtual_count_ % group_size;
          classes_.append(current_class_, tail ? tail : group_size);
        }
        auto interleave = classes_.finish();
        profile_detail::index_metadata<P> metadata{std::move(interleave),
          std::move(false_borrows_), std::move(cut_lcps_), virtual_count_};
        auto result = writer_.finish(std::move(metadata));
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
    Output writer_;
    // Queued: received == borrowed_count + 1; otherwise they are equal.
    // The decoder holds the queued key or the last consumed borrowed key.
    // Output has already encoded exactly borrowed_count successful records.
    bool incoming_ = false;
    std::optional<coded_sample_type> outgoing_;
    profile_sample_encoder<P> outgoing_encoder_;
    profile_sample_decoder<P> incoming_decoder_;
    rank_groups_builder<group_size> classes_;
    std::uint64_t current_class_ = 0;
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
    std::uint64_t incoming_borrowed_common_ = 0;
    input_mode input_mode_ = input_mode::unset;
    bool incoming_false_ = false;
    bool previous_false_ = false;
    bool input_closed_ = false;
    bool finished_ = false;
    bool failed_ = false;

    void check_active() const {
      if (!native_ || finished_ || failed_) error_detail::raise<std::logic_error>("index builder is no longer active");
    }
    static Output checked_output(Output output) {
      if (output.size() || output.finished() || output.failed())
        error_detail::raise<std::invalid_argument>("index builder requires an empty active output");
      return output;
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
      auto comparison = incoming_decoder_.accept_full(key, target_ordinal);
      accept_key(comparison);
    }
    // A new input is admitted only after its predecessor was consumed. The
    // decoder therefore compares against the last borrowed key, even though
    // the active merged frontier can advance through native keys afterward.
    void accept_key(bit_comparison comparison) noexcept {
      incoming_ = true;
      incoming_false_ = !comparison.order && previous_false_;
      incoming_common_ = incoming_borrowed_common_ = comparison.common_bits;
      ++received_;
    }
    bit_comparison compare_heads() const {
      if (!incoming_) return {0, -1};
      if (native_cursor_.done()) return {0, 1};
      if (native_common_ != incoming_common_)
        return {std::min(native_common_, incoming_common_), native_common_ > incoming_common_ ? -1 : 1};
      auto native = native_cursor_.peek().key.prefix;
      auto incoming = incoming_decoder_.key();
      // Revisit at most seven already equal bits to keep byte-aligned loads.
      auto start = native_common_ & ~std::uint64_t{7};
      if (!start) return compare_common_bits(native, incoming);
      auto comparison = compare_common_bits(native.subview(start, native.size() - start),
        incoming.subview(start, incoming.size() - start));
      comparison.common_bits += start;
      return comparison;
    }

  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Diet's incremental fractional-index builder.
 */
