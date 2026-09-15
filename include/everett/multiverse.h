/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/file.h>
#include <everett/profile_blob.h>

#include <filesystem>
#include <stdexcept>
#include <utility>

namespace everett {
  // Aggregate state/progression runtimes are intentionally incomplete. These
  // declarations carry the family identity without disguising reference_world
  // as an implemented persistent world or timeline.
  template <class P> struct world;
  template <class P> struct timeline;
  template <class P> struct branch_point;

  // An individual policy-bound sort code. Construction validates its packed
  // representation and unit alignment only. The owner of a sort-code family
  // must additionally establish uniqueness and prefix freedom, and pin its
  // interpretation version. No hash/category registry is implemented here.
  template <class P> struct sort {
    using policy_type = P;

    explicit sort(bit_string code) : code_(std::move(code)) {
      code_.validate();
      if (code_.bit_size % P::bits_per_unit)
        throw std::invalid_argument("sort code does not match policy units");
    }
    bit_view code() const & { return code_.view(); }
    bit_view code() const && = delete;
    bool operator==(sort const &) const = default;

  private:
    bit_string code_;
  };

  // The working read side of the backing store. It holds a canonical existing
  // root and opens policy-checked immutable object envelopes; it does not make
  // directories, allocate IDs, update a metadata catalog, publish worlds or reclaim data.
  // SQLite world/pin/progress metadata integration remains separate work.
  // Files/slices retain their mappings independently of this path holder.
  template <class P> struct multiverse {
    using policy_type = P;
    using sort = everett::sort<P>;
    using blob = everett::profile_blob<P>;
    using file = everett::file<P>;
    using world = everett::world<P>;
    using timeline = everett::timeline<P>;
    using branch_point = everett::branch_point<P>;

    explicit multiverse(std::filesystem::path root) : root_(checked_root(std::move(root))) {}

    std::filesystem::path const & root() const & noexcept { return root_; }
    std::filesystem::path const & root() const && = delete;

    file open_object(object_id const & id, file_kind kind) const {
      auto result = file::open(root_ / object_path(id, kind));
      if (result.header().kind != kind) throw std::invalid_argument("unexpected Everett object kind");
      return result;
    }

  private:
    static std::filesystem::path checked_root(std::filesystem::path root) {
      if (root.empty()) throw std::invalid_argument("multiverse root must name an existing directory");
      auto absolute = std::filesystem::canonical(root);
      if (!std::filesystem::is_directory(absolute))
        throw std::invalid_argument("multiverse root is not a directory");
      return absolute;
    }
    std::filesystem::path root_;
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Everett's multiverse support.
 */
