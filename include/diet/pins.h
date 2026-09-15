/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Diet's pins support.
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include "diet/fingerprint.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace diet {
  // Persistent owner of an exact collection of objects and their additive
  // contributions. A contribution is not necessarily the hash of the object's
  // own native records: an update that deletes a key contributes its negative
  // old binding even when its sole native record is a zero-hash tombstone.
  template <class T, class A = wrapping_fingerprint_algebra> struct pin_set {
    using element = typename A::element;
    using pin = std::shared_ptr<T const>;

    struct entry {
      // An exact object identity, assigned by the object store, never inferred
      // from a collision-prone logical signature. Different objects may have
      // equal contributions and equal own-record signatures.
      std::string object_id;
      pin object;
      element contribution = A::zero();
      std::optional<element> native_signature;
    };

    pin_set() : state_(std::make_shared<state>()) {}

    std::size_t size() const noexcept { return state_->entries.size(); }
    bool empty() const noexcept { return state_->entries.empty(); }
    std::span<entry const> entries() const noexcept { return state_->entries; }
    std::span<pin const> pins() const noexcept { return state_->pins; }
    element signature() const { return state_->signature; }

    entry const * find(std::string_view object_id) const noexcept {
      auto found = std::find_if(state_->entries.begin(), state_->entries.end(),
        [&](entry const & candidate) { return candidate.object_id == object_id; });
      return found == state_->entries.end() ? nullptr : &*found;
    }

    element recompute_signature() const {
      auto result = A::zero();
      for (auto const & item : state_->entries) result = A::add(result, item.contribution);
      return result;
    }

    pin_set add(entry item) const {
      validate(item);
      if (find(item.object_id)) throw std::invalid_argument("duplicate pinned object identity");
      auto next = std::make_shared<state>(*state_);
      next->signature = A::add(next->signature, item.contribution);
      next->pins.push_back(item.object);
      next->entries.push_back(std::move(item));
      return pin_set(std::move(next));
    }

    pin_set remove(std::string_view object_id) const {
      auto old = find(object_id);
      if (!old) throw std::invalid_argument("removing an unpinned object identity");
      auto next = std::make_shared<state>();
      next->signature = A::subtract(state_->signature, old->contribution);
      for (auto const & item : state_->entries) {
        if (item.object_id == object_id) continue;
        next->entries.push_back(item);
        next->pins.push_back(item.object);
      }
      return pin_set(std::move(next));
    }

    // Replace a physical composition while preserving its logical contribution.
    // All expected identities must still exist, exactly once. A new immutable
    // owner is returned only after validating the complete replacement; old
    // readers retain the original objects and metadata without synchronization.
    pin_set replace(std::span<std::string const> expected_inputs,
      std::vector<entry> replacements) const {
      std::vector<std::string_view> inputs;
      inputs.reserve(expected_inputs.size());
      auto removed = A::zero();
      for (auto const & object_id : expected_inputs) {
        if (std::find(inputs.begin(), inputs.end(), object_id) != inputs.end())
          throw std::invalid_argument("duplicate expected pinned input");
        auto old = find(object_id);
        if (!old) throw std::invalid_argument("expected pinned input is absent");
        inputs.push_back(object_id);
        removed = A::add(removed, old->contribution);
      }

      auto is_input = [&](std::string_view object_id) {
        return std::find(inputs.begin(), inputs.end(), object_id) != inputs.end();
      };
      auto added = A::zero();
      for (std::size_t i = 0; i < replacements.size(); ++i) {
        auto const & item = replacements[i];
        validate(item);
        if (find(item.object_id))
          throw std::invalid_argument("replacement must name a new immutable object");
        for (std::size_t j = 0; j < i; ++j)
          if (replacements[j].object_id == item.object_id)
            throw std::invalid_argument("duplicate replacement object identity");
        added = A::add(added, item.contribution);
      }
      if (added != removed)
        throw std::invalid_argument("replacement changes the pinned contribution sum");

      auto next = std::make_shared<state>();
      next->signature = A::add(A::subtract(state_->signature, removed), added);
      // Keep unrelated entries in their original order. The owner's sum is
      // order independent; semantic recency/order belongs to its consumer.
      for (auto const & item : state_->entries) {
        if (is_input(item.object_id)) continue;
        next->entries.push_back(item);
        next->pins.push_back(item.object);
      }
      for (auto & item : replacements) {
        next->pins.push_back(item.object);
        next->entries.push_back(std::move(item));
      }
      return pin_set(std::move(next));
    }

  private:
    struct state {
      std::vector<entry> entries;
      // Cached projection used by snapshot callers that only need the pins.
      std::vector<pin> pins;
      element signature = A::zero();
    };

    static void validate(entry const & item) {
      if (item.object_id.empty()) throw std::invalid_argument("empty pinned object identity");
      if (!item.object) throw std::invalid_argument("null pinned object");
    }

    explicit pin_set(std::shared_ptr<state const> value) : state_(std::move(value)) {}
    std::shared_ptr<state const> state_;
  };
}
