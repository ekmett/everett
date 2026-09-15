/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/profile_blob.h>

#include <algorithm>
#include <cstdint>
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
  template <class P> struct sample_cursor {
    using policy_type = P;
    using target_type = profile_blob<P>;
    static constexpr std::uint64_t group_size = P::group_size;

    explicit sample_cursor(std::shared_ptr<target_type const> target)
      : target_(checked_target(std::move(target))),
        native_(target_->native().view()), borrowed_(target_->borrowed().view()) {
      work_.decoded_entries = std::uint64_t(!native_.done()) + std::uint64_t(!borrowed_.done());
      choose_next();
    }

    bool done() const noexcept { return native_.done() && borrowed_.done(); }

    profile_sample_view<P> peek() const & {
      if (done()) throw std::out_of_range("sample cursor at end");
      auto item = next_borrowed_ ? borrowed_.peek() : native_.peek();
      return {item.key.prefix, ordinal_, next_borrowed_ ? stream_role::borrowed : stream_role::native,
              item.ordinal};
    }
    profile_sample_view<P> peek() const && = delete;

    void advance() {
      if (done()) throw std::out_of_range("sample cursor at end");
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
    static std::shared_ptr<target_type const> checked_target(std::shared_ptr<target_type const> target) {
      if (!target) throw std::invalid_argument("sample cursor requires a pinned target");
      return target;
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
