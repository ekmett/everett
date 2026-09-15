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

#include <everett/profile_blob.h>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace everett {
  template <class P> struct profile_sample {
    using policy_type = P;
    bit_string key;
    std::uint64_t target_ordinal = 0;
  };

  template <class P> struct profile_sample_view {
    using policy_type = P;
    bit_view key;
    std::uint64_t target_ordinal = 0;
    stream_role source_role = stream_role::native;
    std::uint64_t source_ordinal = 0;
  };

  // A typed in-memory handoff, not a serialized count-code format. Backspace
  // counts P units from the previously emitted sample; suffix owns only the
  // remaining key bits. The first sample is literal (backspace == 0).
  template <class P> struct profile_coded_sample {
    using policy_type = P;
    std::uint64_t backspace = 0;
    bit_string suffix;
    std::uint64_t target_ordinal = 0;
  };

  namespace sampling_detail {
    template <class P> void check_ordinal(std::uint64_t count, std::uint64_t ordinal) {
      if (count > std::numeric_limits<std::uint64_t>::max() / P::group_size || ordinal != count * P::group_size)
        error_detail::raise<std::invalid_argument>("sample ordinals must be consecutive policy groups");
    }

    // Suffix must not alias context. Allocate before changing its retained
    // prefix, then the bounded resize/copy operations cannot allocate or fail.
    // Preserving a prefix does not copy it unless the buffer must grow; growth
    // is geometric so gradually lengthening keys do not reallocate each time.
    inline void replace_suffix(bit_string & context, std::uint64_t retained, bit_view suffix) {
      auto bits = profile_detail::add(retained, suffix.size());
      if (bits > std::numeric_limits<std::uint64_t>::max() - 7)
        error_detail::raise<std::length_error>("sample key is too large");
      auto bytes = profile_detail::byte_count(bits);
      if (bytes > context.bytes.max_size()) error_detail::raise<std::length_error>("sample key is too large");
      if (bytes > context.bytes.capacity()) {
        auto capacity = context.bytes.capacity();
        auto grown = capacity > (context.bytes.max_size() >> 1) ? context.bytes.max_size() : capacity << 1;
        context.bytes.reserve(std::max(static_cast<std::size_t>(bytes), grown));
      }
      profile_detail::resize(context, retained);
      profile_detail::append(context, suffix);
    }
  }

  // One reusable reconstructed key per endpoint. Validation/allocation failure
  // leaves the preceding accepted key and ordinal intact. key() borrows that
  // context until the next successful encode/accept, move, or destruction.
  template <class P> struct profile_sample_encoder {
    using policy_type = P;

    profile_coded_sample<P> encode(bit_view key, std::uint64_t target_ordinal) {
      sampling_detail::check_ordinal<P>(count_, target_ordinal);
      if (key.size() & (P::bits_per_unit - 1)) error_detail::raise<std::invalid_argument>("sample key unit mismatch");
      auto previous = key_.view();
      auto comparison = compare_common_bits(previous, key);
      if (comparison.order > 0) error_detail::raise<std::invalid_argument>("sample keys must be sorted");
      return encode_known(key, target_ordinal, comparison.common_bits);
    }

    bit_view key() const & { return key_.view(); }
    bit_view key() const && = delete;
    std::uint64_t size() const noexcept { return count_; }

  private:
    template <class, class> friend struct index_builder;
    // The builder supplies exact comparison state for sorted, unit-aligned
    // keys and consecutive output ordinals; the public entry remains checked.
    profile_coded_sample<P> encode_known(bit_view key, std::uint64_t target_ordinal,
                                         std::uint64_t common_bits) {
      auto retained = common_bits >> P::unit_shift;
      auto retained_bits = profile_detail::multiply(retained, P::bits_per_unit);
      // Copy the transmitted suffix before editing context. Input may alias
      // this encoder's current key, including a subview of that key.
      profile_coded_sample<P> result{(key_.bit_size >> P::unit_shift) - retained,
        bit_string::copy(key.subview(retained_bits, key.size() - retained_bits)), target_ordinal};
      sampling_detail::replace_suffix(key_, retained_bits, result.suffix.view());
      ++count_;
      return result;
    }

    bit_string key_;
    std::uint64_t count_ = 0;
  };

  template <class P> struct profile_sample_decoder {
    using policy_type = P;

    bit_view accept(profile_coded_sample<P> const & sample) { return accept_compared(sample).first; }

    bit_view key() const & { return key_.view(); }
    bit_view key() const && = delete;
    std::uint64_t size() const noexcept { return count_; }

  private:
    template <class, class> friend struct index_builder;
    std::pair<bit_view, bit_comparison> accept_compared(profile_coded_sample<P> const & sample) {
      sampling_detail::check_ordinal<P>(count_, sample.target_ordinal);
      auto suffix = sample.suffix.view();
      if (suffix.size() & (P::bits_per_unit - 1)) error_detail::raise<std::invalid_argument>("sample suffix unit mismatch");
      auto previous = key_.view();
      auto previous_units = previous.size() >> P::unit_shift;
      if (sample.backspace > previous_units) error_detail::raise<std::invalid_argument>("sample backspace exceeds previous key");
      auto retained = profile_detail::multiply(previous_units - sample.backspace, P::bits_per_unit);
      // Both keys share the retained prefix. Comparing only the two remaining
      // suffixes validates order without another full-key reconstruction.
      auto comparison = compare_common_bits(previous.subview(retained, previous.size() - retained), suffix);
      if (comparison.order > 0)
        error_detail::raise<std::invalid_argument>("sample keys must be sorted");
      comparison.common_bits += retained;
      sampling_detail::replace_suffix(key_, retained, suffix);
      ++count_;
      return {key_.view(), comparison};
    }

    bit_string key_;
    std::uint64_t count_ = 0;
  };

  struct sampling_work {
    std::uint64_t native_entries = 0;
    std::uint64_t borrowed_entries = 0;
    std::uint64_t decoded_entries = 0;
    std::uint64_t key_comparisons = 0;

    std::uint64_t consumed_entries() const noexcept { return native_entries + borrowed_entries; }
  };

  // Sequential samples of one exact, immutable native/index pair. Both streams
  // retain their own front-coding context. Construction decodes at most two
  // records; advance consumes at most P::group_size merged occurrences. Native
  // entries precede equal borrowed entries, and borrowed duplicates remain.
  //
  // These are record-work bounds: decoding/comparison still pays for key bits.
  // Scratch space holds two current reconstructed keys. Values are never
  // copied, and no array of reconstructed keys or samples is materialized.
  // peek() borrows cursor scratch until the next advance, move, or destruction.
  // A moved-to cursor remains valid; fresh peek() calls derive fresh key views.
  template <class P, class Target = profile_blob<P>> struct sample_cursor {
    using policy_type = P;
    using target_type = Target;
    static_assert(std::same_as<typename Target::policy_type, P>);
    static constexpr std::uint64_t group_size = P::group_size;

    explicit sample_cursor(std::shared_ptr<target_type const> target)
      : sample_cursor(bind_target(std::move(target))) {}

    bool done() const noexcept { return !target_ || (native_.done() && borrowed_.done()); }

    profile_sample_view<P> peek() const & {
      if (done()) error_detail::raise<std::out_of_range>("sample cursor at end");
      auto item = next_borrowed_ ? borrowed_.peek() : native_.peek();
      return {item.key.prefix, ordinal_, next_borrowed_ ? stream_role::borrowed : stream_role::native,
              item.ordinal};
    }
    profile_sample_view<P> peek() const && = delete;

    void advance() {
      if (done()) error_detail::raise<std::out_of_range>("sample cursor at end");
      auto count = std::min(group_size, target_->virtual_size() - ordinal_);
      for (std::uint64_t i = 0; i != count; ++i) {
        if (next_borrowed_) {
          borrowed_.advance();
          ++work_.borrowed_entries;
          if (!borrowed_.done()) ++work_.decoded_entries;
        } else {
          native_.advance();
          ++work_.native_entries;
          if (!native_.done()) ++work_.decoded_entries;
        }
        ++ordinal_;
        choose_next();
      }
    }

    std::shared_ptr<target_type const> target() const noexcept { return target_; }
    sampling_work const & counters() const noexcept { return work_; }

  private:
    struct binding {
      std::shared_ptr<target_type const> target;
      profile_blob_view<P> view;
    };
    static binding bind_target(std::shared_ptr<target_type const> target) {
      if (!target) error_detail::raise<std::invalid_argument>("sample cursor requires a pinned target");
      auto view = target->view();
      return {std::move(target), view};
    }
    explicit sample_cursor(binding source)
      : target_(std::move(source.target)), native_(source.view.native()), borrowed_(source.view.borrowed()) {
      work_.decoded_entries = std::uint64_t(!native_.done()) + std::uint64_t(!borrowed_.done());
      choose_next();
    }

    void choose_next() {
      if (native_.done()) next_borrowed_ = true;
      else if (borrowed_.done()) next_borrowed_ = false;
      else {
        ++work_.key_comparisons;
        next_borrowed_ = compare_bits(borrowed_.peek().key.prefix, native_.peek().key.prefix) < 0;
      }
    }

    std::shared_ptr<target_type const> target_;
    profile_cursor<P, stream_role::native> native_;
    profile_cursor<P, stream_role::borrowed> borrowed_;
    sampling_work work_;
    std::uint64_t ordinal_ = 0;
    bool next_borrowed_ = false;
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Everett's sequential sampling of pinned encoded blob pairs.
 */
