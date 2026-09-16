/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Scans one sort in an immutable typed world with chronological resolution.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <everett/typed_world.h>
#include <iterator>
#include <ranges>

namespace everett {
  template <class S> struct typed_row {
    typed_detail::key_t<S> key;
    typed_detail::state_t<S> value;
  };

  // Native runs are oldest first. The heap orders equal keys by that age,
  // so noncommutative arrows are applied in exactly their original order.
  // The fractional cascade positions each native run at the lower bound.
  // It retains one reconstructed key per native run and at most one resolved
  // output row. Values continue borrowing native bytes.
  // Each step unit consumes one physical record; key bytes, callbacks and
  // heap comparisons are additional costs. Construction searches bounded
  // catalog windows and seeds each run from the lower-bound query. The captured
  // world keeps all mappings and cursors alive.
  template <class S, class World> struct typed_scan {
    using policy_type = typename World::policy_type;
    using row_type = typed_row<S>;
    using semantics = sort_semantics<S>;
    using native_type = typename World::runtime_family::native_type;
    using key_transport = typename World::key_transport;

    using key_type = typed_detail::key_t<S>;
    explicit typed_scan(World snapshot, std::optional<key_type> lo = {}, std::optional<key_type> hi = {},
        range_positioning_work * work = nullptr)
      : snapshot_(std::move(snapshot)), prefix_(key_transport::template prefix<S>()),
        lower_(lo ? std::optional{key_transport::template encode<S>(*lo)} : std::nullopt),
        upper_(hi ? std::optional{key_transport::template encode<S>(*hi)} : std::nullopt) {
      if (work) *work = {};
      if (lower_ && upper_ && compare_bits(lower_->view(), upper_->view()) > 0)
        throw std::invalid_argument("reversed typed range");
      if (lower_ && upper_ && compare_bits(lower_->view(), upper_->view()) == 0) { finished_ = true; return; }
      sweep_ = typed_detail::native_sweep<World>(snapshot_, lower_ ? lower_->view() : prefix_.view(), work);
      finished_ = sweep_.done();
    }
    typed_scan(typed_scan const &) = default;
    typed_scan & operator=(typed_scan const &) = default;
    typed_scan(typed_scan &&) = default;
    typed_scan & operator=(typed_scan &&) = default;

    // Dereferencing returns an owning row. Iterator copies share their walk
    // until one advances, then copy the per-run keys and cursor positions.
    // No reference escapes into an iterator's mutable FC reconstruction.
    struct iterator {
      using value_type = row_type;
      using difference_type = std::ptrdiff_t;
      using iterator_concept = std::forward_iterator_tag;
      using iterator_category = std::input_iterator_tag;
      iterator() = default;
      value_type operator*() const {
        if (!state_ || !state_->has_row()) throw std::out_of_range("typed range iterator end");
        return *state_->row_;
      }
      iterator & operator++() {
        if (!state_ || !state_->has_row()) throw std::out_of_range("typed range iterator end");
        if (state_.use_count() != 1) state_ = std::make_shared<typed_scan>(*state_);
        state_->row_.reset(); settle(); return *this;
      }
      iterator operator++(int) { auto previous = *this; ++*this; return previous; }
      friend bool operator==(iterator const & a, iterator const & b) {
        if (a.at_end() || b.at_end()) return a.at_end() && b.at_end();
        return a.same_position(b);
      }
      friend bool operator==(iterator const & value, std::default_sentinel_t) { return value.at_end(); }
    private:
      friend typed_scan;
      std::shared_ptr<typed_scan> state_;
      explicit iterator(typed_scan const & value) : state_(std::make_shared<typed_scan>(value)) { settle(); }
      bool at_end() const { return !state_ || state_->done(); }
      bool same_position(iterator const & other) const {
        return state_->identity_ == other.state_->identity_ && state_->consumed() == other.state_->consumed();
      }
      void settle() { while (!state_->has_row() && !state_->done()) state_->step(256); }
    };
    iterator begin() const {
      if (failed_) throw std::logic_error("failed typed scan");
      return iterator(*this);
    }
    std::default_sentinel_t end() const noexcept { return {}; }

    bool done() const noexcept { return finished_ && !row_; }
    bool has_row() const noexcept { return row_.has_value(); }
    bool failed() const noexcept { return failed_; }
    std::uint64_t consumed() const noexcept { return sweep_.consumed(); }
    row_type take_row() {
      if (failed_) throw std::logic_error("failed typed scan");
      if (!row_) throw std::logic_error("typed scan has no row");
      auto result = std::move(*row_); row_.reset(); return result;
    }
    std::optional<row_type> next() {
      if (failed_) throw std::logic_error("failed typed scan");
      while (!has_row() && !done()) step(256);
      return has_row() ? std::optional<row_type>{take_row()} : std::nullopt;
    }
    std::uint64_t step(std::uint64_t budget) {
      if (failed_) throw std::logic_error("failed typed scan");
      if (!budget || done() || has_row()) return 0;
      std::uint64_t used = 0;
      try {
        while (used != budget && !finished_ && !row_) {
          if (sweep_.done()) { finish_group(); finished_ = true; break; }
          auto current = sweep_.peek();
          if (group_ && compare_bits(group_key_.view(), current.key.prefix) != 0) {
            finish_group();
            if (row_) break;
          }
          if (!group_) {
            if (upper_ && compare_bits(current.key.prefix, upper_->view()) >= 0) { finished_ = true; break; }
            if (lower_ && compare_bits(current.key.prefix, lower_->view()) < 0) { sweep_.consume(); ++used; continue; }
            auto order = compare_prefix(current.key.prefix);
            if (order > 0) { finished_ = true; break; }
            if (order < 0) { sweep_.consume(); ++used; continue; }
            group_key_ = bit_string::copy(current.key.prefix);
            auto key = key_transport::template decode<S>(group_key_.view().subview(prefix_.bit_size,
              group_key_.bit_size - prefix_.bit_size));
            auto value = semantics::initial(key);
            group_.emplace(row_type{std::move(key), std::move(value)});
          }
          if constexpr (typed_detail::replacement<S>) {
            last_value_ = current.value;
            last_retained_ = sweep_.retained_bits();
          }
          else group_->value = semantics::apply(group_->key, std::move(group_->value),
            typed_detail::value<policy_type, S>(current.value));
          sweep_.consume(); ++used;
          if (sweep_.done() || compare_bits(group_key_.view(), sweep_.peek().key.prefix) != 0)
            finish_group();
          if (sweep_.done()) finished_ = true;
        }
      } catch (...) { failed_ = true; throw; }
      return used;
    }

    // Delete exactly the rows this cursor has not yet returned. The private
    // observations carry old arrows, not hash-only evidence of old values.
    auto erase_remaining() requires typed_detail::replacement<S> {
      if (failed_) throw std::logic_error("failed typed scan");
      try {
        typename World::contribution_type result{snapshot_, {}};
        result.observed_.emplace();
        while (!done()) {
          while (!has_row() && !done()) step(256);
          if (!has_row()) break;
          result.records_.push_back({bit_string::copy(group_key_.view()),
            typed_detail::value<policy_type, S>(semantics::erase(row_->key)), last_retained_});
          result.observed_->push_back(last_value_);
          row_.reset();
        }
        return result;
      } catch (...) { failed_ = true; throw; }
    }

  private:
    std::shared_ptr<void const> identity_ = std::make_shared<int const>(0);
    World snapshot_;
    bit_string prefix_, group_key_;
    std::optional<bit_string> lower_, upper_;
    typed_detail::native_sweep<World> sweep_;
    std::optional<row_type> group_, row_;
    bit_view last_value_;
    std::uint64_t last_retained_ = 0;
    bool finished_ = false, failed_ = false;

    int compare_prefix(bit_view key) const {
      auto count = std::min(key.size(), prefix_.bit_size);
      auto order = compare_bits(key.subview(0, count), prefix_.view().subview(0, count));
      return order ? order : key.size() < prefix_.bit_size ? -1 : 0;
    }
    void finish_group() {
      if (!group_) return;
      if constexpr (typed_detail::replacement<S>)
        group_->value = semantics::apply(group_->key, std::move(group_->value),
          typed_detail::value<policy_type, S>(last_value_));
      if (semantics::present(group_->key, group_->value)) row_.emplace(std::move(*group_));
      group_.reset();
    }
  };

