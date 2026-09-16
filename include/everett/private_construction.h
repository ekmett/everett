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
  // A lock name is removed only after its durable release event: an active
  // scope never has two lock identities. The file contains no values.
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
        remove_name(root_, path());
      } catch (...) {
        // Recovery finds either an active scope or an orphan lease name.
        // Destruction cannot report an uncertain SQLite outcome as a
        // successful cleanup.
      }
      close(lease_);
    }
    catalog_options options() const { return {250, id_}; }
    object_id const & identity() const & noexcept { return id_; }
    object_id const & identity() const && = delete;
    static std::size_t recover(std::filesystem::path const & root) {
      auto location = std::filesystem::canonical(root);
      auto catalog = sqlite_catalog<P>::open(location);
      using state = typename sqlite_catalog<P>::private_scope_state;
      std::size_t count = 0;
      for (auto const & id : catalog.private_scopes()) {
        int fd;
        try { fd = acquire(path(location, id), false); }
        catch (std::system_error const & error) {
          if (error.code() != std::errc::no_such_file_or_directory) throw;
          // Another owner may have released and removed its lease since the
          // initial query. A missing *active* lease remains an error.
          if (catalog.scope_state(id) == state::active) throw;
          continue;
        }
        if (fd < 0) continue; // A live owner still has this exact lease.
        held lease{fd};
        if (catalog.scope_state(id) != state::active) {
          remove_name(location, path(location, id));
          continue;
        }
        catalog.release_private_scope(random_object_ids{}().hex(), id);
        remove_name(location, path(location, id));
        ++count;
      }
      // A crash can leave a name before scope registration or after release.
      // The lock, followed by a fresh catalog read, distinguishes those from
      // live construction. Only canonical lease names belong to this sweep.
      for (auto const & entry : std::filesystem::directory_iterator(location)) {
        auto name = entry.path().filename().string();
        if (name.size() != 46 || !name.starts_with(".private-") || !name.ends_with(".lock")) continue;
        auto hex = std::string_view(name).substr(9, 32);
        if (hex.find_first_not_of("0123456789abcdef") != std::string_view::npos) continue;
        auto id = object_id::from_hex(hex);
        int fd;
        try { fd = acquire(entry.path(), false); }
        catch (std::system_error const & error) {
          if (error.code() == std::errc::no_such_file_or_directory) continue;
          throw;
        }
        if (fd < 0) continue;
        held lease{fd};
        if (catalog.scope_state(id) != state::active) remove_name(location, entry.path());
      }
      return count;
    }
  private:
    std::filesystem::path root_;
    object_id id_;
    int lease_ = -1;
    struct held { int fd; ~held() { private_construction::close(fd); } };
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
      // Recovery can acquire a just-created name before its creator locks it.
      // Never register a scope after that name was removed under the lock.
      if (::fstat(fd, &info) || info.st_nlink != 1) {
        auto error = errno; close(fd);
        throw std::system_error(info.st_nlink != 1 ? ENOENT : error, std::generic_category(),
          "private construction lease has no unique name");
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
    static void remove_name(std::filesystem::path const & root, std::filesystem::path const & path) {
#if defined(__APPLE__) || defined(__linux__)
      if (::unlink(path.c_str()) && errno != ENOENT)
        throw std::system_error(errno, std::generic_category(), "remove released construction lease");
      posix_object_ops ops;
      auto directory = ops.open_root(root);
      if (directory < 0) throw std::system_error(errno, std::generic_category(), "open released lease directory");
      auto result = ops.sync_directory(directory); auto error = errno;
      ops.close(directory);
      if (result) throw std::system_error(error, std::generic_category(), "sync released lease directory");
#else
      (void)root; (void)path;
#endif
    }
  };
}
