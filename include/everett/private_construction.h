/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Keeps private transaction construction pinned until its last owner or crash recovery.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <everett/runtime_store.h>

#include <memory>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/file.h>
#endif

namespace everett {
  // Share the lease with every snapshot and worker that can still use the
  // private frontier. A different process may recover only an unlocked lease.
  // Lock files are retained: unlinking them would permit two lock identities
  // for one scope. They contain no values or transaction state.
  template <class P> struct private_construction {
    static std::shared_ptr<private_construction> create(std::filesystem::path const & root) {
      auto result = std::shared_ptr<private_construction>(new private_construction(
        std::filesystem::canonical(root), random_object_ids{}()));
      result->lease_ = acquire(result->path(), true);
      sync_name(result->root_, result->lease_);
      auto catalog = sqlite_catalog<P>::open(result->root_);
      catalog.begin_private_scope(random_object_ids{}().hex(), result->id_);
      return result;
    }
    private_construction(private_construction const &) = delete;
    private_construction & operator=(private_construction const &) = delete;
    ~private_construction() {
      if (lease_ < 0) return;
      try {
        auto catalog = sqlite_catalog<P>::open(root_);
        catalog.release_private_scope(random_object_ids{}().hex(), id_);
      } catch (...) {
        // The durable scope remains discoverable. Recovery retries release
        // after this process lease has gone; destruction cannot report an
        // uncertain SQLite outcome as a successful cleanup.
      }
      close(lease_);
    }
    catalog_options options() const { return {250, id_}; }
    object_id const & identity() const & noexcept { return id_; }
    object_id const & identity() const && = delete;
    static std::size_t recover(std::filesystem::path const & root) {
      auto location = std::filesystem::canonical(root);
      auto catalog = sqlite_catalog<P>::open(location);
      std::size_t count = 0;
      for (auto const & id : catalog.private_scopes()) {
        auto fd = acquire(path(location, id), false);
        if (fd < 0) continue; // A live owner still has this exact lease.
        struct held { int fd; ~held() { private_construction::close(fd); } } lease{fd};
        catalog.release_private_scope(random_object_ids{}().hex(), id);
        ++count;
      }
      return count;
    }
  private:
    std::filesystem::path root_;
    object_id id_;
    int lease_ = -1;
    private_construction(std::filesystem::path root, object_id id)
      : root_(std::move(root)), id_(std::move(id)) {}
    static std::filesystem::path path(std::filesystem::path const & root, object_id const & id) {
      return root / (".private-" + id.hex() + ".lock");
    }
    std::filesystem::path path() const { return path(root_, id_); }
    static void close(int fd) noexcept {
#if defined(__APPLE__) || defined(__linux__)
      (void)::close(fd);
#else
      (void)fd;
#endif
    }
    static int acquire(std::filesystem::path const & path, bool create) {
#if defined(__APPLE__) || defined(__linux__)
      auto fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | (create ? O_CREAT | O_EXCL : 0), 0600);
      if (fd < 0) throw std::system_error(errno, std::generic_category(), "open private construction lease");
      struct stat info{};
      if (::fstat(fd, &info) || !S_ISREG(info.st_mode)) {
        auto error = errno ? errno : EINVAL; close(fd);
        throw std::system_error(error, std::generic_category(), "private construction lease is not regular");
      }
      if (::flock(fd, LOCK_EX | LOCK_NB)) {
        auto error = errno; close(fd);
        if (!create && (error == EWOULDBLOCK || error == EAGAIN)) return -1;
        throw std::system_error(error, std::generic_category(), "lock private construction lease");
      }
      return fd;
#else
      (void)path; (void)create;
      throw std::system_error(std::make_error_code(std::errc::operation_not_supported), "private construction leases require POSIX");
#endif
    }
    static void sync_name(std::filesystem::path const & root, int fd) {
#if defined(__APPLE__) || defined(__linux__)
      if (::fsync(fd)) throw std::system_error(errno, std::generic_category(), "sync private construction lease");
      posix_object_ops ops;
      auto directory = ops.open_root(root);
      if (directory < 0) throw std::system_error(errno, std::generic_category(), "open construction lease directory");
      auto result = ops.sync_directory(directory); auto error = errno;
      ops.close(directory);
      if (result) throw std::system_error(error, std::generic_category(), "sync construction lease directory");
#else
      (void)root; (void)fd;
#endif
    }
  };
}
