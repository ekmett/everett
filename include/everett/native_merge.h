/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Merges ordered native streams incrementally with policy-specific value composition.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/native_writer.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace everett {
  struct replace_native_value {
    bit_view operator()(bit_view, bit_view, bit_view newer) const { return newer; }
    bit_view operator()(bit_view, bit_view newer) const { return newer; }
  };

  namespace native_merge_detail {
    template <class T> T & composer(T & value) { return value; }
    template <class T> T & composer(std::reference_wrapper<T> value) { return value.get(); }
    template <class Compose, class Key> bool is_tombstone(Compose & compose, bit_view value, Key && key) {
      auto & semantic = composer(compose);
      if constexpr (requires { semantic.is_tombstone(value); })
        return semantic.is_tombstone(value);
      else if constexpr (requires { semantic.is_tombstone(bit_view{}, value); }) {
        auto full = std::invoke(std::forward<Key>(key));
        if constexpr (std::is_same_v<decltype(full), bit_view>) return semantic.is_tombstone(full, value);
        else return semantic.is_tombstone(full.view(), value);
      } else return false;
    }
    // Keep the predecessor as immutable literal spans, without copying its
    // inherited prefix. A new retained boundary can precede the immediately
    // preceding literal, so checking that one literal alone is insufficient.
    // Each record adds at most one span; truncation removes spans permanently.
    // Deep proper-prefix chains can retain one span per key unit.
    template <class P> struct encoded_source {
      explicit encoded_source(profile_view<P> view)
        : data_(view.bytes(), view.metadata().extent << P::unit_shift), cursor_(view) {
        if (!done()) retain(cursor_.peek());
      }
      bool done() const noexcept { return cursor_.done(); }
      profile_encoded_record const & peek() const & { return cursor_.peek(); }
      profile_encoded_record const & peek() const && = delete;
      std::uint64_t retained_bits() const { return peek().retained << P::unit_shift; }
      bit_string materialize() const {
        bit_string result;
        std::uint64_t begin = 0;
        for (auto const & part : spans_) {
          profile_detail::append(result, data_.subview(part.source_bit_offset,
            (part.end_units - begin) << P::unit_shift));
          begin = part.end_units;
        }
        return result;
      }

      std::optional<bit_comparison> advance_comparison() {
        cursor_.advance();
        if (done()) return std::nullopt;
        auto const & next = cursor_.peek();
        auto result = compare_successor(next.retained, next.suffix);
        if (result.order >= 0)
          throw std::invalid_argument("native merge inputs must have unique sorted keys");
        retain(next);
        return result;
      }

    private:
      struct span {
        std::uint64_t source_bit_offset;
        std::uint64_t end_units;
      };
      static_assert(sizeof(span) == 16);
      bit_comparison compare_successor(std::uint64_t retained, bit_view suffix) const {
        auto first = spans_.size();
        // Every crossed span beyond this boundary will be removed by retain;
        // walking back is amortized over the one span added per input record.
        while (first && spans_[first - 1].end_units > retained) --first;
        auto begin = first ? spans_[first - 1].end_units : 0;
        // Ordinary FC differs in the first remaining policy unit. Handle it
        // directly; redundant controls fall through to the full fragment walk.
        if (first != spans_.size() && !suffix.empty()) {
          auto offset = spans_[first].source_bit_offset + ((retained - begin) << P::unit_shift);
          auto before = profile_detail::load_bits(data_, offset, P::bits_per_unit);
          auto after = profile_detail::load_bits(suffix, 0, P::bits_per_unit);
          if (before != after) {
            auto common = unsigned(std::countl_zero(before ^ after)) - (64 - P::bits_per_unit);
            return {(retained << P::unit_shift) + common, before < after ? -1 : 1};
          }
        }
        auto position = retained;
        std::uint64_t compared = 0;
        while (first != spans_.size() && compared != suffix.size()) {
          auto const & fragment = spans_[first];
          auto offset = fragment.source_bit_offset + ((position - begin) << P::unit_shift);
          auto count = std::min((fragment.end_units - position) << P::unit_shift,
                                suffix.size() - compared);
          auto result = compare_common_bits(data_.subview(offset, count), suffix.subview(compared, count));
          if (result.order) {
            result.common_bits += (retained << P::unit_shift) + compared;
            return result;
          }
          compared += count;
          position = fragment.end_units;
          begin = position;
          ++first;
        }
        auto previous_units = spans_.empty() ? 0 : spans_.back().end_units;
        auto previous_remaining = (previous_units - retained) << P::unit_shift;
        return {(retained << P::unit_shift) + compared,
                previous_remaining < suffix.size() ? -1 : previous_remaining > suffix.size() ? 1 : 0};
      }
      void retain(profile_encoded_record const & record) {
        auto retained = record.retained;
        while (!spans_.empty()) {
          auto begin = spans_.size() > 1 ? spans_[spans_.size() - 2].end_units : 0;
          if (begin >= retained) spans_.pop_back();
          else {
            if (spans_.back().end_units > retained) spans_.back().end_units = retained;
            break;
          }
        }
        // Every frame uses this one immutable backing. A span's logical start
        // is the preceding endpoint, so truncation needs no copied length/view.
        if (!record.suffix.empty()) spans_.push_back({record.suffix.offset(), record.key_units});
      }
      bit_view data_;
      profile_encoded_cursor<P> cursor_;
      std::vector<span> spans_;
    };
  }

  struct native_merge_progress {
    std::uint64_t keys = 0;
    std::uint64_t input_records = 0;
  };

  // Merge two chronologically ordered native streams. Equal keys call
  // compose(key, older_value, newer_value), or compose(older_value, newer_value)
  // for value-only policies. Default composition is replacement. Key-aware
  // policies retain materialized cursors; the other cases forward encoded
  // literals and validate strict input order through immutable prefix spans.
  // A custom policy accepting both forms uses the key-aware form.
  // Each input must have unique sorted keys. Input owners stay pinned,
  // including after a decoding/composition failure.
  // Callback keys borrow cursor scratch. A policy retaining a value view must
  // retain its source owner too; reassignment can release earlier source pins.
  // Construction parses the first record from each nonempty source before any
  // step budget is charged; key-aware policies also reconstruct those keys.
  // One step unit handles one distinct key and at most two input records.
  // Key bytes, composition work, output allocation and final EF work are extra.
  // An alternate Output consumes append(retained,literal,value) synchronously,
  // encodes policy P, and supplies size/common_value_width/finished/failed/finish.
  // It starts empty and active; finished() and failed() are noexcept. The caller
  // grants exclusive sink use to this builder, and keeps any referenced sink
  // alive. A finish failure is retryable only when the sink reports !failed().
  template <class P, class Native = profile_array<P>, class Compose = replace_native_value,
            class Output = profile_detail::native_output<P>>
  struct native_merge_builder {
    static_assert(std::is_same_v<typename Native::policy_type, P>);
    static_assert(std::is_same_v<typename Output::policy_type, P>);
    using policy_type = P;
    using source_type = Native;
    using output_type = Output;
    static_assert(noexcept(std::declval<Output const &>().finished()));
    static_assert(noexcept(std::declval<Output const &>().failed()));
    using source_pointer = std::shared_ptr<Native const>;
    static constexpr bool encoded_keys = std::is_same_v<Compose, replace_native_value> ||
      (!std::is_invocable_v<Compose &, bit_view, bit_view, bit_view> &&
       std::is_invocable_v<Compose &, bit_view, bit_view>);
    static_assert(encoded_keys || std::is_invocable_v<Compose &, bit_view, bit_view, bit_view>,
                  "native composition accepts (key, older, newer) or (older, newer)");

    native_merge_builder(source_pointer older, source_pointer newer, Compose compose = {},
        std::optional<std::uint64_t> common_value_width = P::value_width)
      requires std::is_constructible_v<Output, std::optional<std::uint64_t>>
      : native_merge_builder(Output(common_value_width), std::move(older), std::move(newer), std::move(compose)) {}
    native_merge_builder(Output output, source_pointer older, source_pointer newer, Compose compose = {})
      : older_(checked(std::move(older))), newer_(checked(std::move(newer))),
        older_cursor_(older_->view()), newer_cursor_(newer_->view()),
        writer_(checked_output(std::move(output))), compose_(std::move(compose)) {}
    native_merge_builder(native_merge_builder const &) = delete;
    native_merge_builder & operator=(native_merge_builder const &) = delete;
    native_merge_builder(native_merge_builder &&) = default;
    native_merge_builder & operator=(native_merge_builder && other)
      requires (std::is_move_assignable_v<Compose> && std::is_move_assignable_v<Output>) {
      if (this != &other) {
        // Composition policies may throw during transfer. Keep a partially
        // assigned destination poisoned until every state field agrees.
        failed_ = true;
        older_ = std::move(other.older_); newer_ = std::move(other.newer_);
        older_cursor_ = std::move(other.older_cursor_); newer_cursor_ = std::move(other.newer_cursor_);
        writer_ = std::move(other.writer_); compose_ = std::move(other.compose_);
        progress_ = other.progress_;
        older_prefix_ = other.older_prefix_; newer_prefix_ = other.newer_prefix_;
        failed_ = other.failed_;
      }
      return *this;
    }

    bool done() const noexcept {
      return older_ && newer_ && !failed_ && older_cursor_.done() && newer_cursor_.done();
    }
    bool failed() const noexcept { return failed_; }
    bool finished() const noexcept { return writer_.finished(); }
    native_merge_progress progress() const noexcept { return progress_; }
    source_pointer older_source() const noexcept { return older_; }
    source_pointer newer_source() const noexcept { return newer_; }

    native_merge_progress step(std::uint64_t key_budget = 1) {
      require_active();
      native_merge_progress work;
      try {
        while (work.keys < key_budget && !done()) {
          auto comparison = compare_heads();
          auto order = comparison.order;
          auto consumed = order == 0 ? 2u : 1u;
          auto total_keys = profile_detail::add(progress_.keys, 1);
          auto total_records = profile_detail::add(progress_.input_records, consumed);
          if (order < 0) {
            auto item = older_cursor_.peek();
            append(older_cursor_, item, item.value, older_prefix_, older_cursor_.retained_bits());
            newer_prefix_ = comparison.common_bits >> P::unit_shift;
            older_prefix_ = advance(older_cursor_);
          } else if (order > 0) {
            auto item = newer_cursor_.peek();
            append(newer_cursor_, item, item.value, newer_prefix_, newer_cursor_.retained_bits());
            older_prefix_ = comparison.common_bits >> P::unit_shift;
            newer_prefix_ = advance(newer_cursor_);
          } else {
            auto older = older_cursor_.peek(), newer = newer_cursor_.peek();
            auto value = [&] {
              if constexpr (encoded_keys) return std::invoke(compose_, older.value, newer.value);
              else return std::invoke(compose_, older.key.prefix, older.value, newer.value);
            }();
            auto limit = std::min(older_cursor_.retained_bits(), newer_cursor_.retained_bits());
            auto emit = [&](auto const & source, auto const & item) {
              if constexpr (std::is_same_v<decltype(value), bit_view>)
                append(source, item, value, older_prefix_, limit);
              else append(source, item, value.view(), older_prefix_, limit);
            };
            if constexpr (encoded_keys) {
              // Equal keys have interchangeable literal donors. Use the one
              // that already carries the conservative tombstone boundary,
              // avoiding reconstruction of an older, shorter literal.
              if (newer.retained < older.retained) emit(newer_cursor_, newer);
              else emit(older_cursor_, older);
            } else emit(older_cursor_, older);
            older_prefix_ = advance(older_cursor_);
            newer_prefix_ = advance(newer_cursor_);
          }
          progress_ = {total_keys, total_records};
          ++work.keys;
          work.input_records += consumed;
        }
      } catch (...) {
        failed_ = true;
        throw;
      }
      return work;
    }

    auto finish() {
      require_active();
      if (!done()) throw std::logic_error("native merge has unconsumed inputs");
      try { return writer_.finish(); }
      catch (...) {
        // In-memory EF allocation failure is retryable. A sink that has
        // partially written final output must report its irreversible failure.
        failed_ = writer_.failed();
        throw;
      }
    }

  private:
    using cursor_type = std::conditional_t<encoded_keys, native_merge_detail::encoded_source<P>,
                                            profile_cursor<P, stream_role::native>>;
    static bit_view suffix(auto const & item, std::uint64_t retained) {
      if constexpr (encoded_keys) {
        if (retained < item.retained)
          throw std::invalid_argument("native merge output prefix precedes input prefix");
        auto first = (retained - item.retained) << P::unit_shift;
        return item.suffix.subview(first, item.suffix.size() - first);
      } else {
        auto first = retained << P::unit_shift;
        return item.key.prefix.subview(first, item.key.prefix.size() - first);
      }
    }
    void append(cursor_type const & source, auto const & item, bit_view value,
        std::uint64_t retained, std::uint64_t limit_bits) {
      if (native_merge_detail::is_tombstone(compose_, value, [&] {
          if constexpr (encoded_keys) return source.materialize();
          else return item.key.prefix;
        })) retained = std::min(retained, limit_bits >> P::unit_shift);
      if constexpr (encoded_keys) {
        if (retained < item.retained) {
          auto key = source.materialize();
          auto first = retained << P::unit_shift;
          writer_.append(retained, key.view().subview(first, key.bit_size - first), value);
          return;
        }
      }
      writer_.append(retained, suffix(item, retained), value);
    }
    // Both heads follow the last emitted key p. The head sharing more of p
    // sorts first. Equal LCPs need only a suffix comparison from that boundary.
    bit_comparison compare_heads() const {
      if (older_cursor_.done()) return {0, 1};
      if (newer_cursor_.done()) return {0, -1};
      if (older_prefix_ != newer_prefix_)
        return {std::min(older_prefix_, newer_prefix_) << P::unit_shift,
                older_prefix_ > newer_prefix_ ? -1 : 1};
      auto start = older_prefix_ << P::unit_shift;
      auto result = compare_common_bits(suffix(older_cursor_.peek(), older_prefix_),
                                       suffix(newer_cursor_.peek(), newer_prefix_));
      result.common_bits += start;
      return result;
    }
    static std::uint64_t advance(cursor_type & cursor) {
      auto comparison = cursor.advance_comparison();
      if (!comparison) return 0;
      if (comparison->order >= 0)
        throw std::invalid_argument("native merge inputs must have unique sorted keys");
      return comparison->common_bits >> P::unit_shift;
    }
    static Output checked_output(Output output) {
      if (output.size() || output.finished() || output.failed())
        throw std::invalid_argument("native merge output must be empty and active");
      return output;
    }
    static source_pointer checked(source_pointer source) {
      if (!source) throw std::invalid_argument("null native merge input");
      return source;
    }
    void require_active() const {
      if (!older_ || !newer_ || failed_ || writer_.finished())
        throw std::logic_error("native merge is inactive");
    }
    source_pointer older_;
    source_pointer newer_;
    cursor_type older_cursor_;
    cursor_type newer_cursor_;
    Output writer_;
    Compose compose_;
    native_merge_progress progress_;
    std::uint64_t older_prefix_ = 0;
    std::uint64_t newer_prefix_ = 0;
    bool failed_ = false;
  };
}
