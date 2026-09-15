/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Diet's fridge support.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <diet/file.h>
#include <diet/fingerprint.h>
#include <diet/cola_local_merge.h>
#include <diet/file_index_pipeline.h>
#include <diet/mapped_blob.h>
#include <diet/mapped_cola.h>
#include <diet/native_merge.h>
#include <diet/native_file_merge.h>
#include <diet/object_writer.h>
#include <diet/object_stream.h>
#include <diet/query.h>

#include <cerrno>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace diet {
  // Aggregate state/progression runtimes are intentionally incomplete. These
  // declarations carry the family identity without disguising reference_cola
  // as an implemented persistent cola or timeline.
  template <class P> struct cola;
  template <class P> struct timeline;
  template <class P> struct branch_point;
  template <class P, class A, std::uint64_t DepthLimit> struct typed_engine;
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
  template <class P = string_policy> struct fridge {
    using policy_type = P;
    using registry_type = typename P::registry_type;
    using sort = diet::sort<P>;
    using blob = diet::profile_blob<P>;
    using native_array = diet::profile_array<P>;
    using native_writer = diet::profile_native_writer<P>;
    using native_file_writer = diet::native_file_writer<P>;
    template <class Native = native_array, class Compose = replace_native_value>
    using native_merge_builder = diet::native_merge_builder<P, Native, Compose>;
    template <class Native = native_array, class Compose = replace_native_value>
    using native_file_merge = diet::native_file_merge<P, Native, Compose>;
    using index = diet::profile_index<P>;
    template <class Native = native_array>
    using index_builder = diet::index_builder<P, Native>;
    template <class Native = native_array>
    using file_index_builder = diet::file_index_builder<P, Native>;
    using file_index_stage = diet::file_index_stage<P>;
    using file_index_pipeline = diet::file_index_pipeline<P>;
    template <class Target = blob>
    using sample_cursor = diet::sample_cursor<P, Target>;
    using query_root = diet::query_root<P>;
    using query_root_builder = diet::query_root_builder<P>;
    using query_cursor = diet::query_cursor<P>;
    using query_context = diet::profile_query_context<P>;
    using file = diet::file<P>;
    using object_writer = diet::object_writer<P>;
    using object_stream = diet::object_stream<P>;
    using mapped_native = diet::mapped_native<P>;
    using mapped_index = diet::mapped_index<P>;
    using mapped_blob = diet::mapped_blob<P>;
    using mapped_query_root = diet::mapped_query_root<P>;
    using cola_index = diet::cola_index<P>;
    using cola_index_builder = diet::cola_index_builder<P>;
    using cola_destination_plan = diet::cola_destination_plan<P>;
    using cola_local_merge_result = diet::cola_local_merge_result<P>;
    template <class Compose = replace_native_value>
    using cola_local_merge_job = diet::cola_local_merge_job<P, Compose>;
    using cola_query_root = diet::cola_query_root<P>;
    using cola_query_cursor = diet::cola_query_cursor<P>;
    using mapped_cola_index = diet::mapped_cola_index<P>;
    using mapped_cola_blob = diet::mapped_cola_blob<P>;
    using mapped_cola_artifact = diet::mapped_cola_artifact<P>;
    using mapped_cola_index_builder = diet::mapped_cola_index_builder<P>;
    using mapped_cola_query_root = diet::mapped_cola_query_root<P>;
    using cola = diet::cola<P>;
    using timeline = diet::timeline<P>;
    using branch_point = diet::branch_point<P>;
    using active_engine = diet::typed_engine<P, wrapping_fingerprint_algebra, 256>;
    using tap = diet::connection<active_engine>;

    explicit fridge(std::filesystem::path root) : root_(checked_root(std::move(root))) {}

    // Establish missing directory names with a barrier on each new directory
    // and its parent. Existing ancestors must already be durable and trusted.
    // A failed barrier leaves the names in place for explicit recovery.
    static fridge create(std::filesystem::path root) {
      if (root.empty()) throw std::invalid_argument("fridge root must name a directory");
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
          if (fd < 0) throw std::system_error(errno, std::generic_category(), "open fridge directory");
        }
        descriptor(descriptor const &) = delete;
        ~descriptor() { if (fd >= 0) ::close(fd); }
      } parent(ops.open_root(ancestor));
      for (auto name = missing.rbegin(); name != missing.rend(); ++name) {
        if (ops.make_directory(parent.fd, name->c_str()) && errno != EEXIST)
          throw std::system_error(errno, std::generic_category(), "create fridge directory");
        descriptor child(ops.open_directory(parent.fd, name->c_str()));
        if (ops.sync_directory(child.fd) || ops.sync_directory(parent.fd))
          throw std::system_error(errno, std::generic_category(), "sync fridge directory; creation outcome unknown");
        if (ops.close(std::exchange(parent.fd, -1)))
          throw std::system_error(errno, std::generic_category(), "close fridge directory; creation outcome unknown");
        parent.fd = std::exchange(child.fd, -1);
      }
      if (ops.close(std::exchange(parent.fd, -1)))
        throw std::system_error(errno, std::generic_category(), "close fridge directory; creation outcome unknown");
      return fridge(path);
#else
      throw std::system_error(std::make_error_code(std::errc::operation_not_supported), "create fridge directory");
#endif
    }

    std::filesystem::path const & root() const & noexcept { return root_; }
    std::filesystem::path const & root() const && = delete;

    // Include diet/connection.h and link diet::sqlite for these operations.
    tap connect(std::string_view name) const;
    tap connect(std::string_view name, connection_options const & options) const;

    file open_object(object_id const & id, file_kind kind, file_open_mode mode = file_open_mode::checked) const {
      auto result = file::open(root_ / object_path(id, kind), mode);
      if (mode == file_open_mode::checked && result.header().kind != kind)
        throw std::invalid_argument("unexpected Diet object kind");
      return result;
    }

    mapped_query_root open_query(blob_identity const & head) const {
      return diet::open_mapped_query<P>(root_, head);
    }
    mapped_cola_query_root open_cola_query(blob_identity const & head) const {
      return diet::open_mapped_cola_query<P>(root_, head);
    }

    // The caller establishes root durability and reserves both identities.
    // Sealing acknowledges object persistence operations, not cola adoption.
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
      if (root.empty()) throw std::invalid_argument("fridge root must name an existing directory");
      auto absolute = std::filesystem::canonical(root);
      if (!std::filesystem::is_directory(absolute))
        throw std::invalid_argument("fridge root is not a directory");
      return absolute;
    }
    std::filesystem::path root_;
  };
}
