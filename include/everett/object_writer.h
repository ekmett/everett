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

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#if defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace everett {
  // A caller-reserved, never-reused attempt identity, distinct from the opaque
  // physical object identity. Neither identity is computed from content here.
  struct object_attempt_id {
    explicit object_attempt_id(std::string hex) : value_(std::move(hex)) {}
    std::string const & hex() const noexcept { return value_.hex(); }
    bool operator==(object_attempt_id const &) const = default;
  private:
    object_id value_;
  };

  enum struct object_sync_barrier { fsync, apple_full_fsync };
  enum struct object_write_stage {
    starting, private_created, body_written, content_synced, name_installed,
    names_synced, private_removed, complete
  };

  struct object_write_paths {
    std::filesystem::path final;
    std::filesystem::path private_output;
  };

  inline object_write_paths object_output_paths(std::filesystem::path const & root,
      object_id const & id, object_attempt_id const & attempt, file_kind kind) {
    auto final = root / object_path(id, kind);
    auto temporary = final.parent_path() /
      ("." + final.filename().string() + ".attempt-" + attempt.hex());
    return {std::move(final), std::move(temporary)};
  }

  // Retain these identities and any surviving names after an error. The stage
  // records the last acknowledged operation, not the durable outcome. There is
  // deliberately no retry, adopt-existing, cached-readback recovery or cleanup
  // method. Recovery and durable identity reservation belong to the caller.
  struct object_write_error : std::system_error {
    object_write_error(int error, char const * operation, object_id object,
        object_attempt_id attempt, object_write_paths paths, object_write_stage stage)
      : std::system_error(error, std::generic_category(), operation),
        object(std::move(object)), attempt(std::move(attempt)), paths(std::move(paths)),
        stage(stage), operation(operation) {}
    object_id object;
    object_attempt_id attempt;
    object_write_paths paths;
    object_write_stage stage;
    char const * operation;
  };

  // Reports successful envelope writes and the requested OS persistence calls
  // for this object and its names under root. Not a content address, catalog
  // adoption, allocator reservation, recovery-root receipt or power-loss test.
  struct object_seal_receipt {
    object_id object;
    object_attempt_id attempt;
    std::filesystem::path path;
    std::uint64_t bytes;
    std::uint32_t body_crc32c;
    object_sync_barrier barrier;
  };

  // Bounded syscall seam, with POSIX return values and errno. Custom Ops use
  // the same contract; methods must not throw. No filesystem recovery policy
  // hides behind this interface. Windows and other platforms are unsupported.
  struct posix_object_ops {
#if defined(__APPLE__) || defined(__linux__)
    static constexpr bool supported = sizeof(off_t) >= sizeof(std::int64_t);
    object_sync_barrier barrier() const noexcept {
#if defined(__APPLE__)
      return object_sync_barrier::apple_full_fsync;
#else
      return object_sync_barrier::fsync;
#endif
    }
    int open_root(std::filesystem::path const & path) noexcept {
      return ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    }
    int make_directory(int parent, char const * name) noexcept {
      return ::mkdirat(parent, name, 0700);
    }
    int open_directory(int parent, char const * name) noexcept {
      return ::openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    }
    int create_private(int parent, char const * name) noexcept {
      return ::openat(parent, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    }
    std::ptrdiff_t write(int fd, std::span<std::byte const> bytes) noexcept {
      return ::write(fd, bytes.data(), bytes.size());
    }
    std::ptrdiff_t write_at(int fd, std::span<std::byte const> bytes, std::uint64_t at) noexcept {
      return ::pwrite(fd, bytes.data(), bytes.size(), static_cast<off_t>(at));
    }
    int make_read_only(int fd) noexcept { return ::fchmod(fd, 0400); }
    int sync_file(int fd) noexcept {
#if defined(__APPLE__)
      // No fsync fallback: unsupported full barriers fail the attempt.
      return ::fcntl(fd, F_FULLFSYNC);
#else
      return ::fsync(fd);
#endif
    }
    int sync_directory(int fd) noexcept { return ::fsync(fd); }
    int install(int parent, char const * from, char const * to) noexcept {
      return ::linkat(parent, from, parent, to, 0);
    }
    int remove_private(int parent, char const * name) noexcept { return ::unlinkat(parent, name, 0); }
    int close(int fd) noexcept { return ::close(fd); }
#else
    static constexpr bool supported = false;
#endif
  };

  // Seal one supplied object into an existing, durably established root. The
  // caller reserves both identities and retains verified input ownership until
  // later catalog adoption/recovery. Input spans stay readable and immutable
  // throughout this call (including mmap-backed bodies). Only 96 header bytes
  // are buffered; chunks need not be contiguous and empty chunks are allowed.
  //
  // Root and shards must share one supported local filesystem/device. Root
  // ancestry and that filesystem are trusted: no concurrent shard renames,
  // mount changes, in-place object mutation or hostile directory manipulation.
  // Component handles reject shard symlinks; they are not a sandbox against
  // mutation of the supplied root's ancestors. The filesystem must support
  // hard links and directory fsync. Sealed means our writer stops mutating it;
  // read-only permissions do not prevent an owner from changing them later.
  template <class P, class Ops = posix_object_ops> struct object_writer {
    using policy_type = P;

    static object_seal_receipt seal(std::filesystem::path const & root,
        object_id const & id, object_attempt_id const & attempt, file_header<P> const & header,
        std::span<std::span<std::byte const> const> chunks, Ops & ops) {
      if constexpr (!Ops::supported) {
        (void)root; (void)id; (void)attempt; (void)header; (void)chunks; (void)ops;
        throw std::system_error(std::make_error_code(std::errc::operation_not_supported),
                                "Everett object writer is unsupported on this platform");
      } else {
        file_detail::validate_metadata(header);
        auto body_size = file_detail::body_bytes<P>(header.extent);
        auto total_size = file_detail::total_bytes<P>(header.extent);
        // POSIX file offsets are signed. The production implementations use
        // 64-bit off_t; this stricter common bound also bounds write progress.
        if (total_size > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
          throw std::length_error("Everett object exceeds supported file offsets");
        std::uint64_t supplied = 0;
        std::byte last{};
        for (auto chunk : chunks) {
          if (chunk.size() > body_size - supplied)
            throw std::invalid_argument("Everett body extent mismatch");
          supplied += chunk.size();
          if (!chunk.empty()) last = chunk.back();
        }
        if (supplied != body_size) throw std::invalid_argument("Everett body extent mismatch");
        if constexpr (P::unit == profile_unit::bit)
          if ((header.extent & 7) && (std::to_integer<unsigned>(last) & ((1u << (8 - (header.extent & 7))) - 1)))
            throw std::invalid_argument("noncanonical bit-profile tail padding");

        operation run{root, id, attempt, header.kind, ops};
        run.open();
        std::array<std::byte, file_detail::header_bytes> placeholder{};
        run.write_all(placeholder);
        std::uint32_t crc = 0;
        for (auto chunk : chunks) {
          // Limit each CRC/write unit independently of the caller's span size.
          // This is work streaming, not an allocated copy of the encoded body.
          while (!chunk.empty()) {
            auto part = chunk.first(std::min(chunk.size(), std::size_t{1} << 20));
            run.write_all(part);
            crc = crc32c(part, crc);
            chunk = chunk.subspan(part.size());
          }
        }
        auto encoded_header = encode_file_header(header, crc);
        run.write_header(encoded_header);
        run.stage = object_write_stage::body_written;
        run.finish();
        return {id, attempt, run.paths.final, total_size, crc, ops.barrier()};
      }
    }

    static object_seal_receipt seal(std::filesystem::path const & root,
        object_id const & id, object_attempt_id const & attempt, file_header<P> const & header,
        std::span<std::span<std::byte const> const> chunks) {
      Ops ops;
      return seal(root, id, attempt, header, chunks, ops);
    }
    static object_seal_receipt seal(std::filesystem::path const & root,
        object_id const & id, object_attempt_id const & attempt, file_header<P> const & header,
        std::span<std::byte const> body, Ops & ops) {
      return seal(root, id, attempt, header, std::span{&body, 1}, ops);
    }
    static object_seal_receipt seal(std::filesystem::path const & root,
        object_id const & id, object_attempt_id const & attempt, file_header<P> const & header,
        std::span<std::byte const> body) {
      Ops ops;
      return seal(root, id, attempt, header, body, ops);
    }

  private:
    struct operation {
      std::filesystem::path const & root;
      object_id const & id;
      object_attempt_id const & attempt;
      Ops & ops;
      object_write_paths paths;
      std::string first, second, final_name, private_name;
      std::array<int, 4> fds{-1, -1, -1, -1}; // root, first shard, leaf, object
      object_write_stage stage = object_write_stage::starting;

      operation(std::filesystem::path const & root, object_id const & id,
          object_attempt_id const & attempt, file_kind kind, Ops & ops)
        : root(root), id(id), attempt(attempt), ops(ops),
          paths(object_output_paths(root, id, attempt, kind)),
          first(id.hex().substr(0, 2)), second(id.hex().substr(2, 2)),
          final_name(paths.final.filename().string()), private_name(paths.private_output.filename().string()) {}
      operation(operation const &) = delete;
      operation & operator=(operation const &) = delete;
      ~operation() {
        // Error paths close handles once but never unlink outputs or retry any
        // failed I/O. Preserve the first failure; close can itself lose an ack.
        for (auto it = fds.rbegin(); it != fds.rend(); ++it)
          if (*it >= 0) (void)ops.close(std::exchange(*it, -1));
      }
      [[noreturn]] void fail(char const * what, int error = errno) const {
        throw object_write_error(error ? error : EIO, what, id, attempt, paths, stage);
      }
      void open() {
        fds[0] = ops.open_root(root);
        if (fds[0] < 0) fail("open object root");
        for (unsigned i = 1; i <= 2; ++i) {
          auto const & name = i == 1 ? first : second;
          if (ops.make_directory(fds[i - 1], name.c_str()) < 0 && errno != EEXIST)
            fail("create object shard");
          fds[i] = ops.open_directory(fds[i - 1], name.c_str());
          if (fds[i] < 0) fail("open object shard");
        }
        fds[3] = ops.create_private(fds[2], private_name.c_str());
        if (fds[3] < 0) fail("create private object");
        stage = object_write_stage::private_created;
      }
      void write_all(std::span<std::byte const> bytes) {
        while (!bytes.empty()) {
          auto part = bytes.first(std::min(bytes.size(), std::size_t{1} << 20));
          auto count = ops.write(fds[3], part);
          if (count < 0) { if (errno == EINTR) continue; fail("write object body"); }
          if (count == 0 || std::uint64_t(count) > part.size()) fail("write object body", EIO);
          bytes = bytes.subspan(static_cast<std::size_t>(count));
        }
      }
      void write_header(std::span<std::byte const> bytes) {
        std::uint64_t offset = 0;
        while (!bytes.empty()) {
          auto count = ops.write_at(fds[3], bytes, offset);
          if (count < 0) { if (errno == EINTR) continue; fail("write object header"); }
          if (count == 0 || std::uint64_t(count) > bytes.size()) fail("write object header", EIO);
          offset += static_cast<std::uint64_t>(count);
          bytes = bytes.subspan(static_cast<std::size_t>(count));
        }
      }
      void finish() {
        if (ops.make_read_only(fds[3]) < 0) fail("protect object");
        // No retry after synchronization failure, even EINTR.
        if (ops.sync_file(fds[3]) < 0) fail("sync object contents");
        stage = object_write_stage::content_synced;
        if (ops.install(fds[2], private_name.c_str(), final_name.c_str()) < 0)
          fail("install object without replacement");
        stage = object_write_stage::name_installed;
        // Sync existing ancestors too: another legitimate creator may have
        // just made them. Root's own durable parent entry is a caller premise.
        for (unsigned i = 3; i-- > 0;)
          if (ops.sync_directory(fds[i]) < 0) fail("sync object directory");
        stage = object_write_stage::names_synced;
        if (ops.remove_private(fds[2], private_name.c_str()) < 0) fail("remove private object name");
        stage = object_write_stage::private_removed;
        if (ops.sync_directory(fds[2]) < 0) fail("sync private name removal");
        // Also persists link-count changes; on Apple this full drive barrier
        // follows the directory writeback requests. No unsupported fallback.
        if (ops.sync_file(fds[3]) < 0) fail("sync installed object");
        for (unsigned i = 4; i-- > 0;) {
          auto fd = std::exchange(fds[i], -1);
          if (ops.close(fd) < 0) fail("close sealed object handles");
        }
        stage = object_write_stage::complete;
      }
    };
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Streams and seals immutable object files without catalog publication.
 */
