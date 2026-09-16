/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Advances an age-ordered native frontier without repeated point queries.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <everett/profile.h>
#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

namespace everett::typed_detail {
  // Runs are oldest first. Equal keys leave the heap in that same order.
  // Each cursor retains its current decoded key and parsed physical frame.
  template <class World> struct native_sweep {
    using native_type = typename World::runtime_family::native_type;
    struct observation { bit_view value; std::uint64_t retained_bits; };
    explicit native_sweep(World const & snapshot) {
      if constexpr (requires { snapshot.runtime().runs(); }) {
        auto runs = snapshot.runtime().runs();
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
      } else throw std::logic_error("runtime does not expose a native sweep");
    }
    bool done() const noexcept { return heap_.empty(); }
    std::uint64_t consumed() const noexcept { return consumed_; }
    auto peek() const { return sources_[heap_.front()].cursor.peek(); }
    std::uint64_t retained_bits() const {
      auto const & cursor = sources_[heap_.front()].cursor;
      if constexpr (requires { cursor.retained_bits(); }) return cursor.retained_bits();
      else return 0; // A custom cursor can conservatively emit the entire key.
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
    // Queries must be increasing. Values borrow pinned immutable natives.
    std::optional<observation> replacement(bit_view key) {
      while (!done() && compare_bits(peek().key.prefix, key) < 0) consume();
      std::optional<observation> result;
      while (!done() && compare_bits(peek().key.prefix, key) == 0) {
        result = observation{peek().value, retained_bits()};
        consume();
      }
      return result;
    }
  private:
    struct source {
      std::shared_ptr<native_type const> native;
      decltype(std::declval<native_type const &>().view().cursor()) cursor;
      explicit source(std::shared_ptr<native_type const> value)
        : native(std::move(value)), cursor(native->view()) {}
    };
    std::vector<source> sources_;
    std::vector<std::size_t> heap_;
    std::uint64_t consumed_ = 0;
    auto later() const {
      return [this](std::size_t a, std::size_t b) {
        auto order = compare_bits(sources_[a].cursor.peek().key.prefix, sources_[b].cursor.peek().key.prefix);
        return order ? order > 0 : a > b;
      };
    }
  };
}
