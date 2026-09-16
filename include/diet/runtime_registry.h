/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Retains one publication's exact owner closure without revisiting shared suffixes.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <diet/sections.h>

#include <memory>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace diet::runtime_store_detail {
  struct ignore_registry_visits {
    void operator()(bool, bool) const noexcept {}
  };

  // Counts registry roots and immediate edges, separately from shared_ptr's
  // ownership. A live owner has acquired its children exactly once. No owner
  // points back to this registry; retaining it costs only its current closure.
  template <class Node, class Visit = ignore_registry_visits> struct runtime_registry {
    using pair_type = typename Node::pair_type;
    using native_pointer = typename Node::native_pointer;
    using native_root = std::pair<object_id, native_pointer>;
    explicit runtime_registry(Visit visit = {}) : visit_(std::move(visit)) {
      static_assert(noexcept(std::declval<Visit &>()(false, false)));
      static_assert(std::is_nothrow_move_constructible_v<Visit> && std::is_nothrow_move_assignable_v<Visit>);
    }
    runtime_registry(runtime_registry const &) = delete;
    runtime_registry & operator=(runtime_registry const &) = delete;
    runtime_registry(runtime_registry && other) noexcept : visit_(std::move(other.visit_)) { swap_roots(other); }
    runtime_registry & operator=(runtime_registry && other) noexcept {
      if (this != &other) { clear(); visit_ = std::move(other.visit_); swap_roots(other); }
      return *this;
    }
    ~runtime_registry() { clear(); }

    pair_type pair(blob_identity const & id) const {
      auto found = pairs_.find(id.index.hex());
      if (found == pairs_.end()) return {};
      if (found->second.value->mapped()->identity() != id)
        throw std::invalid_argument("checkpoint index has another native identity");
      return found->second.value;
    }
    native_pointer native(object_id const & id) const {
      auto found = natives_.find(id.hex());
      return found == natives_.end() ? native_pointer{} : found->second.value;
    }
    std::size_t pair_count() const noexcept { return pairs_.size(); }
    std::size_t native_count() const noexcept { return natives_.size(); }

    // Failed acquisition rolls back every increment and leaves the previous
    // authorized closure unchanged. Only after all roots are coherent do we
    // retire the old roots. Callers decode against exactly this new closure.
    bool replace(std::vector<pair_type> pairs, std::vector<native_root> natives) {
      std::size_t p = 0, n = 0;
      auto rollback = [&]() noexcept {
        while (n) release_native(natives[--n].first);
        while (p) release_pair(pairs[--p]->mapped()->identity());
      };
      try {
        for (; p != pairs.size(); ++p) if (!acquire_pair(pairs[p])) { rollback(); return false; }
        for (; n != natives.size(); ++n) if (!acquire_native(natives[n].first, natives[n].second)) { rollback(); return false; }
      } catch (...) { rollback(); throw; }
      for (auto const & old : native_roots_) release_native(old.first);
      for (auto const & old : pair_roots_) release_pair(old->mapped()->identity());
      pair_roots_ = std::move(pairs); native_roots_ = std::move(natives);
      trim(pairs_); trim(natives_);
      return true;
    }
    void clear() noexcept {
      for (auto const & old : native_roots_) release_native(old.first);
      for (auto const & old : pair_roots_) release_pair(old->mapped()->identity());
      native_roots_.clear(); pair_roots_.clear();
      decltype(pairs_){}.swap(pairs_); decltype(natives_){}.swap(natives_);
      decltype(pair_roots_){}.swap(pair_roots_); decltype(native_roots_){}.swap(native_roots_);
    }

  private:
    template <class T> struct entry { T value; std::size_t references = 1; };
    std::unordered_map<std::string, entry<pair_type>> pairs_;
    std::unordered_map<std::string, entry<native_pointer>> natives_;
    std::vector<pair_type> pair_roots_;
    std::vector<native_root> native_roots_;
    [[no_unique_address]] Visit visit_;
    template <class Map> static void trim(Map & map) noexcept {
      // Geometric shrinkage pays for rebuilding buckets with the retired
      // entries. Allocation failure may retain spare buckets, never owners.
      if (map.bucket_count() > 64 && map.size() < map.bucket_count() / 4) {
        try { map.rehash(map.size() * 2 + 1); }
        catch (std::bad_alloc const &) {}
      }
    }
    void swap_roots(runtime_registry & other) noexcept {
      pairs_.swap(other.pairs_); natives_.swap(other.natives_);
      pair_roots_.swap(other.pair_roots_); native_roots_.swap(other.native_roots_);
    }
    bool acquire_native(object_id const & id, native_pointer const & value) {
      auto [found, inserted] = natives_.try_emplace(id.hex(), entry<native_pointer>{value});
      if (!inserted) {
        if (found->second.value != value) return false;
        ++found->second.references;
      } else visit_(false, true);
      return true;
    }
    bool acquire_pair(pair_type const & value) {
      auto const & id = value->mapped()->identity();
      auto [found, inserted] = pairs_.try_emplace(id.index.hex(), entry<pair_type>{value});
      if (!inserted) {
        if (found->second.value != value || found->second.value->mapped()->identity() != id) return false;
        ++found->second.references; return true;
      }
      // Recursive insertion can rehash the maps; retain identities and owners,
      // never iterators into those maps, across the child acquisitions.
      auto main = value->main_target(); auto secondary = value->secondary_target();
      bool native_live = false, main_live = false, secondary_live = false;
      auto rollback = [&]() noexcept {
        pairs_.erase(id.index.hex());
        if (secondary_live) release_native(*value->mapped()->index_object()->secondary_id());
        if (main_live) release_pair(main->mapped()->identity());
        if (native_live) release_native(id.native);
      };
      try {
        native_live = acquire_native(id.native, value->native_owner());
        if (!native_live) { rollback(); return false; }
        if (main && !(main_live = acquire_pair(main))) { rollback(); return false; }
        if (secondary && !(secondary_live = acquire_native(*value->mapped()->index_object()->secondary_id(), secondary))) {
          rollback(); return false;
        }
      } catch (...) { rollback(); throw; }
      visit_(true, true); return true;
    }
    void release_native(object_id const & id) noexcept {
      auto found = natives_.find(id.hex());
      if (!--found->second.references) { natives_.erase(found); visit_(false, false); }
    }
    void release_pair(blob_identity const & id) noexcept {
      auto found = pairs_.find(id.index.hex());
      if (--found->second.references) return;
      auto value = std::move(found->second.value);
      pairs_.erase(found); visit_(true, false);
      if (auto secondary = value->secondary_target())
        release_native(*value->mapped()->index_object()->secondary_id());
      if (auto main = value->main_target()) release_pair(main->mapped()->identity());
      release_native(value->mapped()->identity().native);
    }
  };
}
