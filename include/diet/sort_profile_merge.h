/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Merges sort-owned records while retaining inherited keys as borrowed spans.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <diet/sort_profile.h>
#include <diet/native_merge.h>

namespace diet {
  namespace sort_profile_detail {
    template <class View> struct merge_source {
      explicit merge_source(View view) : view_(view) {
        if (view_.size()) { frame_ = view_.encoded_at(0); retain(); }
      }
      bool done() const noexcept { return ordinal_ == view_.size(); }
      auto const & frame() const noexcept { return frame_; }
      std::span<bit_view const> spans() const noexcept { return spans_; }
      std::optional<bit_comparison> advance() {
        if (done()) throw std::out_of_range("sort merge source end");
        if (++ordinal_ == view_.size()) {
          if (frame_.next_offset != view_.data().size() || frame_.key_units != view_.metadata().terminal_key_units)
            throw std::invalid_argument("sort merge source terminal mismatch");
          return std::nullopt;
        }
        frame_ = view_.next(frame_);
        if (frame_.retained > length_) throw std::invalid_argument("sort merge missing inherited prefix");
        auto comparison = compare_spans(spans_, frame_.literal, frame_.retained);
        comparison.common_bits += frame_.retained;
        if (comparison.order >= 0) throw std::invalid_argument("sort merge input is not strictly ordered");
        retain(); return comparison;
      }
      bit_string materialize() const {
        bit_string result;
        for (auto span : spans_) profile_detail::append(result, span);
        return result;
      }
    private:
      View view_;
      sort_profile_frame frame_;
      std::vector<bit_view> spans_;
      std::uint64_t ordinal_ = 0, length_ = 0;
      void retain() {
        if (frame_.retained > length_) throw std::invalid_argument("sort merge requires an initial prefix");
        while (length_ > frame_.retained) {
          auto amount = std::min(spans_.back().size(), length_ - frame_.retained);
          auto keep = spans_.back().size() - amount; length_ -= amount;
          if (!keep) spans_.pop_back(); else spans_.back() = spans_.back().prefix(keep);
        }
        for (auto part : frame_.literal) if (part.size()) { spans_.push_back(part); length_ += part.size(); }
        if (length_ != frame_.key_units) throw std::invalid_argument("sort merge logical frame length");
      }
    };
  }

  template <class P, class Native = sort_profile_array<P>, class Compose = replace_native_value,
            class Selector = typename Native::stream_family::selector_type>
  struct sort_profile_merge_builder {
    using source_pointer = std::shared_ptr<Native const>;
    using view_type = decltype(std::declval<Native const &>().view());
    sort_profile_merge_builder(source_pointer older, source_pointer newer, Compose compose = {})
      : older_(checked(std::move(older))), newer_(checked(std::move(newer))),
        left_(older_->view()), right_(newer_->view()), compose_(std::move(compose)) {}
    bool done() const noexcept { return older_ && newer_ && !failed_ && left_.done() && right_.done(); }
    bool failed() const noexcept { return failed_; }
    native_merge_progress progress() const noexcept { return progress_; }
    std::uint64_t materialized_keys() const noexcept { return materialized_keys_; }
    native_merge_progress step(std::uint64_t budget = 1) {
      if (!older_ || !newer_ || failed_ || finished_) throw std::logic_error("inactive sort merge");
      native_merge_progress work;
      try {
        while (work.keys < budget && !done()) {
          auto comparison = compare();
          if (comparison.order < 0) {
            output_.append_frame(left_.frame(), left_.spans(), left_common_, left_.frame().value);
            right_common_ = comparison.common_bits; left_common_ = advance(left_);
          } else if (comparison.order > 0) {
            output_.append_frame(right_.frame(), right_.spans(), right_common_, right_.frame().value);
            left_common_ = comparison.common_bits; right_common_ = advance(right_);
          } else {
            bit_string key;
            auto value = [&] {
              if constexpr (!std::is_same_v<Compose, replace_native_value> && std::is_invocable_v<Compose &, bit_view, bit_view, bit_view>) {
                key = left_.materialize(); ++materialized_keys_;
                return std::invoke(compose_, key.view(), left_.frame().value, right_.frame().value);
              } else return std::invoke(compose_, left_.frame().value, right_.frame().value);
            }();
            if constexpr (std::is_same_v<decltype(value), bit_view>)
              output_.append_frame(left_.frame(), left_.spans(), left_common_, value);
            else output_.append_frame(left_.frame(), left_.spans(), left_common_, value.view());
            left_common_ = advance(left_); right_common_ = advance(right_);
          }
          ++work.keys; work.input_records += comparison.order ? 1 : 2;
        }
        progress_.keys += work.keys; progress_.input_records += work.input_records;
        return work;
      } catch (...) { failed_ = true; throw; }
    }
    sort_profile_array<P, Selector> finish() {
      if (!done() || finished_) throw std::logic_error("unfinished sort merge");
      try { auto result = output_.finish(); finished_ = true; return result; }
      catch (...) { failed_ = true; throw; }
    }
  private:
    source_pointer older_, newer_;
    sort_profile_detail::merge_source<view_type> left_, right_;
    sort_profile_writer<P, Selector> output_;
    Compose compose_;
    native_merge_progress progress_;
    std::uint64_t left_common_ = 0, right_common_ = 0, materialized_keys_ = 0;
    bool failed_ = false, finished_ = false;
    static source_pointer checked(source_pointer value) {
      if (!value) throw std::invalid_argument("null sort merge input"); return value;
    }
    static std::uint64_t advance(auto & source) { auto cmp = source.advance(); return cmp ? cmp->common_bits : 0; }
    bit_comparison compare() const {
      if (left_.done()) return {0, 1};
      if (right_.done()) return {0, -1};
      if (left_common_ != right_common_)
        return {std::min(left_common_, right_common_), left_common_ > right_common_ ? -1 : 1};
      auto cmp = sort_profile_detail::compare_spans(left_.spans(), right_.spans(), left_common_, right_common_);
      cmp.common_bits += left_common_; return cmp;
    }
  };
}
