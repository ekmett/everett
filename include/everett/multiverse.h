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
#include <everett/mapped_blob.h>
#include <everett/native_merge.h>
#include <everett/object_writer.h>
#include <everett/object_stream.h>
#include <everett/query.h>

#include <filesystem>
#include <span>
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
      if (code_.bit_size & (P::bits_per_unit - 1))
        throw std::invalid_argument("sort code does not match policy units");
    }
    bit_view code() const & { return code_.view(); }
    bit_view code() const && = delete;
    bool operator==(sort const &) const = default;

  private:
    bit_string code_;
  };

  // Holds an existing canonical object root. Reads check envelopes by default;
  // sealing creates immutable files under caller-reserved identities. The
  // optional SQLite catalog owns persistent roots and reservations separately.
  // Files/slices retain their mappings independently of this path holder.
  template <class P> struct multiverse {
    using policy_type = P;
    using sort = everett::sort<P>;
    using blob = everett::profile_blob<P>;
    using native_array = everett::profile_array<P>;
    using native_writer = everett::profile_native_writer<P>;
    template <class Native = native_array, class Compose = replace_native_value>
    using native_merge_builder = everett::native_merge_builder<P, Native, Compose>;
    using query_root = everett::query_root<P>;
    using query_root_builder = everett::query_root_builder<P>;
    using query_cursor = everett::query_cursor<P>;
    using query_context = everett::profile_query_context<P>;
    using file = everett::file<P>;
    using object_writer = everett::object_writer<P>;
    using object_stream = everett::object_stream<P>;
    using mapped_native = everett::mapped_native<P>;
    using mapped_index = everett::mapped_index<P>;
    using mapped_blob = everett::mapped_blob<P>;
    using mapped_query_root = everett::mapped_query_root<P>;
    using world = everett::world<P>;
    using timeline = everett::timeline<P>;
    using branch_point = everett::branch_point<P>;

    explicit multiverse(std::filesystem::path root) : root_(checked_root(std::move(root))) {}

    std::filesystem::path const & root() const & noexcept { return root_; }
    std::filesystem::path const & root() const && = delete;

    file open_object(object_id const & id, file_kind kind, file_open_mode mode = file_open_mode::checked) const {
      auto result = file::open(root_ / object_path(id, kind), mode);
      if (mode == file_open_mode::checked && result.header().kind != kind)
        throw std::invalid_argument("unexpected Everett object kind");
      return result;
    }

    mapped_query_root open_query(blob_identity const & head) const {
      return everett::open_mapped_query<P>(root_, head);
    }

    // The caller establishes root durability and reserves both identities.
    // Sealing acknowledges object persistence operations, not world adoption.
    object_seal_receipt seal_object(object_id const & id, object_attempt_id const & attempt,
        file_header<P> const & header, std::span<std::byte const> body) const {
      return object_writer::seal(root_, id, attempt, header, body);
    }
    object_seal_receipt seal_object(object_id const & id, object_attempt_id const & attempt,
        file_header<P> const & header, std::span<std::span<std::byte const> const> chunks) const {
      return object_writer::seal(root_, id, attempt, header, chunks);
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
