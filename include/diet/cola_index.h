/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares dual-target main/secondary fractional indexes for COLA.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <diet/native_writer.h>
#include <diet/profile_blob.h>
#include <diet/sampling.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace diet {
  enum class cola_target : unsigned { main = 0, secondary = 1 };
  enum class cola_origin : unsigned { native = 0, main = 1, secondary = 2 };
  namespace cola_detail {
    inline unsigned route(unsigned value) {
      if (value >= 2) error_detail::raise<std::out_of_range>("COLA target route");
      return value;
    }
    inline std::uint64_t target_ordinal(std::uint64_t ordinal, std::uint64_t stride) {
      if (ordinal > std::numeric_limits<std::uint64_t>::max() / stride)
        error_detail::raise<std::overflow_error>("COLA target ordinal overflow");
      return ordinal * stride;
    }
    struct frontier_selection {
      unsigned origin = 3;
      std::array<std::uint64_t, 3> common{};
    };
    // Every live head follows the same preceding occurrence. Larger LCPs
    // sort first; equal LCPs need only a suffix comparison. Stable origin order
    // preserves native, main-borrow, secondary-borrow ties.
    template <class Live, class Head> frontier_selection choose_frontier(
        std::array<std::uint64_t, 3> const & prefixes, Live live, Head head) {
      frontier_selection result;
      bit_view key;
      for (unsigned origin = 0; origin < 3; ++origin) if (live(origin)) {
        auto candidate = head(origin);
        if (result.origin == 3) {
          result.origin = origin; key = candidate; result.common[origin] = key.size();
          continue;
        }
        auto first = prefixes[origin], second = prefixes[result.origin];
        bit_comparison comparison;
        if (first != second) comparison = {std::min(first, second), first > second ? -1 : 1};
        else {
          comparison = compare_common_bits(candidate.subview(first, candidate.size() - first),
            key.subview(first, key.size() - first));
          comparison.common_bits += first;
        }
        if (comparison.order < 0) {
          // New winner <= old winner <= each old loser: ordered LCP minimum.
          for (unsigned i = 0; i < origin; ++i)
            result.common[i] = std::min(result.common[i], comparison.common_bits);
          result.origin = origin; key = candidate;
          result.common[origin] = key.size();
        } else result.common[origin] = comparison.common_bits;
      }
      if (result.origin == 3) error_detail::raise<std::invalid_argument>("COLA frontier has no live stream");
      return result;
    }

  }

  struct cola_window {
    std::uint64_t native_first = 0, native_last = 0;
    std::array<std::uint64_t, 2> borrowed_first{}, borrowed_last{};
  };
  template <class P> struct cola_window_result {
    std::optional<profile_blob_native_match<P>> native;
    std::array<std::optional<profile_blob_borrowed_predecessor<P>>, 2> predecessors;
  };

  template <class P> struct profile_stream_family {
    using native_view = profile_view<P, stream_role::native>;
    using borrowed_view = profile_view<P, stream_role::borrowed>;
    using borrowed_array = profile_array<P, stream_role::borrowed>;
    using borrowed_writer = profile_borrowed_writer<P>;
  };
  template <class P, class Native, class = void> struct cola_stream_family { using type = profile_stream_family<P>; };
  template <class P, class Native> struct cola_stream_family<P, Native, std::void_t<typename Native::stream_family>> {
    using type = typename Native::stream_family;
  };

  // Both population directories refer to the same three-way augmented order.
  // Native keys win ties, followed by main borrows, then secondary borrows.
  // Shapes are checked without scanning payload. Contents, flags and cut LCPs
  // must come from this builder or an independently validated reader.
  template <class P, class Family = profile_stream_family<P>> struct cola_index_view {
    using policy_type = P;
    using native_view = typename Family::native_view;
    using borrowed_view = typename Family::borrowed_view;
    using rank_view = rank_groups_view<P::group_size>;
    static constexpr auto group_size = P::group_size;
    cola_index_view(native_view native, std::array<borrowed_view, 2> borrowed,
        std::array<rank_view, 2> ranks, std::array<std::span<std::byte const>, 2> flags,
        std::array<word_view, 2> cuts, std::uint64_t count)
      : native_(native), borrowed_(borrowed), ranks_(ranks), flags_(flags), cuts_(cuts), count_(count) {
      auto remaining = std::numeric_limits<std::uint64_t>::max() - native.size();
      if (borrowed[0].size() > remaining || borrowed[1].size() > remaining - borrowed[0].size() ||
          native.size() + borrowed[0].size() + borrowed[1].size() != count)
        error_detail::raise<std::invalid_argument>("COLA augmented count mismatch");
      for (unsigned route = 0; route < 2; ++route) {
        auto size = borrowed[route].size();
        if (size > std::numeric_limits<std::uint64_t>::max() - 7 || ranks[route].size() != count ||
            flags[route].size() != ((size + 7) >> 3) || cuts[route].size() != group_count())
          error_detail::raise<std::invalid_argument>("COLA section shape mismatch");
      }
    }
    native_view const & native() const noexcept { return native_; }
    borrowed_view const & borrowed(unsigned route) const { return borrowed_[cola_detail::route(route)]; }
    borrowed_view const & borrowed(cola_target route) const { return borrowed(unsigned(route)); }
    rank_view const & interleave(unsigned route) const { return ranks_[cola_detail::route(route)]; }
    rank_view const & interleave(cola_target route) const { return interleave(unsigned(route)); }
    std::span<std::byte const> false_borrow_bits(unsigned route) const { return flags_[cola_detail::route(route)]; }
    std::span<std::byte const> false_borrow_bits(cola_target route) const { return false_borrow_bits(unsigned(route)); }
    word_view cut_lcps(unsigned route) const { return cuts_[cola_detail::route(route)]; }
    word_view cut_lcps(cola_target route) const { return cut_lcps(unsigned(route)); }
    std::uint64_t virtual_size() const noexcept { return count_; }
    std::uint64_t group_count() const noexcept { return count_ / group_size + (count_ % group_size != 0); }
    bool false_borrow(unsigned route, std::uint64_t ordinal) const {
      route = cola_detail::route(route);
      if (ordinal >= borrowed_[route].size()) error_detail::raise<std::out_of_range>("COLA borrowed ordinal");
      return (std::to_integer<unsigned>(flags_[route][ordinal >> 3]) >> (ordinal & 7)) & 1;
    }
    bool false_borrow(cola_target route, std::uint64_t ordinal) const { return false_borrow(unsigned(route), ordinal); }
    cola_window project(std::uint64_t group) const {
      if (group >= group_count()) error_detail::raise<std::out_of_range>("COLA virtual group");
      auto first = group * group_size;
      auto width = std::min<std::uint64_t>(group_size, count_ - first);
      std::array<std::uint64_t, 2> prefix{}, population{};
      for (unsigned route = 0; route < 2; ++route) {
        prefix[route] = ranks_[route].rank(group);
        population[route] = ranks_[route].class_at(group);
        if (prefix[route] > first || prefix[route] > borrowed_[route].size() ||
            population[route] > width || population[route] > borrowed_[route].size() - prefix[route])
          error_detail::raise<std::invalid_argument>("invalid COLA rank projection");
      }
      if (prefix[0] > first - prefix[1] || population[0] > width - population[1])
        error_detail::raise<std::invalid_argument>("overlapping COLA population directories");
      auto native_first = first - prefix[0] - prefix[1];
      auto native_last = native_first + width - population[0] - population[1];
      if (native_last > native_.size()) error_detail::raise<std::invalid_argument>("invalid COLA native projection");
      return {native_first, native_last, prefix,
        {prefix[0] + population[0], prefix[1] + population[1]}};
    }
    cola_window_result<P> search_window(std::uint64_t group, profile_query_context<P> const & lower,
        profile_comparison_work * native_work = nullptr,
        std::array<profile_comparison_work *, 2> borrowed_work = {}) const {
      if (lower.order() > 0) error_detail::raise<std::invalid_argument>("query precedes COLA boundary");
      auto window = project(group);
      cola_window_result<P> result;
      native_.compare_window(window.native_first, window.native_last, lower,
        [&](profile_comparison_item<P> item) {
          if (!item.comparison.order())
            result.native = profile_blob_native_match<P>{item.ordinal, bit_string::copy(item.value)};
          return item.comparison.order() < 0;
        }, native_work);
      for (unsigned route = 0; route < 2; ++route) {
        auto remember = [&](std::uint64_t ordinal, profile_query_context<P> const & comparison) {
          auto is_false = false_borrow(route, ordinal);
          result.predecessors[route] = profile_blob_borrowed_predecessor<P>{ordinal,
            cola_detail::target_ordinal(ordinal, group_size), is_false, comparison};
          if (!comparison.order() && is_false && !result.native) {
            if (!window.native_first) error_detail::raise<std::invalid_argument>("COLA false borrow has no native predecessor");
            auto native_ordinal = window.native_first - 1;
            result.native = profile_blob_native_match<P>{native_ordinal,
              bit_string::copy(native_.encoded_at(native_ordinal).value)};
          }
        };
        borrowed_[route].compare_window(window.borrowed_first[route], window.borrowed_last[route], lower,
          [&](profile_comparison_item<P> item) {
            if (item.comparison.order() > 0) return false;
            remember(item.ordinal, item.comparison);
            return true;
          }, borrowed_work[route]);
        if (!result.predecessors[route] && window.borrowed_first[route])
          remember(window.borrowed_first[route] - 1, lower.predecessor(cuts_[route][group]));
      }
      return result;
    }
  private:
    native_view native_;
    std::array<borrowed_view, 2> borrowed_;
    std::array<rank_view, 2> ranks_;
    std::array<std::span<std::byte const>, 2> flags_;
    std::array<word_view, 2> cuts_;
    std::uint64_t count_;
  };

  // A secondary is a native-only leaf. Its sampled ordinal directly names a
  // native K-window; it has no onward index route.
  template <class P, class View> std::optional<profile_blob_native_match<P>> cola_search_secondary(
      View leaf, profile_blob_borrowed_predecessor<P> const & predecessor,
      profile_comparison_work * work = nullptr) {
    auto first = predecessor.target_ordinal;
    if (first >= leaf.size() || first % P::group_size || predecessor.comparison.order() > 0)
      error_detail::raise<std::invalid_argument>("invalid COLA secondary route");
    auto last = first + std::min<std::uint64_t>(P::group_size, leaf.size() - first);
    std::optional<profile_blob_native_match<P>> result;
    leaf.compare_window(first, last, predecessor.comparison, [&](profile_comparison_item<P> item) {
      if (!item.comparison.order()) result = profile_blob_native_match<P>{item.ordinal, bit_string::copy(item.value)};
      return item.comparison.order() < 0;
    }, work);
    return result;
  }

  template <class P, class Native = profile_array<P>, class Main = void> struct cola_index;
  template <class P, class Target = cola_index<P>> struct cola_sample_cursor;
  namespace cola_detail {
    template <class P, class Native, class Main> struct index_output;
    template <class P> struct index_metadata {
      std::array<rank_groups<P::group_size>, 2> ranks;
      std::array<std::vector<std::byte>, 2> flags;
      std::array<std::vector<std::uint64_t>, 2> cuts;
      std::uint64_t count = 0;
    };
  }
  template <class P, class Native = profile_array<P>, class Main = void,
            class Output = cola_detail::index_output<P, Native, Main>> struct cola_index_builder;
  template <class P> struct cola_sample_view {
    bit_view key;
    std::uint64_t target_ordinal = 0;
    cola_origin origin = cola_origin::native;
    std::uint64_t source_ordinal = 0;
  };

  // Immutable main node with one recursive main edge and one native-only leaf.
  // This is a fractional-index representation, not a scheduler or chronology.
  // Default parameters own native arrays and a homogeneous main chain. A
  // supplied Native/Main pair instead yields a construction artifact retaining
  // those exact owners; its borrowed payload/navigation are newly built output.
  // Persist such a mixed artifact before adopting a homogeneous mapped query.
  template <class P, class Native, class Main> struct cola_index {
    using policy_type = P;
    using native_array = Native;
    using target_type = std::conditional_t<std::is_void_v<Main>, cola_index, Main>;
    using stream_family = typename cola_stream_family<P, Native>::type;
    using view_type = cola_index_view<P, stream_family>;
    using borrowed_array = typename stream_family::borrowed_array;
    using native_pointer = std::shared_ptr<native_array const>;
    using pair_type = std::shared_ptr<cola_index const>;
    using main_pointer = std::shared_ptr<target_type const>;
    static constexpr auto group_size = P::group_size;
    static cola_index build(std::span<profile_record const> records, main_pointer main = {}, native_pointer secondary = {})
      requires (std::is_same_v<Native, profile_array<P>> && std::is_void_v<Main>);
    // The adopted array must be immutable, ordinary-FC and strictly sorted.
    static cola_index adopt_native(native_pointer native, main_pointer main = {}, native_pointer secondary = {});
    // Include both level-zero arrays, then add empty routing nodes until the
    // first augmented group suffices. Neither handle may be silently dropped.
    static pair_type prepare_root(pair_type main = {}, native_pointer secondary = {})
      requires (std::is_same_v<Native, profile_array<P>> && std::is_void_v<Main>);
    cola_index(cola_index const &) = delete;
    cola_index & operator=(cola_index const &) = delete;
    cola_index(cola_index &&) = default;
    cola_index & operator=(cola_index &&) = default;
    native_pointer native_owner() const noexcept { return native_; }
    native_array const & native() const & { require_active(); return *native_; }
    native_array const & native() const && = delete;
    borrowed_array const & borrowed(unsigned route) const & { require_active(); return borrowed_[cola_detail::route(route)]; }
    borrowed_array const & borrowed(unsigned) const && = delete;
    borrowed_array const & borrowed(cola_target route) const & { return borrowed(unsigned(route)); }
    borrowed_array const & borrowed(cola_target) const && = delete;
    rank_groups<group_size> const & interleave(unsigned route) const & { require_active(); return ranks_[cola_detail::route(route)]; }
    rank_groups<group_size> const & interleave(unsigned) const && = delete;
    rank_groups<group_size> const & interleave(cola_target route) const & { return interleave(unsigned(route)); }
    rank_groups<group_size> const & interleave(cola_target) const && = delete;
    std::span<std::byte const> false_borrow_bits(unsigned route) const & { require_active(); return flags_[cola_detail::route(route)]; }
    std::span<std::byte const> false_borrow_bits(unsigned) const && = delete;
    std::span<std::byte const> false_borrow_bits(cola_target route) const & { return false_borrow_bits(unsigned(route)); }
    std::span<std::byte const> false_borrow_bits(cola_target) const && = delete;
    std::span<std::uint64_t const> cut_lcps(unsigned route) const & { require_active(); return cuts_[cola_detail::route(route)]; }
    std::span<std::uint64_t const> cut_lcps(unsigned) const && = delete;
    std::span<std::uint64_t const> cut_lcps(cola_target route) const & { return cut_lcps(unsigned(route)); }
    std::span<std::uint64_t const> cut_lcps(cola_target) const && = delete;
    main_pointer main_target() const noexcept { return main_; }
    native_pointer secondary_target() const noexcept { return secondary_; }
    std::uint64_t virtual_size() const noexcept { return native_ ? count_ : 0; }
    std::uint64_t group_count() const noexcept { auto n = virtual_size(); return n / group_size + (n % group_size != 0); }
    view_type view() const & {
      require_active();
      return {native_->view(), {borrowed_[0].view(), borrowed_[1].view()}, {ranks_[0].view(), ranks_[1].view()},
        {flags_[0], flags_[1]}, {word_view(std::span<std::uint64_t const>(cuts_[0])),
          word_view(std::span<std::uint64_t const>(cuts_[1]))}, count_};
    }
    view_type view() const && = delete;
    cola_window project(std::uint64_t group) const { return view().project(group); }
    cola_window_result<P> search_window(std::uint64_t group, profile_query_context<P> const & lower,
        profile_comparison_work * native_work = nullptr,
        std::array<profile_comparison_work *, 2> borrowed_work = {}) const {
      return view().search_window(group, lower, native_work, borrowed_work);
    }
  private:
    friend struct cola_detail::index_output<P, Native, Main>;
    native_pointer native_;
    main_pointer main_;
    native_pointer secondary_;
    std::array<borrowed_array, 2> borrowed_;
    std::array<rank_groups<group_size>, 2> ranks_;
    std::array<std::vector<std::byte>, 2> flags_;
    std::array<std::vector<std::uint64_t>, 2> cuts_;
    std::uint64_t count_;
    void require_active() const {
      if (!native_) error_detail::raise<std::logic_error>("moved-from COLA index");
    }
    cola_index(native_pointer native, main_pointer main, native_pointer secondary,
        std::array<borrowed_array, 2> borrowed, std::array<rank_groups<group_size>, 2> ranks,
        std::array<std::vector<std::byte>, 2> flags, std::array<std::vector<std::uint64_t>, 2> cuts,
        std::uint64_t count)
      : native_(std::move(native)), main_(std::move(main)), secondary_(std::move(secondary)),
        borrowed_(std::move(borrowed)), ranks_(std::move(ranks)), flags_(std::move(flags)), cuts_(std::move(cuts)), count_(count) {}
  };

  namespace cola_detail {
    // Default output owns the two FC payloads. Alternate concrete outputs
    // consume the same known-prefix stream and final sparse navigation.
    template <class P, class Native, class Main> struct index_output {
      using index_type = cola_index<P, Native, Main>;
      using native_pointer = typename index_type::native_pointer;
      using main_pointer = typename index_type::main_pointer;
      bool failed() const noexcept { return false; }
      void append_known(unsigned route, bit_view key, std::uint64_t common) { writers_[route].append_known(key, common); }
      index_type finish(native_pointer native, main_pointer main, native_pointer secondary, index_metadata<P> metadata) {
        std::array<typename index_type::borrowed_array, 2> borrowed{writers_[0].finish(), writers_[1].finish()};
        return index_type(std::move(native), std::move(main), std::move(secondary), std::move(borrowed),
          std::move(metadata.ranks), std::move(metadata.flags), std::move(metadata.cuts), metadata.count);
      }
    private:
      std::array<typename index_type::stream_family::borrowed_writer, 2> writers_;
    };
  }

  // Samples the exact local three-way augmented order, retaining its owner.
  // Generic mapped targets provide the same cola_index_view through view().
  template <class P, class Target> struct cola_sample_cursor {
    using policy_type = P;
    explicit cola_sample_cursor(std::shared_ptr<Target const> target) : cola_sample_cursor(bind(std::move(target))) {}
    bool done() const noexcept { return !target_ || ordinal_ == count_; }
    std::shared_ptr<Target const> target() const noexcept { return target_; }
    cola_sample_view<P> peek() const & {
      if (done()) error_detail::raise<std::out_of_range>("COLA sampler at end");
      auto origin = choose().origin;
      if (!origin) { auto item = native_.peek(); return {item.key.prefix, ordinal_, cola_origin::native, item.ordinal}; }
      auto item = borrowed_[origin - 1].peek();
      return {item.key.prefix, ordinal_, cola_origin(origin), item.ordinal};
    }
    cola_sample_view<P> peek() const && = delete;
    void advance() { (void)advance_impl<false>(); }
    // Exact old/new sampled-key comparison, accumulated across at most K
    // adjacent transitions. EOF has no successor and returns nullopt.
    std::optional<bit_comparison> advance_comparison() { return advance_impl<true>(); }

  private:
    template <bool Compare> std::optional<bit_comparison> advance_impl() {
      if (done()) error_detail::raise<std::out_of_range>("COLA sampler at end");
      auto selected = choose();
      auto first_length = selected.common[selected.origin];
      auto common = first_length;
      auto width = std::min<std::uint64_t>(P::group_size, count_ - ordinal_);
      for (std::uint64_t i = 0; i < width; ++i) {
        prefixes_ = selected.common;
        auto next = !selected.origin ? native_.advance_comparison() :
          borrowed_[selected.origin - 1].advance_comparison();
        if (next && (next->order > 0 || (!selected.origin && !next->order)))
          error_detail::raise<std::invalid_argument>("COLA sample source order mismatch");
        prefixes_[selected.origin] = next ? next->common_bits : 0;
        ++ordinal_;
        if (!done() && (Compare || i + 1 < width)) {
          selected = choose();
          if constexpr (Compare) common = std::min(common, prefixes_[selected.origin]);
        }
      }
      if constexpr (Compare) if (!done())
        return bit_comparison{common, common == first_length &&
          selected.common[selected.origin] == first_length ? 0 : -1};
      return std::nullopt;
    }
    using view_type = decltype(std::declval<Target const &>().view());
    using native_cursor = decltype(std::declval<typename view_type::native_view>().cursor());
    using borrowed_cursor = decltype(std::declval<typename view_type::borrowed_view>().cursor());
    struct binding { std::shared_ptr<Target const> target; view_type view; };
    std::shared_ptr<Target const> target_;
    native_cursor native_;
    std::array<borrowed_cursor, 2> borrowed_;
    std::array<std::uint64_t, 3> prefixes_{};
    std::uint64_t count_, ordinal_ = 0;
    static binding bind(std::shared_ptr<Target const> target) {
      if (!target) error_detail::raise<std::invalid_argument>("null COLA sample target");
      auto view = target->view();
      return {std::move(target), std::move(view)};
    }
    explicit cola_sample_cursor(binding source)
      : target_(std::move(source.target)), native_(source.view.native()),
        borrowed_{borrowed_cursor(source.view.borrowed(0)), borrowed_cursor(source.view.borrowed(1))},
        count_(source.view.virtual_size()) {}
    cola_detail::frontier_selection choose() const {
      return cola_detail::choose_frontier(prefixes_,
        [&](unsigned origin) { return !origin ? !native_.done() : !borrowed_[origin - 1].done(); },
        [&](unsigned origin) { return !origin ? native_.peek().key.prefix : borrowed_[origin - 1].peek().key.prefix; });
    }
  };

  // Each step consumes at most budget merged occurrences. Consuming a sampled
  // head advances its source by at most K occurrences, and bytes/comparison and
  // allocation remain additional work. Only current cursor/key contexts are
  // reconstructed; borrowed payload and compact navigation are output storage.
  // Without targets only native count is inspected; step charges occurrences
  // while doing one zero-navigation update per crossed K-group, without keys.
  // Native contents must already be trusted/admitted; this metadata-only path
  // deliberately does not revalidate their framing or strictly sorted order.
  template <class P, class Native, class Main, class Output> struct cola_index_builder {
    using policy_type = P;
    using index_type = cola_index<P, Native, Main>;
    using native_pointer = typename index_type::native_pointer;
    using pair_type = typename index_type::pair_type;
    using main_pointer = typename index_type::main_pointer;
    using target_type = typename index_type::target_type;
    explicit cola_index_builder(native_pointer native, main_pointer main = {}, native_pointer secondary = {})
      : cola_index_builder(Output{}, std::move(native), std::move(main), std::move(secondary)) {}
    cola_index_builder(Output output, native_pointer native, main_pointer main = {}, native_pointer secondary = {})
      : native_(checked(std::move(native))), main_(std::move(main)), secondary_(std::move(secondary)), output_(std::move(output)) {
      auto remaining = std::numeric_limits<std::uint64_t>::max() - native_->size();
      auto primary_count = main_ ? main_->group_count() : 0;
      auto secondary_count = secondary_ ? secondary_->size() / P::group_size + (secondary_->size() % P::group_size != 0) : 0;
      if (primary_count > remaining || secondary_count > remaining - primary_count)
        error_detail::raise<std::length_error>("COLA augmented count overflow");
      if (main_ || secondary_) native_cursor_.emplace(native_->view());
      if (main_) primary_cursor_.emplace(main_);
      if (secondary_) secondary_cursor_.emplace(secondary_->view());
    }
    cola_index_builder(cola_index_builder const &) = delete;
    cola_index_builder & operator=(cola_index_builder const &) = delete;
    cola_index_builder(cola_index_builder &&) = default;
    cola_index_builder & operator=(cola_index_builder &&) = default;
    bool done() const noexcept {
      return native_ && !failed_ && (native_cursor_ ? !live(0) && !live(1) && !live(2) : count_ == native_->size());
    }
    bool failed() const noexcept { return failed_ || output_.failed(); }
    bool finished() const noexcept { return finished_; }
    std::uint64_t size() const noexcept { return count_; }
    std::uint64_t step(std::uint64_t budget) {
      require_active();
      std::uint64_t consumed = 0;
      try {
        // With no targets, all augmented occurrences are native. Navigation
        // depends only on their count: do not open a payload cursor at all.
        if (!native_cursor_) return step_terminal(budget);
        while (consumed < budget && !done()) {
          auto selected = cola_detail::choose_frontier(prefixes_,
            [&](unsigned origin) { return live(origin); },
            [&](unsigned origin) { return head(origin); });
          auto origin = selected.origin;
          auto key = head(origin);
          auto common = prefixes_[origin];
          bool equal = count_ && common == previous_length_ && key.size() == previous_length_;
          if (!origin && equal)
            error_detail::raise<std::invalid_argument>("COLA sources violate sorted native order");
          for (unsigned route = 0; route < 2; ++route)
            if (borrowed_count_[route]) cut_common_[route] = std::min(cut_common_[route], common);
          if (!width_) for (unsigned route = 0; route < 2; ++route)
            cuts_[route].push_back(borrowed_count_[route] ? cut_common_[route] : 0);
          native_equal_ = !origin || (equal && native_equal_);
          if (origin) {
            auto route = origin - 1;
            // Along a sorted walk the minimum adjacent LCP since the preceding
            // borrow is its exact LCP with this key. Reuse that same frontier
            // for FC output instead of comparing the inherited prefix again.
            output_.append_known(route, key, borrowed_count_[route] ? cut_common_[route] : 0);
            auto ordinal = borrowed_count_[route]++;
            if (!(ordinal & 7)) flags_[route].push_back(std::byte{0});
            if (native_equal_) flags_[route].back() |= std::byte(1u << (ordinal & 7));
            ++population_[route];
            cut_common_[route] = key.size();
          }
          previous_length_ = key.size();
          prefixes_ = selected.common;
          if (!origin) prefixes_[0] = advance_native(*native_cursor_);
          else if (origin == 1) {
            auto next = primary_cursor_->advance_comparison();
            prefixes_[1] = next ? next->common_bits : 0;
          } else {
            auto next_common = key.size();
            for (std::uint64_t i = 0; i < P::group_size && !secondary_cursor_->done(); ++i)
              next_common = std::min(next_common, advance_native(*secondary_cursor_));
            prefixes_[2] = next_common;
          }
          ++count_; ++consumed; ++width_;
          if (width_ == P::group_size) flush_group();
        }
      } catch (...) { failed_ = true; throw; }
      return consumed;
    }
    auto finish() {
      require_active();
      if (!done()) error_detail::raise<std::logic_error>("COLA builder has remaining input");
      try {
        if (width_) flush_group();
        cola_detail::index_metadata<P> metadata{{ranks_[0].finish(), ranks_[1].finish()},
          std::move(flags_), std::move(cuts_), count_};
        auto result = output_.finish(native_, main_, secondary_, std::move(metadata));
        finished_ = true;
        return result;
      } catch (...) { failed_ = true; throw; }
    }
  private:
    native_pointer native_;
    main_pointer main_;
    native_pointer secondary_;
    using native_cursor_type = decltype(std::declval<Native const &>().view().cursor());
    std::optional<native_cursor_type> native_cursor_;
    std::optional<cola_sample_cursor<P, target_type>> primary_cursor_;
    std::optional<native_cursor_type> secondary_cursor_;
    Output output_;
    std::array<rank_groups_builder<P::group_size>, 2> ranks_;
    std::array<std::vector<std::byte>, 2> flags_;
    std::array<std::vector<std::uint64_t>, 2> cuts_;
    std::array<std::uint64_t, 2> borrowed_count_{}, cut_common_{}, population_{};
    std::array<std::uint64_t, 3> prefixes_{};
    std::uint64_t previous_length_ = 0, count_ = 0, width_ = 0;
    bool native_equal_ = false, failed_ = false, finished_ = false;
    static native_pointer checked(native_pointer native) {
      if (!native) error_detail::raise<std::invalid_argument>("null COLA native source");
      return native;
    }
    void require_active() const {
      if (!native_ || failed() || finished_) error_detail::raise<std::logic_error>("COLA builder is no longer active");
    }
    bool live(unsigned origin) const noexcept {
      if (!origin) return native_cursor_ && !native_cursor_->done();
      if (origin == 1) return primary_cursor_ && !primary_cursor_->done();
      return secondary_cursor_ && !secondary_cursor_->done();
    }
    bit_view head(unsigned origin) const {
      if (!origin) return native_cursor_->peek().key.prefix;
      if (origin == 1) return primary_cursor_->peek().key;
      return secondary_cursor_->peek().key.prefix;
    }
    static std::uint64_t advance_native(native_cursor_type & cursor) {
      auto next = cursor.advance_comparison();
      if (next && next->order >= 0)
        error_detail::raise<std::invalid_argument>("COLA native source order mismatch");
      return next ? next->common_bits : 0;
    }
    std::uint64_t step_terminal(std::uint64_t budget) {
      std::uint64_t consumed = 0;
      auto total = native_->size();
      while (consumed < budget && count_ < total) {
        if (!width_) for (auto & cuts : cuts_) cuts.push_back(0);
        auto chunk = std::min({budget - consumed, total - count_, P::group_size - width_});
        count_ += chunk; consumed += chunk; width_ += chunk;
        if (width_ == P::group_size) flush_group();
      }
      return consumed;
    }
    void flush_group() {
      for (unsigned route = 0; route < 2; ++route) ranks_[route].append(population_[route], width_);
      width_ = 0; population_ = {};
    }
  };

  template <class P, class Native, class Main>
  cola_index<P, Native, Main> cola_index<P, Native, Main>::build(
      std::span<profile_record const> records, main_pointer main, native_pointer secondary)
      requires (std::is_same_v<Native, profile_array<P>> && std::is_void_v<Main>) {
    profile_native_writer<P> writer;
    for (auto const & record : records) writer.append(record);
    return adopt_native(std::make_shared<native_array const>(writer.finish()), std::move(main), std::move(secondary));
  }
  template <class P, class Native, class Main>
  cola_index<P, Native, Main> cola_index<P, Native, Main>::adopt_native(
      native_pointer native, main_pointer main, native_pointer secondary) {
    cola_index_builder<P, Native, Main> builder(std::move(native), std::move(main), std::move(secondary));
    while (!builder.done()) builder.step(4096);
    return builder.finish();
  }
  template <class P, class Native, class Main>
  typename cola_index<P, Native, Main>::pair_type cola_index<P, Native, Main>::prepare_root(
      pair_type main, native_pointer secondary)
      requires (std::is_same_v<Native, profile_array<P>> && std::is_void_v<Main>) {
    if (main && !secondary && main->virtual_size() <= group_size) return main;
    auto empty = std::make_shared<native_array const>(native_array::build({}));
    auto root = std::make_shared<cola_index const>(adopt_native(empty, std::move(main), std::move(secondary)));
    while (root->virtual_size() > group_size)
      root = std::make_shared<cola_index const>(adopt_native(empty, std::move(root)));
    return root;
  }
}
