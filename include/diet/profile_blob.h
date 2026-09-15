/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Diet's profile blob support.
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <diet/error_detail.h>

#include <diet/profile.h>
#include <diet/profile_index.h>
#include <diet/rank_groups.h>
#include <diet/word_view.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace diet {
  template <class P, class Native = profile_array<P>, class Output = profile_detail::index_output<P>>
  struct index_builder;

  struct profile_blob_window {
    std::uint64_t native_first = 0;
    std::uint64_t native_last = 0;
    std::uint64_t borrowed_first = 0;
    std::uint64_t borrowed_last = 0;
  };

  template <class P>
  struct profile_blob_native_match {
    using policy_type = P;
    std::uint64_t ordinal = 0;
    bit_string value;
  };

  template <class P>
  struct profile_blob_borrowed_predecessor {
    using policy_type = P;
    std::uint64_t ordinal = 0;
    std::uint64_t target_ordinal = 0;
    bool false_borrow = false;
    profile_query_context<P> comparison;
  };

  template <class P>
  struct profile_blob_window_result {
    using policy_type = P;
    std::optional<profile_blob_native_match<P>> native;
    std::optional<profile_blob_borrowed_predecessor<P>> borrowed_predecessor;
  };

  // A borrowed query view over one native/index pair. The backing sections
  // must remain immutable and outlive the view. Construction checks shapes;
  // query-time projection validates the selected ranks before subtraction.
  // Exact sample keys, cut LCPs and dependency identities require admission
  // validation by the owner; this view does not scan their contents.
  template <class P>
  struct profile_blob_view {
    using policy_type = P;
    static constexpr std::uint64_t group_size = P::group_size;
    using native_view = profile_view<P, stream_role::native>;
    using borrowed_view = profile_view<P, stream_role::borrowed>;

    profile_blob_view(native_view native, borrowed_view borrowed,
        rank_groups_view<group_size> interleave, std::span<std::byte const> false_borrows,
        word_view cut_lcps, std::uint64_t virtual_count)
      : native_(native), borrowed_(borrowed), interleave_(interleave),
        false_borrows_(false_borrows), cut_lcps_(cut_lcps), virtual_count_(virtual_count) {
      if (borrowed.size() > std::numeric_limits<std::uint64_t>::max() - 7 ||
          native.size() > std::numeric_limits<std::uint64_t>::max() - borrowed.size() ||
          native.size() + borrowed.size() != virtual_count ||
          interleave.size() != virtual_count ||
          false_borrows.size() != ((borrowed.size() + 7) >> 3) ||
          cut_lcps.size() != group_count())
        error_detail::raise<std::invalid_argument>("profile blob section shape mismatch");
    }

    native_view const & native() const noexcept { return native_; }
    borrowed_view const & borrowed() const noexcept { return borrowed_; }
    rank_groups_view<group_size> const & interleave() const noexcept { return interleave_; }
    std::span<std::byte const> false_borrow_bits() const noexcept { return false_borrows_; }
    word_view cut_lcps() const noexcept { return cut_lcps_; }
    std::uint64_t virtual_size() const noexcept { return virtual_count_; }
    std::uint64_t group_count() const noexcept {
      return virtual_count_ / group_size + (virtual_count_ % group_size != 0);
    }

    bool false_borrow(std::uint64_t ordinal) const {
      if (ordinal >= borrowed_.size()) error_detail::raise<std::out_of_range>("profile blob borrowed ordinal");
      return (std::to_integer<unsigned>(false_borrows_[ordinal >> 3]) >> (ordinal & 7)) & 1;
    }

    profile_blob_window project(std::uint64_t group) const {
      if (group >= group_count()) error_detail::raise<std::out_of_range>("profile blob virtual group");
      auto first = group * group_size;
      auto last = first + std::min<std::uint64_t>(group_size, virtual_count_ - first);
      auto const & ranks = interleave_;
      auto a = ranks.rank(group);
      auto population = ranks.class_at(group);
      if (a > first || a > borrowed_.size() || population > last - first ||
          population > borrowed_.size() - a)
        error_detail::raise<std::invalid_argument>("invalid profile blob rank projection");
      auto b = a + population;
      if (last - b > native_.size())
        error_detail::raise<std::invalid_argument>("invalid profile blob native projection");
      return {first - a, last - b, a, b};
    }

    // lower compares the exact sampled boundary to its owned query. At group
    // zero an empty-key context suffices. The query must not precede lower.
    profile_blob_window_result<P> search_window(
        std::uint64_t group, profile_query_context<P> const & lower,
        profile_comparison_work * native_work = nullptr,
        profile_comparison_work * borrowed_work = nullptr) const {
      if (lower.order() > 0) error_detail::raise<std::invalid_argument>("query precedes its routed boundary");
      auto window = project(group);
      profile_blob_window_result<P> result;
      native_.compare_window(window.native_first, window.native_last, lower,
        [&](profile_comparison_item<P> item) {
          auto order = item.comparison.order();
          if (!order) result.native = profile_blob_native_match<P>{item.ordinal, bit_string::copy(item.value)};
          return order < 0;
        }, native_work);

      auto remember = [&](std::uint64_t ordinal, profile_query_context<P> const & comparison) {
        auto is_false = false_borrow(ordinal);
        result.borrowed_predecessor = profile_blob_borrowed_predecessor<P>{
          ordinal, checked_target_ordinal(ordinal), is_false, comparison};
        if (!comparison.order() && is_false && !result.native) {
          if (!window.native_first) error_detail::raise<std::invalid_argument>("false borrow has no native predecessor");
          auto native_ordinal = window.native_first - 1;
          result.native = profile_blob_native_match<P>{
            native_ordinal, bit_string::copy(native_.encoded_at(native_ordinal).value)};
        }
      };
      borrowed_.compare_window(window.borrowed_first, window.borrowed_last, lower,
        [&](profile_comparison_item<P> item) {
          if (item.comparison.order() > 0) return false;
          remember(item.ordinal, item.comparison);
          return true;
        }, borrowed_work);
      if (!result.borrowed_predecessor && window.borrowed_first) {
        // Ordered cut LCPs recover equality and direction without fetching the
        // preceding record's length or touching its physical block.
        auto comparison = lower.predecessor(cut_lcps_[group]);
        remember(window.borrowed_first - 1, comparison);
      }
      return result;
    }


  private:
    native_view native_;
    borrowed_view borrowed_;
    rank_groups_view<group_size> interleave_;
    std::span<std::byte const> false_borrows_;
    word_view cut_lcps_;
    std::uint64_t virtual_count_;

    static std::uint64_t checked_target_ordinal(std::uint64_t ordinal) {
      if (ordinal > std::numeric_limits<std::uint64_t>::max() / group_size) {
        error_detail::raise<std::overflow_error>("borrowed target ordinal overflows");
      }
      return ordinal * group_size;
    }
  };

  // Immutable native/index pair. P fixes key units and native value layout;
  // the borrowed role keeps P while contributing zero-width value slots.
  // Group ranks and target ordinals count entries, independently of P's units.
  template <class P>
  struct profile_blob {
    using policy_type = P;
    static constexpr std::uint64_t group_size = P::group_size;
    using native_array = profile_array<P, stream_role::native>;
    using borrowed_array = profile_array<P, stream_role::borrowed>;

    static profile_blob build(
        std::span<profile_record const> native,
        std::span<bit_string const> borrowed = {}) {
      check_count(native.size(), borrowed.size());
      for (std::size_t i = 1; i < native.size(); ++i) {
        if (compare_bits(native[i - 1].key.view(), native[i].key.view()) >= 0) {
          error_detail::raise<std::invalid_argument>("profile blob native keys must be strictly sorted");
        }
      }
      profile_blob result;
      result.native_ = std::make_shared<native_array const>(
        native_array::build(native));
      build_index(result, native, borrowed);
      return result;
    }

    // Adopt a trusted ordinary-FC native array without decoding or rebuilding
    // it. Keys must be unique. The all-native index needs only zero rank/cut
    // directories; profile_native_writer establishes the content precondition.
    static profile_blob adopt_native(native_array native) {
      (void)native.view();
      profile_blob result;
      result.virtual_count_ = native.size();
      auto groups = result.group_count();
      if (groups > result.cut_lcps_.max_size()) error_detail::raise<std::length_error>("native index is too large");
      result.cut_lcps_.resize(static_cast<std::size_t>(groups), 0);
      result.interleave_ = rank_groups<group_size>::build(result.cut_lcps_, native.size());
      result.native_ = std::make_shared<native_array const>(std::move(native));
      return result;
    }

    // Reindexing retains the exact ordinary-FC native allocation and its
    // independent physical directory. Only index-local navigation is rebuilt.
    profile_blob reindex(std::span<bit_string const> borrowed) const {
      check_count(native_->size(), borrowed.size());
      std::vector<profile_record> native_records;
      native_records.reserve(static_cast<std::size_t>(native_->size()));
      native_->view().visit_all([&](profile_item<P> item) {
        native_records.push_back({bit_string::copy(item.key.prefix), {}});
        return true;
      });
      profile_blob result;
      result.native_ = native_;
      build_index(result, native_records, borrowed);
      return result;
    }

    native_array const & native() const noexcept { return *native_; }
    borrowed_array const & borrowed() const noexcept { return borrowed_; }
    rank_groups<group_size> const & interleave() const noexcept { return interleave_; }
    std::span<std::byte const> false_borrow_bits() const noexcept { return false_borrows_; }
    std::uint64_t virtual_size() const noexcept { return virtual_count_; }
    std::span<std::uint64_t const> cut_lcps() const noexcept { return cut_lcps_; }
    // Incremental construction binds the exact downstream pair. Batch
    // build/reindex accept unbound sample spans and leave this empty.
    std::shared_ptr<profile_blob const> target() const noexcept { return target_; }
    std::uint64_t group_count() const noexcept {
      return virtual_count_ / group_size + (virtual_count_ % group_size != 0);
    }

    profile_blob_view<P> view() const & {
      return {native_->view(), borrowed_.view(), interleave_.view(), false_borrows_,
        word_view(std::span<std::uint64_t const>(cut_lcps_)), virtual_count_};
    }
    profile_blob_view<P> view() const && = delete;

    bool false_borrow(std::uint64_t ordinal) const { return view().false_borrow(ordinal); }
    profile_blob_window project(std::uint64_t group) const { return view().project(group); }
    profile_blob_window_result<P> search_window(
        std::uint64_t group, profile_query_context<P> const & lower,
        profile_comparison_work * native_work = nullptr,
        profile_comparison_work * borrowed_work = nullptr) const {
      return view().search_window(group, lower, native_work, borrowed_work);
    }

  private:
    friend struct index_builder<P, native_array>;

    std::shared_ptr<native_array const> native_ =
      std::make_shared<native_array const>(native_array::build({}));
    borrowed_array borrowed_ = borrowed_array::build({});
    rank_groups<group_size> interleave_ = rank_groups<group_size>::build({}, 0);
    std::vector<std::byte> false_borrows_;
    std::uint64_t virtual_count_ = 0;
    std::vector<std::uint64_t> cut_lcps_;
    std::shared_ptr<profile_blob const> target_;

    static void check_count(std::uint64_t native, std::uint64_t borrowed) {
      if (borrowed > std::numeric_limits<std::uint64_t>::max() - 7 ||
          native > std::numeric_limits<std::uint64_t>::max() - borrowed) {
        error_detail::raise<std::length_error>("profile blob virtual count overflows");
      }
    }


    static void build_index(profile_blob & result, std::span<profile_record const> native,
                            std::span<bit_string const> borrowed) {
      std::vector<profile_record> borrowed_records;
      borrowed_records.reserve(borrowed.size());
      result.false_borrows_.resize((borrowed.size() + 7) >> 3);
      std::size_t native_at = 0;
      for (std::size_t i = 0; i != borrowed.size(); ++i) {
        if (i && compare_bits(borrowed[i - 1].view(), borrowed[i].view()) > 0) {
          error_detail::raise<std::invalid_argument>("profile blob borrowed keys must be sorted");
        }
        while (native_at != native.size() &&
               compare_bits(native[native_at].key.view(), borrowed[i].view()) < 0) {
          ++native_at;
        }
        if (native_at != native.size() &&
            compare_bits(native[native_at].key.view(), borrowed[i].view()) == 0) {
          result.false_borrows_[i >> 3] |= static_cast<std::byte>(1u << (i & 7));
        }
        borrowed_records.push_back({borrowed[i], {}});
      }
      auto count = std::uint64_t(native.size()) + borrowed.size();
      std::vector<std::uint64_t> classes(static_cast<std::size_t>(count / group_size + (count % group_size != 0)));
      std::size_t a = 0;
      std::size_t s = 0;
      for (std::uint64_t i = 0; i != count; ++i) {
        // Native precedes every borrowed occurrence of an equal key.
        auto take_borrowed = s != borrowed.size() &&
          (a == native.size() || compare_bits(borrowed[s].view(), native[a].key.view()) < 0);
        if (i % group_size == 0) {
          auto const & boundary = take_borrowed ? borrowed[s] : native[a].key;
          result.cut_lcps_.push_back(s ? compare_common_bits(borrowed[s - 1].view(), boundary.view()).common_bits : 0);
        }
        if (take_borrowed) {
          ++classes[static_cast<std::size_t>(i / group_size)];
          ++s;
        } else {
          ++a;
        }
      }
      result.borrowed_ = borrowed_array::build(borrowed_records);
      result.interleave_ = rank_groups<group_size>::build(classes, count);
      result.virtual_count_ = count;
    }
  };
}
