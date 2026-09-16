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

#include <everett/cola_index.h>
#include <algorithm>
#include <memory>
#include <optional>
#include <vector>
#include <unordered_map>

namespace everett {
  // Optional initialization instrumentation. Seeding bounds framing headers;
  // it excludes selector dictionary parsing and reconstructed key bytes.
  struct range_positioning_work {
    std::uint64_t catalogs = 0, seeded_headers = 0;
    profile_comparison_work comparisons;
  };
}
namespace everett::typed_detail {
  // Runs are oldest first. Equal keys leave the heap in that same order.
  // Each cursor retains its current decoded key and parsed physical frame.
  template <class World> struct native_sweep {
    using native_type = typename World::runtime_family::native_type;
    struct observation { bit_view value; std::uint64_t retained_bits; };
    native_sweep() = default;
    explicit native_sweep(World const & snapshot, bit_view lower = {}, range_positioning_work * work = nullptr) {
      if (work) *work = {};
      using P = typename World::policy_type;
      std::unordered_map<native_type const *, std::uint64_t> starts;
      auto node = snapshot.runtime().query_root().head();
      auto context = profile_query_context<P>(lower);
      std::uint64_t group = 0;
      bool above = false;
      while (node) {
        if (work) ++work->catalogs;
        auto view = node->view();
        cola_lower_bound_result<P> found;
        if (!above && view.virtual_size()) found = view.lower_bound_window(group, context, work ? &work->comparisons : nullptr);
        starts.emplace(node->native_owner().get(), found.native_ordinal);
        if (auto side = node->secondary_target()) {
          if (work) ++work->catalogs;
          std::uint64_t ordinal = 0;
          if (auto const & predecessor = found.predecessors[1]) {
            auto leaf = side->view();
            auto first = predecessor->target_ordinal;
            if (first >= leaf.size() || first % P::group_size)
              throw std::invalid_argument("invalid range secondary route");
            auto last = first + std::min<std::uint64_t>(P::group_size, leaf.size() - first);
            ordinal = last;
            leaf.compare_window(first, last, predecessor->comparison, [&](profile_comparison_item<P> item) {
              if (item.comparison.order() < 0) return true;
              ordinal = item.ordinal; return false;
            }, work ? &work->comparisons : nullptr);
          }
          starts.emplace(side.get(), ordinal);
        }
        auto main = node->main_target();
        if (auto const & predecessor = found.predecessors[0]) {
          if (!main || predecessor->target_ordinal % P::group_size || predecessor->target_ordinal >= main->virtual_size())
            throw std::invalid_argument("invalid range main route");
          group = predecessor->target_ordinal / P::group_size;
          context = predecessor->comparison;
        } else above = true; // No sampled predecessor: all downstream keys exceed lower.
        node = std::move(main);
      }
      if constexpr (requires { snapshot.runtime().runs(); }) {
        auto runs = snapshot.runtime().runs();
        sources_.reserve(runs.size()); heap_.reserve(runs.size());
        for (auto const & run : runs) {
          auto native = [&] {
            if constexpr (requires { run.native_owner(); }) return run.native_owner();
            else return run->native;
          }();
          auto position = starts.find(native.get());
          if (position == starts.end()) throw std::invalid_argument("native run absent from range cascade");
          if (work && position->second < native->view().size())
            work->seeded_headers += position->second % P::codec_block_size + 1;
          sources_.emplace_back(std::move(native), position->second, lower);
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
      source(std::shared_ptr<native_type const> value, std::uint64_t ordinal, bit_view query)
        : native(std::move(value)), cursor(native->view(), ordinal, query) {}
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
