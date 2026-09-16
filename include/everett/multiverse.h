/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Everett's multiverse support.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/file.h>
#include <everett/fingerprint.h>
#include <everett/cola_local_merge.h>
#include <everett/file_index_pipeline.h>
#include <everett/mapped_blob.h>
#include <everett/mapped_cola.h>
#include <everett/native_merge.h>
#include <everett/native_file_merge.h>
#include <everett/object_writer.h>
#include <everett/object_stream.h>
#include <everett/query.h>

#include <cerrno>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace everett {
  // Aggregate state/progression runtimes are intentionally incomplete. These
  // declarations carry the family identity without disguising reference_world
  // as an implemented persistent world or timeline.
  template <class P> struct world;
  template <class P> struct timeline;
  template <class P> struct branch_point;
  template <class P> struct active_engine;
  template <class Core> struct connection;
  struct connection_options;

  // An individual policy-bound sort code. Construction validates its packed
  // representation and unit alignment only. The registry establishes the
  // family's prefix freedom and selects its record and semantic handlers.
  template <class P> struct sort {
    using policy_type = P;
    using registry_type = typename P::registry_type;

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
  template <class P = string_policy> struct multiverse {
    using policy_type = P;
    using registry_type = typename P::registry_type;
    using sort = everett::sort<P>;
    using blob = everett::profile_blob<P>;
    using native_array = everett::profile_array<P>;
    using native_writer = everett::profile_native_writer<P>;
    using native_file_writer = everett::native_file_writer<P>;
    template <class Native = native_array, class Compose = replace_native_value>
    using native_merge_builder = everett::native_merge_builder<P, Native, Compose>;
    template <class Native = native_array, class Compose = replace_native_value>
    using native_file_merge = everett::native_file_merge<P, Native, Compose>;
    using index = everett::profile_index<P>;
    template <class Native = native_array>
    using index_builder = everett::index_builder<P, Native>;
    template <class Native = native_array>
    using file_index_builder = everett::file_index_builder<P, Native>;
    using file_index_stage = everett::file_index_stage<P>;
    using file_index_pipeline = everett::file_index_pipeline<P>;
    template <class Target = blob>
    using sample_cursor = everett::sample_cursor<P, Target>;
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
    using cola_index = everett::cola_index<P>;
    using cola_index_builder = everett::cola_index_builder<P>;
    using cola_destination_plan = everett::cola_destination_plan<P>;
    using cola_local_merge_result = everett::cola_local_merge_result<P>;
    template <class Compose = replace_native_value>
    using cola_local_merge_job = everett::cola_local_merge_job<P, Compose>;
    using cola_query_root = everett::cola_query_root<P>;
    using cola_query_cursor = everett::cola_query_cursor<P>;
    using mapped_cola_index = everett::mapped_cola_index<P>;
    using mapped_cola_blob = everett::mapped_cola_blob<P>;
    using mapped_cola_artifact = everett::mapped_cola_artifact<P>;
    using mapped_cola_index_builder = everett::mapped_cola_index_builder<P>;
    using mapped_cola_query_root = everett::mapped_cola_query_root<P>;
    using world = everett::world<P>;
    using timeline = everett::timeline<P>;
    using branch_point = everett::branch_point<P>;
    using active_engine = everett::active_engine<P>;
    using session = everett::connection<active_engine>;

    explicit multiverse(std::filesystem::path root) : root_(checked_root(std::move(root))) {}

    // Establish missing directory names with a barrier on each new directory
    // and its parent. Existing ancestors must already be durable and trusted.
    // A failed barrier leaves the names in place for explicit recovery.
    static multiverse create(std::filesystem::path root) {
      if (root.empty()) throw std::invalid_argument("multiverse root must name a directory");
#if defined(__APPLE__) || defined(__linux__)
      auto path = std::filesystem::weakly_canonical(std::filesystem::absolute(root));
      auto ancestor = path;
      std::vector<std::string> missing;
      while (!std::filesystem::exists(ancestor)) {
        missing.push_back(ancestor.filename().string());
        ancestor = ancestor.parent_path();
      }
      ancestor = checked_root(ancestor);
      posix_object_ops ops;
      struct descriptor {
        int fd;
        explicit descriptor(int value) : fd(value) {
          if (fd < 0) throw std::system_error(errno, std::generic_category(), "open multiverse directory");
        }
        descriptor(descriptor const &) = delete;
        ~descriptor() { if (fd >= 0) ::close(fd); }
      } parent(ops.open_root(ancestor));
      for (auto name = missing.rbegin(); name != missing.rend(); ++name) {
        if (ops.make_directory(parent.fd, name->c_str()) && errno != EEXIST)
          throw std::system_error(errno, std::generic_category(), "create multiverse directory");
        descriptor child(ops.open_directory(parent.fd, name->c_str()));
        if (ops.sync_directory(child.fd) || ops.sync_directory(parent.fd))
          throw std::system_error(errno, std::generic_category(), "sync multiverse directory; creation outcome unknown");
        if (ops.close(std::exchange(parent.fd, -1)))
          throw std::system_error(errno, std::generic_category(), "close multiverse directory; creation outcome unknown");
        parent.fd = std::exchange(child.fd, -1);
      }
      if (ops.close(std::exchange(parent.fd, -1)))
        throw std::system_error(errno, std::generic_category(), "close multiverse directory; creation outcome unknown");
      return multiverse(path);
#else
      throw std::system_error(std::make_error_code(std::errc::operation_not_supported), "create multiverse directory");
#endif
    }

    std::filesystem::path const & root() const & noexcept { return root_; }
    std::filesystem::path const & root() const && = delete;

    // Include everett/connection.h and link everett::sqlite for these operations.
    session connect(std::string_view name) const;
    session connect(std::string_view name, connection_options const & options) const;
    std::size_t recover_transactions() const;

    file open_object(object_id const & id, file_kind kind, file_open_mode mode = file_open_mode::checked) const {
      auto result = file::open(root_ / object_path(id, kind), mode);
      if (mode == file_open_mode::checked && result.header().kind != kind)
        throw std::invalid_argument("unexpected Everett object kind");
      return result;
    }

    mapped_query_root open_query(blob_identity const & head) const {
      return everett::open_mapped_query<P>(root_, head);
    }
    mapped_cola_query_root open_cola_query(blob_identity const & head) const {
      return everett::open_mapped_cola_query<P>(root_, head);
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
