/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/native_writer.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace everett {
  struct replace_native_value {
    bit_view operator()(bit_view, bit_view, bit_view newer) const { return newer; }
  };

  struct native_merge_progress {
    std::uint64_t keys = 0;
    std::uint64_t input_records = 0;
  };

  // Merge two chronologically ordered native streams. Equal keys call
  // compose(key, older_value, newer_value); default composition is replacement.
  // Each input must have unique sorted keys. Input owners stay pinned,
  // including after a decoding/composition failure. Construction decodes the
  // first record from each nonempty source before any step budget is charged.
  // One step unit handles one distinct key and at most two input records.
  // Key bytes, composition work, output allocation and final EF work are extra.
  template <class P, class Native = profile_array<P>, class Compose = replace_native_value>
  struct native_merge_builder {
    static_assert(std::is_same_v<typename Native::policy_type, P>);
    using policy_type = P;
    using source_type = Native;
    using source_pointer = std::shared_ptr<Native const>;

    native_merge_builder(source_pointer older, source_pointer newer, Compose compose = {},
        std::optional<std::uint64_t> common_value_width = P::value_width)
      : older_(checked(std::move(older))), newer_(checked(std::move(newer))),
        older_cursor_(older_->view()), newer_cursor_(newer_->view()),
        writer_(common_value_width), compose_(std::move(compose)) {}
    native_merge_builder(native_merge_builder const &) = delete;
    native_merge_builder & operator=(native_merge_builder const &) = delete;
    native_merge_builder(native_merge_builder &&) = default;
    native_merge_builder & operator=(native_merge_builder && other)
      requires std::is_move_assignable_v<Compose> {
      if (this != &other) {
        // Composition policies may throw during transfer. Keep a partially
        // assigned destination poisoned until every state field agrees.
        failed_ = true;
        older_ = std::move(other.older_); newer_ = std::move(other.newer_);
        older_cursor_ = std::move(other.older_cursor_); newer_cursor_ = std::move(other.newer_cursor_);
        writer_ = std::move(other.writer_); compose_ = std::move(other.compose_);
        progress_ = other.progress_; failed_ = other.failed_;
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
          auto order = older_cursor_.done() ? 1 : newer_cursor_.done() ? -1 :
            compare_bits(older_cursor_.peek().key.prefix, newer_cursor_.peek().key.prefix);
          auto consumed = order == 0 ? 2u : 1u;
          auto total_keys = profile_detail::add(progress_.keys, 1);
          auto total_records = profile_detail::add(progress_.input_records, consumed);
          if (order < 0) {
            auto item = older_cursor_.peek();
            writer_.append(item.key.prefix, item.value);
            older_cursor_.advance();
          } else if (order > 0) {
            auto item = newer_cursor_.peek();
            writer_.append(item.key.prefix, item.value);
            newer_cursor_.advance();
          } else {
            auto older = older_cursor_.peek(), newer = newer_cursor_.peek();
            auto value = std::invoke(compose_, older.key.prefix, older.value, newer.value);
            if constexpr (std::is_same_v<decltype(value), bit_view>) writer_.append(older.key.prefix, value);
            else writer_.append(older.key.prefix, value.view());
            older_cursor_.advance();
            newer_cursor_.advance();
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

    profile_array<P> finish() {
      require_active();
      if (!done()) throw std::logic_error("native merge has unconsumed inputs");
      return writer_.finish();
    }

  private:
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
    profile_cursor<P, stream_role::native> older_cursor_;
    profile_cursor<P, stream_role::native> newer_cursor_;
    profile_native_writer<P> writer_;
    Compose compose_;
    native_merge_progress progress_;
    bool failed_ = false;
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Merges ordered native streams incrementally with policy-specific value composition.
 */
