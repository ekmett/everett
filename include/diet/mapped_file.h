/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Diet's mapped file support.
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#define DIET_MAPPED_FILE_UNDEF_NOMINMAX
#endif
#include <windows.h>
#ifdef DIET_MAPPED_FILE_UNDEF_NOMINMAX
#undef DIET_MAPPED_FILE_UNDEF_NOMINMAX
#undef NOMINMAX
#endif
#elif defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#error "Diet mapped_file currently supports Windows and POSIX platforms"
#endif

namespace diet {
  namespace mapped_file_detail {
    struct region {
      void const * data = nullptr;
      std::uint64_t length = 0;
#if defined(_WIN32)
      HANDLE file = INVALID_HANDLE_VALUE;
      HANDLE mapping = nullptr;
#else
      int file = -1;
#endif
      region() = default;
      region(region const &) = delete;
      region & operator=(region const &) = delete;
      ~region() noexcept {
#if defined(_WIN32)
        if (data) ::UnmapViewOfFile(data);
        if (mapping) ::CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) ::CloseHandle(file);
#else
        if (data) ::munmap(const_cast<void *>(data), static_cast<std::size_t>(length));
        if (file != -1) ::close(file);
#endif
      }
    };

    inline void check_slice(std::uint64_t size, std::uint64_t offset, std::uint64_t length) {
      if (offset > size || length > size - offset)
        throw std::out_of_range("mapped slice exceeds file extent");
    }
  }

  struct mapped_file;

  // Owns a mapping pin as well as a bounded range. Keep this object alive for
  // every span obtained from bytes(); the span alone does not own a pin.
  struct mapped_slice {
    mapped_slice() = default;
    mapped_slice(mapped_slice const &) = default;
    mapped_slice & operator=(mapped_slice const &) = default;
    mapped_slice(mapped_slice && other) noexcept
      : owner_(std::move(other.owner_)), offset_(std::exchange(other.offset_, 0)),
        length_(std::exchange(other.length_, 0)) {}
    mapped_slice & operator=(mapped_slice && other) noexcept {
      if (this != &other) {
        owner_ = std::move(other.owner_);
        offset_ = std::exchange(other.offset_, 0);
        length_ = std::exchange(other.length_, 0);
      }
      return *this;
    }
    std::uint64_t size() const noexcept { return length_; }
    bool empty() const noexcept { return length_ == 0; }

    std::span<std::byte const> bytes() const & noexcept {
      auto data = owner_ ? static_cast<std::byte const *>(owner_->data) : nullptr;
      return {data ? data + static_cast<std::size_t>(offset_) : nullptr,
              static_cast<std::size_t>(length_)};
    }
    std::span<std::byte const> bytes() const && = delete;

    mapped_slice slice(std::uint64_t offset, std::uint64_t length) const {
      mapped_file_detail::check_slice(length_, offset, length);
      // Both offsets were already bounded by the mapping, so this addition
      // cannot exceed the checked whole-file length or wrap.
      return {owner_, offset_ + offset, length};
    }

  private:
    friend struct mapped_file;
    mapped_slice(std::shared_ptr<mapped_file_detail::region const> owner,
                 std::uint64_t offset, std::uint64_t length)
      : owner_(std::move(owner)), offset_(offset), length_(length) {}
    std::shared_ptr<mapped_file_detail::region const> owner_;
    std::uint64_t offset_ = 0;
    std::uint64_t length_ = 0;
  };

  // Read-only whole-file mapping, with native POSIX and Windows branches.
  // The caller must keep the backing object immutable while any pin lives:
  // external modification/truncation is not made safe by MAP_PRIVATE or by a
  // shared_ptr. File type and size are checked on the opened handle, not via
  // a separate path stat. This read backend makes no crash-durability claim.
  struct mapped_file {
    mapped_file() = default;

    static mapped_file open(std::filesystem::path const & path) {
      auto state = std::make_shared<mapped_file_detail::region>();
#if defined(_WIN32)
      state->file = ::CreateFileW(path.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
      if (state->file == INVALID_HANDLE_VALUE)
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "open mapped file");
      BY_HANDLE_FILE_INFORMATION info{};
      if (!::GetFileInformationByHandle(state->file, &info))
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "inspect mapped file");
      if (::GetFileType(state->file) != FILE_TYPE_DISK || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        throw std::invalid_argument("mapped file must be a regular file");
      LARGE_INTEGER size{};
      if (!::GetFileSizeEx(state->file, &size))
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "size mapped file");
      if (size.QuadPart < 0) throw std::length_error("negative mapped file size");
      state->length = static_cast<std::uint64_t>(size.QuadPart);
#else
      // Nonblocking open permits rejecting FIFOs rather than waiting for a
      // writer before fstat can establish that the object is not regular.
      int flags = O_RDONLY | O_NONBLOCK;
#ifdef O_CLOEXEC
      flags |= O_CLOEXEC;
#endif
      state->file = ::open(path.c_str(), flags);
      if (state->file == -1) throw std::system_error(errno, std::generic_category(), "open mapped file");
#ifndef O_CLOEXEC
      if (::fcntl(state->file, F_SETFD, FD_CLOEXEC) == -1)
        throw std::system_error(errno, std::generic_category(), "protect mapped descriptor");
#endif
      struct stat info{};
      if (::fstat(state->file, &info) == -1)
        throw std::system_error(errno, std::generic_category(), "inspect mapped file");
      if (!S_ISREG(info.st_mode)) throw std::invalid_argument("mapped file must be a regular file");
      if (info.st_size < 0) throw std::length_error("negative mapped file size");
      state->length = static_cast<std::uint64_t>(info.st_size);
#endif
      if (state->length > (std::numeric_limits<std::size_t>::max)())
        throw std::length_error("mapped file exceeds addressable span size");
      if (state->length) {
#if defined(_WIN32)
        state->mapping = ::CreateFileMappingW(state->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!state->mapping)
          throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "create file mapping");
        state->data = ::MapViewOfFile(state->mapping, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(state->length));
        if (!state->data)
          throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "map file view");
#else
        auto address = ::mmap(nullptr, static_cast<std::size_t>(state->length), PROT_READ, MAP_PRIVATE, state->file, 0);
        if (address == MAP_FAILED) throw std::system_error(errno, std::generic_category(), "map file view");
        state->data = address;
#endif
      }
      return mapped_file(std::move(state));
    }

    std::uint64_t size() const noexcept { return owner_ ? owner_->length : 0; }
    mapped_slice slice(std::uint64_t offset, std::uint64_t length) const {
      mapped_file_detail::check_slice(size(), offset, length);
      return {owner_, offset, length};
    }

  private:
    explicit mapped_file(std::shared_ptr<mapped_file_detail::region const> owner) : owner_(std::move(owner)) {}
    std::shared_ptr<mapped_file_detail::region const> owner_;
  };
}
