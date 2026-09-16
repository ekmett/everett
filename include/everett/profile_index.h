/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Everett's owning fractional-index artifact.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/profile.h>
#include <everett/rank_groups.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace everett {
  template <class P, class Native, class Output> struct index_builder;
  namespace profile_detail {
    template <class P> struct index_output;
    template <class P> struct index_metadata {
      rank_groups<P::group_size> interleave;
      std::vector<std::byte> false_borrows;
      std::vector<std::uint64_t> cut_lcps;
      std::uint64_t virtual_count = 0;
    };
  }

  // An owning index artifact, independent of native storage. Construction uses
  // a pinned source and trusted samples of one exact target. The caller keeps
  // those dependencies pinned through sealing and publication of this artifact.
  template <class P> struct profile_index {
    using policy_type = P;
    using borrowed_array = profile_array<P, stream_role::borrowed>;
    static constexpr std::uint64_t group_size = P::group_size;

    // A terminal pair has no borrowed occurrences. Construct its zero
    // directories from the admitted native count without visiting native keys.
    static profile_index native_only(std::uint64_t count) {
      auto groups = count / group_size + (count % group_size != 0);
      std::vector<std::uint64_t> cuts;
      if (groups > cuts.max_size()) error_detail::raise<std::length_error>("native index is too large");
      cuts.resize(static_cast<std::size_t>(groups), 0);
      auto ranks = rank_groups<group_size>::build(cuts, count);
      return profile_index(borrowed_array::build({}), std::move(ranks), {}, std::move(cuts), count);
    }

    borrowed_array const & borrowed() const & noexcept { return borrowed_; }
    borrowed_array const & borrowed() const && = delete;
    rank_groups<group_size> const & interleave() const & noexcept { return interleave_; }
    rank_groups<group_size> const & interleave() const && = delete;
    std::span<std::byte const> false_borrow_bits() const & noexcept { return false_borrows_; }
    std::span<std::byte const> false_borrow_bits() const && = delete;
    std::span<std::uint64_t const> cut_lcps() const & noexcept { return cut_lcps_; }
    std::span<std::uint64_t const> cut_lcps() const && = delete;
    std::uint64_t virtual_size() const noexcept { return virtual_count_; }
    std::uint64_t native_size() const noexcept { return virtual_count_ - borrowed_.size(); }

  private:
    template <class, class, class> friend struct index_builder;
    friend struct profile_detail::index_output<P>;

    borrowed_array borrowed_;
    rank_groups<group_size> interleave_;
    std::vector<std::byte> false_borrows_;
    std::vector<std::uint64_t> cut_lcps_;
    std::uint64_t virtual_count_;

    profile_index(borrowed_array borrowed, rank_groups<group_size> interleave,
        std::vector<std::byte> false_borrows, std::vector<std::uint64_t> cut_lcps,
        std::uint64_t virtual_count)
      : borrowed_(std::move(borrowed)), interleave_(std::move(interleave)),
        false_borrows_(std::move(false_borrows)), cut_lcps_(std::move(cut_lcps)),
        virtual_count_(virtual_count) {}
  };
  namespace profile_detail {
    // Default index output owns borrowed FC bytes. Alternate outputs share the
    // same builder and consume this known adjacent-key comparison synchronously.
    template <class P> struct index_output {
      using policy_type = P;
      index_output() = default;
      index_output(index_output const &) = delete;
      index_output & operator=(index_output const &) = delete;
      index_output(index_output &&) = default;
      index_output & operator=(index_output &&) = default;
      std::uint64_t size() const noexcept { return writer_.size(); }
      bool finished() const noexcept { return writer_.finished(); }
      bool failed() const noexcept { return false; }
      void append_known(bit_view key, std::uint64_t common_bits) { writer_.append_known(key, common_bits); }
      profile_index<P> finish(index_metadata<P> metadata) {
        auto borrowed = writer_.finish();
        return profile_index<P>(std::move(borrowed), std::move(metadata.interleave),
          std::move(metadata.false_borrows), std::move(metadata.cut_lcps), metadata.virtual_count);
      }
    private:
      profile_borrowed_writer<P> writer_;
    };
  }

}
