/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Scans one sort in an immutable typed cola with chronological resolution.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <diet/typed_cola.h>

namespace diet {
  template <class S> struct typed_row {
    typed_detail::key_t<S> key;
    typed_detail::state_t<S> value;
  };

  // Native runs are oldest first. The heap orders equal keys by that age,
  // so noncommutative arrows are applied in exactly their original order.
  // This is a full scan, including earlier sorts before reaching S, not an
  // indexed range seek. It retains one reconstructed key per native run and
  // at most one resolved output row. Values continue borrowing native bytes.
  // Each step unit consumes one physical record; key bytes, callbacks and
  // heap comparisons are additional costs. Construction decodes each run's
  // first key. The captured cola keeps all mappings and cursors alive.
  template <class S, class Cola> struct typed_scan {
    using policy_type = typename Cola::policy_type;
    using row_type = typed_row<S>;
    using semantics = sort_semantics<S>;
    using native_type = typename Cola::runtime_family::native_type;
    using key_transport = typename Cola::key_transport;

    explicit typed_scan(Cola snapshot) : snapshot_(std::move(snapshot)) {
      prefix_ = key_transport::template prefix<S>();
      auto runs = snapshot_.runtime().runs();
      sources_.reserve(runs.size()); heap_.reserve(runs.size());
      for (auto const & run : runs) {
        auto native = [&] {
          if constexpr (requires { run.native_owner(); }) return run.native_owner();
          else return run->native;
        }();
        sources_.emplace_back(std::move(native));
        if (!sources_.back().cursor.done()) heap_.push_back(sources_.size() - 1);
      }
      std::make_heap(heap_.begin(), heap_.end(), later());
      finished_ = heap_.empty();
    }
    typed_scan(typed_scan const &) = delete;
    typed_scan & operator=(typed_scan const &) = delete;
    typed_scan(typed_scan &&) = default;
    typed_scan & operator=(typed_scan &&) = delete;

    bool done() const noexcept { return finished_ && !row_; }
    bool has_row() const noexcept { return row_.has_value(); }
    bool failed() const noexcept { return failed_; }
    std::uint64_t consumed() const noexcept { return consumed_; }
    row_type take_row() {
      if (!row_) throw std::logic_error("typed scan has no row");
      auto result = std::move(*row_); row_.reset(); return result;
    }
    std::optional<row_type> next() {
      while (!has_row() && !done()) step(256);
      return has_row() ? std::optional<row_type>{take_row()} : std::nullopt;
    }
    std::uint64_t step(std::uint64_t budget) {
      if (failed_) throw std::logic_error("failed typed scan");
      if (!budget || done() || has_row()) return 0;
      std::uint64_t used = 0;
      try {
        while (used != budget && !finished_ && !row_) {
          if (heap_.empty()) { finish_group(); finished_ = true; break; }
          auto current = sources_[heap_.front()].cursor.peek();
          if (group_ && compare_bits(group_key_.view(), current.key.prefix) != 0) {
            finish_group();
            if (row_) break;
          }
          if (!group_) {
            auto order = compare_prefix(current.key.prefix);
            if (order > 0) { finished_ = true; break; }
            if (order < 0) { consume(); ++used; continue; }
            group_key_ = bit_string::copy(current.key.prefix);
            auto key = key_transport::template decode<S>(group_key_.view().subview(prefix_.bit_size,
              group_key_.bit_size - prefix_.bit_size));
            auto value = semantics::initial(key);
            group_.emplace(row_type{std::move(key), std::move(value)});
          }
          if constexpr (typed_detail::replacement<S>) last_value_ = current.value;
          else group_->value = semantics::apply(group_->key, std::move(group_->value),
            typed_detail::value<policy_type, S>(current.value));
          consume(); ++used;
          if (heap_.empty() || compare_bits(group_key_.view(), sources_[heap_.front()].cursor.peek().key.prefix) != 0)
            finish_group();
          if (heap_.empty()) finished_ = true;
        }
      } catch (...) { failed_ = true; throw; }
      return used;
    }

  private:
    struct source {
      std::shared_ptr<native_type const> native;
      decltype(std::declval<native_type const &>().view().cursor()) cursor;
      explicit source(std::shared_ptr<native_type const> value)
        : native(std::move(value)), cursor(native->view()) {}
    };
    Cola snapshot_;
    bit_string prefix_, group_key_;
    std::vector<source> sources_;
    std::vector<std::size_t> heap_;
    std::optional<row_type> group_, row_;
    bit_view last_value_;
    std::uint64_t consumed_ = 0;
    bool finished_ = false, failed_ = false;

    auto later() const {
      return [this](std::size_t a, std::size_t b) {
        auto order = compare_bits(sources_[a].cursor.peek().key.prefix, sources_[b].cursor.peek().key.prefix);
        return order ? order > 0 : a > b;
      };
    }
    int compare_prefix(bit_view key) const {
      auto count = std::min(key.size(), prefix_.bit_size);
      auto order = compare_bits(key.subview(0, count), prefix_.view().subview(0, count));
      return order ? order : key.size() < prefix_.bit_size ? -1 : 0;
    }
    void consume() {
      std::pop_heap(heap_.begin(), heap_.end(), later());
      auto which = heap_.back(); heap_.pop_back();
      auto comparison = sources_[which].cursor.advance_comparison();
      if (comparison && comparison->order >= 0)
        throw std::invalid_argument("scanned native keys are not unique and sorted");
      ++consumed_;
      if (!sources_[which].cursor.done()) {
        heap_.push_back(which); std::push_heap(heap_.begin(), heap_.end(), later());
      }
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

  template <class S = void, class Cola> auto scan(Cola snapshot) {
    using selected = std::conditional_t<std::is_void_v<S>, typed_detail::default_sort_t<typename Cola::policy_type>, S>;
    return typed_scan<selected, Cola>(std::move(snapshot));
  }
}
