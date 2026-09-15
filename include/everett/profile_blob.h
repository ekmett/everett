/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/profile.h>
#include <everett/rank_groups.h>

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

namespace everett {
  template <class P> struct index_builder;

  enum class profile_borrowed_policy {
    ordinary,
    bidirectional,
    shared_boundaries
  };

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
    bool has_context = false;
    bit_string prefix;
    std::uint64_t full_units = 0;
    bool false_borrow = false;

    profile_anchor<P> anchor() const & { return {prefix.view(), full_units}; }
    profile_anchor<P> anchor() const && = delete;
  };

  template <class P>
  struct profile_blob_window_result {
    using policy_type = P;
    std::optional<profile_blob_native_match<P>> native;
    std::optional<profile_blob_borrowed_predecessor<P>> borrowed_predecessor;
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
        std::span<bit_string const> borrowed = {},
        std::uint64_t native_restart_factor = 18,
        std::span<std::uint64_t const> borrowed_prefix_ceilings = {},
        profile_borrowed_policy policy = profile_borrowed_policy::shared_boundaries) {
      check_count(native.size(), borrowed.size());
      for (std::size_t i = 1; i < native.size(); ++i) {
        if (compare_bits(native[i - 1].key.view(), native[i].key.view()) >= 0) {
          throw std::invalid_argument("profile blob native keys must be strictly sorted");
        }
      }
      profile_blob result;
      result.native_ = std::make_shared<native_array const>(
        native_array::build(native, {}, native_restart_factor));
      build_index(result, native, borrowed, borrowed_prefix_ceilings, policy);
      return result;
    }

    // This builder uses decoded native keys as scratch, but retains the exact
    // native allocation and its offset index. It never changes native LPFC to
    // match a replacement fractional index's new shared boundaries.
    profile_blob reindex(
        std::span<bit_string const> borrowed,
        std::span<std::uint64_t const> borrowed_prefix_ceilings = {},
        profile_borrowed_policy policy = profile_borrowed_policy::shared_boundaries) const {
      check_count(native_->size(), borrowed.size());
      std::vector<profile_record> native_records;
      native_records.reserve(static_cast<std::size_t>(native_->size()));
      native_->view().visit_all([&](profile_item<P> item) {
        native_records.push_back({bit_string::copy(item.key.prefix), {}});
        return true;
      });
      profile_blob result;
      result.native_ = native_;
      build_index(result, native_records, borrowed, borrowed_prefix_ceilings, policy);
      return result;
    }

    native_array const & native() const noexcept { return *native_; }
    borrowed_array const & borrowed() const noexcept { return borrowed_; }
    rank_groups<group_size> const & interleave() const noexcept { return interleave_; }
    std::span<std::byte const> false_borrow_bits() const noexcept { return false_borrows_; }
    std::uint64_t virtual_size() const noexcept { return virtual_count_; }
    profile_borrowed_policy borrowed_policy() const noexcept { return borrowed_policy_; }
    // Incremental construction binds the exact downstream pair. Legacy batch
    // build/reindex accept unbound sample spans and leave this empty.
    std::shared_ptr<profile_blob const> target() const noexcept { return target_; }
    std::uint64_t group_count() const noexcept {
      return virtual_count_ / group_size + (virtual_count_ % group_size != 0);
    }

    bool false_borrow(std::uint64_t ordinal) const {
      if (ordinal >= borrowed_.size()) throw std::out_of_range("profile blob borrowed ordinal");
      return (std::to_integer<unsigned>(false_borrows_[ordinal / 8]) >> (ordinal % 8)) & 1;
    }

    profile_blob_window project(std::uint64_t group) const {
      if (group >= group_count()) throw std::out_of_range("profile blob virtual group");
      auto first = group * group_size;
      auto last = first + std::min<std::uint64_t>(group_size, virtual_count_ - first);
      auto a = interleave_.view().rank(group);
      auto b = interleave_.view().rank(group + 1);
      return {first - a, last - b, a, b};
    }

    // The caller selects the group containing the last virtual key <= query,
    // with group zero used when query precedes the entire catalog. lower is
    // the group's known boundary key (empty context is sufficient at zero).
    // The result owns its value and outgoing prefix; it survives later calls.
    // Native equality does not suppress routing or resolve older value arrows.
    profile_blob_window_result<P> search_window(
        bit_view query, std::uint64_t group, profile_anchor<P> lower) const {
      if (query.size() % P::bits_per_unit) {
        throw std::invalid_argument("query length is not aligned to the profile unit");
      }
      auto query_units = query.size() / P::bits_per_unit;
      auto window = project(group);
      profile_blob_window_result<P> result;
      native_->view().visit_window(window.native_first, window.native_last, lower, query_units,
        [&](profile_item<P> item) {
          auto order = compare_profile_prefix(item.key, query);
          if (!order) {
            result.native = profile_blob_native_match<P>{item.ordinal, bit_string::copy(item.value)};
          }
          return order < 0;
        });

      auto consider_sample = [&](profile_item<P> item) {
        auto order = compare_profile_prefix(item.key, query);
        if (order > 0) return false;
        auto is_false = false_borrow(item.ordinal);
        result.borrowed_predecessor = profile_blob_borrowed_predecessor<P>{
          item.ordinal, checked_target_ordinal(item.ordinal), true,
          bit_string::copy(item.key.prefix), item.key.full_units, is_false};
        if (!order && is_false && !result.native) {
          if (!window.native_first) {
            throw std::invalid_argument("false borrow has no native predecessor");
          }
          // The native-before-borrowed tie order makes this the unique matching
          // native slot, even when many equal borrowed samples span group cuts.
          auto ordinal = window.native_first - 1;
          result.native = profile_blob_native_match<P>{
            ordinal, bit_string::copy(native_->view().encoded_at(ordinal).value)};
        }
        return true;
      };
      if (window.borrowed_first) {
        auto ordinal = window.borrowed_first - 1;
        if (borrowed_policy_ != profile_borrowed_policy::ordinary) {
          borrowed_.view().visit_window(ordinal, ordinal + 1, lower, query_units, consider_sample);
        } else {
          result.borrowed_predecessor = profile_blob_borrowed_predecessor<P>{
            ordinal, checked_target_ordinal(ordinal), false, {}, 0, false_borrow(ordinal)};
        }
      }
      borrowed_.view().visit_window(window.borrowed_first, window.borrowed_last, lower,
                                   query_units, consider_sample);
      return result;
    }

  private:
    friend struct index_builder<P>;

    std::shared_ptr<native_array const> native_ =
      std::make_shared<native_array const>(native_array::build({}));
    borrowed_array borrowed_ = borrowed_array::build({});
    rank_groups<group_size> interleave_ = rank_groups<group_size>::build({}, 0);
    std::vector<std::byte> false_borrows_;
    std::uint64_t virtual_count_ = 0;
    profile_borrowed_policy borrowed_policy_ = profile_borrowed_policy::shared_boundaries;
    std::shared_ptr<profile_blob const> target_;

    static void check_count(std::uint64_t native, std::uint64_t borrowed) {
      if (native > std::numeric_limits<std::uint64_t>::max() - borrowed) {
        throw std::length_error("profile blob virtual count overflows");
      }
    }

    static std::uint64_t checked_target_ordinal(std::uint64_t ordinal) {
      if (ordinal > std::numeric_limits<std::uint64_t>::max() / group_size) {
        throw std::overflow_error("borrowed target ordinal overflows");
      }
      return ordinal * group_size;
    }

    static void build_index(profile_blob & result, std::span<profile_record const> native,
                            std::span<bit_string const> borrowed,
                            std::span<std::uint64_t const> borrowed_prefix_ceilings,
                            profile_borrowed_policy policy) {
      if (!borrowed_prefix_ceilings.empty() && borrowed_prefix_ceilings.size() != borrowed.size()) {
        throw std::invalid_argument("one borrowed prefix ceiling is required per record");
      }
      if (policy != profile_borrowed_policy::ordinary &&
          policy != profile_borrowed_policy::bidirectional &&
          policy != profile_borrowed_policy::shared_boundaries) {
        throw std::invalid_argument("invalid profile borrowed prefix policy");
      }
      std::vector<std::uint64_t> ceilings;
      if (policy == profile_borrowed_policy::shared_boundaries) {
        ceilings.assign(borrowed.size(), std::numeric_limits<std::uint64_t>::max());
        if (!borrowed_prefix_ceilings.empty()) {
          std::copy(borrowed_prefix_ceilings.begin(), borrowed_prefix_ceilings.end(), ceilings.begin());
        }
        borrowed_prefix_ceilings = ceilings;
      } else if (policy == profile_borrowed_policy::bidirectional) {
        ceilings.reserve(borrowed.size());
        for (std::size_t i = 0; i != borrowed.size(); ++i) {
          auto ceiling = i + 1 == borrowed.size() ? 0 :
            common_prefix_units<P>(borrowed[i].view(), borrowed[i + 1].view());
          if (!borrowed_prefix_ceilings.empty()) {
            ceiling = std::min<std::uint64_t>(ceiling, borrowed_prefix_ceilings[i]);
          }
          ceilings.push_back(ceiling);
        }
        borrowed_prefix_ceilings = ceilings;
      }
      result.borrowed_policy_ = policy;
      std::vector<profile_record> borrowed_records;
      borrowed_records.reserve(borrowed.size());
      result.false_borrows_.resize(borrowed.size() / 8 + (borrowed.size() % 8 != 0));
      std::size_t native_at = 0;
      for (std::size_t i = 0; i != borrowed.size(); ++i) {
        if (i && compare_bits(borrowed[i - 1].view(), borrowed[i].view()) > 0) {
          throw std::invalid_argument("profile blob borrowed keys must be sorted");
        }
        while (native_at != native.size() &&
               compare_bits(native[native_at].key.view(), borrowed[i].view()) < 0) {
          ++native_at;
        }
        if (native_at != native.size() &&
            compare_bits(native[native_at].key.view(), borrowed[i].view()) == 0) {
          result.false_borrows_[i / 8] |= static_cast<std::byte>(1u << (i % 8));
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
        if (policy == profile_borrowed_policy::shared_boundaries && i % group_size == 0 && s) {
          auto const & boundary = take_borrowed ? borrowed[s] : native[a].key;
          ceilings[s - 1] = std::min<std::uint64_t>(ceilings[s - 1],
            common_prefix_units<P>(borrowed[s - 1].view(), boundary.view()));
        }
        if (take_borrowed) {
          ++classes[static_cast<std::size_t>(i / group_size)];
          ++s;
        } else {
          ++a;
        }
      }
      result.borrowed_ = borrowed_array::build(borrowed_records, borrowed_prefix_ceilings);
      result.interleave_ = rank_groups<group_size>::build(classes, count);
      result.virtual_count_ = count;
    }
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Everett's profile blob support.
 */
