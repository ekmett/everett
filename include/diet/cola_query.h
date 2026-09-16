/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Enumerates native matches along a COLA main chain and its terminal secondaries.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <diet/cola_index.h>

#include <array>
#include <memory>
#include <optional>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace diet {
  template <class P, class Blob = cola_index<P>> struct cola_query_cursor;

  // Source pins the main node that yielded this match. A secondary match
  // belongs to source->secondary_target(), not source's own native array.
  // Traversal order does not define chronology or composition precedence.
  template <class P, class Blob = cola_index<P>> struct cola_query_match {
    std::shared_ptr<Blob const> source;
    bool secondary = false;
    std::uint64_t ordinal = 0;
    bit_string value;
  };

  template <class P, class Blob = cola_index<P>> struct cola_query_root {
    using policy_type = P;
    using pair_type = std::shared_ptr<Blob const>;
    static cola_query_root build(pair_type main = {}, typename Blob::native_pointer secondary = {})
        requires std::is_same_v<Blob, cola_index<P>> {
      return adopt_prepared(Blob::prepare_root(std::move(main), std::move(secondary)));
    }
    static cola_query_root adopt_prepared(pair_type source) {
      if (!source || source->virtual_size() > P::group_size)
        error_detail::raise<std::invalid_argument>("COLA query root needs one bounded head group");
      std::unordered_set<Blob const *> seen;
      for (auto current = source; current; current = current->main_target()) {
        if (!seen.insert(current.get()).second) error_detail::raise<std::invalid_argument>("cyclic COLA main chain");
        auto view = current->view();
        auto main = current->main_target();
        auto secondary = current->secondary_target();
        auto secondary_count = secondary ? secondary->view().size() : 0;
        if (view.borrowed(0).size() != (main ? main->group_count() : 0) ||
            view.borrowed(1).size() != secondary_count / P::group_size + (secondary_count % P::group_size != 0))
          error_detail::raise<std::invalid_argument>("COLA query target sample count mismatch");
      }
      return cola_query_root(std::move(source));
    }
    pair_type head() const noexcept { return head_; }
    cola_query_cursor<P, Blob> cursor(bit_view query) const { return cola_query_cursor<P, Blob>(*this, query); }
    cola_query_cursor<P, Blob> cursor_owned(bit_string query) const {
      return cola_query_cursor<P, Blob>::from_owned(*this, std::move(query));
    }
  private:
    explicit cola_query_root(pair_type head) : head_(std::move(head)) {}
    pair_type head_;
  };

  // Each charged main-node visit searches at most one augmented K-window and
  // one terminal native K-window. Two equal native matches may result: neither
  // may be discarded. Pending matches retain their source and own their values.
  template <class P, class Blob> struct cola_query_cursor {
    using policy_type = P;
    using pair_type = std::shared_ptr<Blob const>;
    using match_type = cola_query_match<P, Blob>;
    explicit cola_query_cursor(cola_query_root<P, Blob> const & root, bit_view query)
      : cola_query_cursor(root, profile_query_context<P>(query), prepared_query{}) {}
    static cola_query_cursor from_owned(cola_query_root<P, Blob> const & root, bit_string query) {
      return cola_query_cursor(root, profile_query_context<P>::from_owned(std::move(query)), prepared_query{});
    }
    cola_query_cursor(cola_query_cursor const &) = default;
    cola_query_cursor & operator=(cola_query_cursor const &) = default;
    cola_query_cursor(cola_query_cursor && other) noexcept
      : current_(std::move(other.current_)), context_(std::move(other.context_)), group_(other.group_),
        pending_(std::move(other.pending_)), failed_(other.failed_) { other.pending_ = {}; }
    cola_query_cursor & operator=(cola_query_cursor && other) noexcept {
      if (this != &other) {
        current_ = std::move(other.current_); context_ = std::move(other.context_); group_ = other.group_;
        pending_ = std::move(other.pending_); failed_ = other.failed_; other.pending_ = {};
      }
      return *this;
    }
    bool has_match() const noexcept { return pending_[0].has_value() || pending_[1].has_value(); }
    bool done() const noexcept { return !current_ && !has_match(); }
    bool failed() const noexcept { return failed_; }
    std::uint64_t step(std::uint64_t main_budget = 1) {
      if (failed_) error_detail::raise<std::logic_error>("COLA query cursor has failed");
      if (has_match()) return 0;
      std::uint64_t visited = 0;
      try {
        while (current_ && visited < main_budget) {
          auto result = current_->view().search_window(group_, context_);
          auto main = current_->main_target();
          auto secondary = current_->secondary_target();
          if (auto const & next = result.predecessors[0])
            if (!main || next->target_ordinal % P::group_size || next->target_ordinal >= main->virtual_size())
              error_detail::raise<std::invalid_argument>("COLA query main route has no target");
          std::optional<profile_blob_native_match<P>> side;
          if (auto const & next = result.predecessors[1]) {
            if (!secondary) error_detail::raise<std::invalid_argument>("COLA query secondary route has no target");
            side = cola_search_secondary<P>(secondary->view(), *next);
          }
          if (result.native) pending_[0].emplace(match_type{current_, false,
            result.native->ordinal, std::move(result.native->value)});
          if (side) pending_[1].emplace(match_type{current_, true, side->ordinal, std::move(side->value)});
          if (auto & next = result.predecessors[0]) {
            group_ = next->target_ordinal / P::group_size;
            context_ = std::move(next->comparison);
            current_ = std::move(main);
          } else current_.reset();
          ++visited;
          if (has_match()) break;
        }
      } catch (...) { failed_ = true; throw; }
      return visited;
    }
    match_type take_match() {
      if (failed_) error_detail::raise<std::logic_error>("COLA query cursor has failed");
      auto & slot = pending_[pending_[0] ? 0 : 1];
      if (!slot) error_detail::raise<std::logic_error>("COLA query cursor has no match");
      auto result = std::move(*slot);
      slot.reset();
      return result;
    }
  private:
    struct prepared_query {};
    cola_query_cursor(cola_query_root<P, Blob> const & root, profile_query_context<P> context, prepared_query)
      : current_(root.head()), context_(std::move(context)) {
      if (!current_) error_detail::raise<std::invalid_argument>("COLA query root has no head");
      if (!current_->virtual_size()) current_.reset();
    }
    pair_type current_;
    profile_query_context<P> context_;
    std::uint64_t group_ = 0;
    std::array<std::optional<match_type>, 2> pending_;
    bool failed_ = false;
  };
}