  // Bounds follow the sort's encoded ordering. Omitted endpoints are open.
  // The fractional cascade finds the native starts; cursors remain open thereafter.
  template <class S = void, class World> auto range(World snapshot,
      std::optional<typed_detail::key_t<std::conditional_t<std::is_void_v<S>,
        typed_detail::default_sort_t<typename World::policy_type>, S>>> lo = {},
      std::optional<typed_detail::key_t<std::conditional_t<std::is_void_v<S>,
        typed_detail::default_sort_t<typename World::policy_type>, S>>> hi = {},
      range_positioning_work * work = nullptr) {
    using selected = std::conditional_t<std::is_void_v<S>, typed_detail::default_sort_t<typename World::policy_type>, S>;
    return typed_scan<selected, World>(std::move(snapshot), std::move(lo), std::move(hi), work);
  }
  template <class S = void, class World> auto erase_range(World snapshot,
      std::optional<typed_detail::key_t<std::conditional_t<std::is_void_v<S>,
        typed_detail::default_sort_t<typename World::policy_type>, S>>> lo = {},
      std::optional<typed_detail::key_t<std::conditional_t<std::is_void_v<S>,
        typed_detail::default_sort_t<typename World::policy_type>, S>>> hi = {})
    requires typed_detail::replacement<std::conditional_t<std::is_void_v<S>,
      typed_detail::default_sort_t<typename World::policy_type>, S>> {
    return range<S>(std::move(snapshot), std::move(lo), std::move(hi)).erase_remaining();
  }
  template <class S = void, class World> auto scan(World snapshot) {
    using selected = std::conditional_t<std::is_void_v<S>, typed_detail::default_sort_t<typename World::policy_type>, S>;
    return typed_scan<selected, World>(std::move(snapshot));
  }
}

// Standard customization: iterators retain their own snapshot and walk state.
/// \cond
namespace std::ranges {
  template <class S, class World>
  inline constexpr bool enable_borrowed_range<everett::typed_scan<S, World>> = true;
}
/// \endcond
