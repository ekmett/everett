/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Exercises object sealing, failure retention and real filesystem reads.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/object_writer.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/mman.h>
#endif

namespace {
  using namespace diet;
  using policy = storage_policy<profile_unit::byte, variable_values, 3>;
  using bit_policy = storage_policy<profile_unit::bit, fixed_values<0>, 7>;

  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F && f) {
    bool rejected = false;
    try { f(); } catch (std::invalid_argument const &) { rejected = true; }
    require(rejected, "invalid object input accepted");
  }
  object_id id(unsigned n = 1) {
    char value[33];
    std::snprintf(value, sizeof value, "0123456789abcdef01234567%08x", n);
    return object_id(value);
  }
  object_attempt_id attempt(unsigned n = 1) {
    char value[33];
    std::snprintf(value, sizeof value, "fedcba9876543210fedcba98%08x", n);
    return object_attempt_id(value);
  }
  std::uint32_t crc_oracle(std::span<std::byte const> bytes, std::uint32_t previous = 0) {
    auto crc = ~previous;
    for (auto byte : bytes) {
      crc ^= std::to_integer<unsigned>(byte);
      for (unsigned i = 0; i < 8; ++i) crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78u : 0);
    }
    return ~crc;
  }

  // In-memory filesystem effects are deliberately separate from the transcript
  // of acknowledged syscalls. A failed link can still have installed its name.
  struct model_ops {
    static constexpr bool supported = true;
    std::vector<std::string> calls;
    std::optional<std::size_t> fail_at;
    int fail_errno = EIO;
    bool install_before_failure = false;
    bool short_writes = false;
    bool interrupt_write = false, interrupt_header = false;
    bool zero_write = false, zero_header = false;
    bool private_exists = false, final_exists = false, read_only = false;
    std::array<bool, 4> opened{};
    std::vector<std::byte> bytes;
    std::size_t offset = 0;
    unsigned writes = 0, headers = 0, installs = 0, removals = 0;
    object_sync_barrier barrier() const noexcept { return object_sync_barrier::fsync; }
    bool event(char const * name) {
      calls.emplace_back(name);
      if (fail_at && calls.size() - 1 == *fail_at) { errno = fail_errno; return false; }
      return true;
    }
    int open_root(std::filesystem::path const &) {
      if (!event("open root")) return -1;
      opened[0] = true;
      return 10;
    }
    int make_directory(int, char const *) { return event("mkdir") ? 0 : -1; }
    int open_directory(int parent, char const *) {
      if (!event("open directory")) return -1;
      opened[std::size_t(parent - 9)] = true;
      return parent + 1;
    }
    int create_private(int, char const *) {
      if (!event("create private")) return -1;
      if (private_exists) { errno = EEXIST; return -1; }
      private_exists = true;
      opened[3] = true;
      return 13;
    }
    std::ptrdiff_t write(int, std::span<std::byte const> data) {
      ++writes;
      if (!event("write")) return -1;
      if (zero_write) return 0;
      if (std::exchange(interrupt_write, false)) { errno = EINTR; return -1; }
      auto count = short_writes ? std::min(std::size_t{7}, data.size()) : data.size();
      bytes.resize(offset + count);
      std::copy_n(data.begin(), count, bytes.begin() + std::ptrdiff_t(offset));
      offset += count;
      return std::ptrdiff_t(count);
    }
    std::ptrdiff_t write_at(int, std::span<std::byte const> data, std::uint64_t at) {
      ++headers;
      if (!event("pwrite")) return -1;
      if (zero_header) return 0;
      if (std::exchange(interrupt_header, false)) { errno = EINTR; return -1; }
      auto count = short_writes ? std::min(std::size_t{3}, data.size()) : data.size();
      require(at + count <= bytes.size(), "header write exceeded reserved prefix");
      std::copy_n(data.begin(), count, bytes.begin() + std::ptrdiff_t(at));
      return std::ptrdiff_t(count);
    }
    int make_read_only(int) {
      if (!event("chmod")) return -1;
      read_only = true;
      return 0;
    }
    int sync_file(int) { return event("sync file") ? 0 : -1; }
    int sync_directory(int fd) {
      require(fd >= 10 && fd <= 12, "wrong directory sync handle");
      return event(fd == 12 ? "sync leaf" : fd == 11 ? "sync first" : "sync root") ? 0 : -1;
    }
    int install(int, char const *, char const *) {
      ++installs;
      bool acknowledged = event("link");
      if (!acknowledged && !install_before_failure) return -1;
      require(private_exists && read_only, "installed writable or absent private output");
      if (final_exists) { errno = EEXIST; return -1; }
      final_exists = true;
      return acknowledged ? 0 : -1;
    }
    int remove_private(int, char const *) {
      ++removals;
      if (!event("unlink private")) return -1;
      require(final_exists, "removed only output name");
      private_exists = false;
      return 0;
    }
    int close(int fd) {
      require(fd >= 10 && fd <= 13 && opened[std::size_t(fd - 10)], "closed descriptor twice");
      opened[std::size_t(fd - 10)] = false; // close errors can consume the descriptor.
      return event("close") ? 0 : -1;
    }
  };

  std::vector<std::byte> body_bytes(std::size_t size) {
    std::vector<std::byte> result(size);
    for (std::size_t i = 0; i < size; ++i) result[i] = std::byte((i * 109 + i / 7) & 255);
    return result;
  }

  void model_tests() {
    auto body = body_bytes(79);
    file_header<policy> header{file_kind::native_blob, body.size(), 5, std::nullopt};
    auto source = std::span<std::byte const>(body);
    std::array chunks{source.first(1), std::span<std::byte const>{}, source.subspan(1, 36), source.subspan(37)};
    auto invoke = [&](auto & ops) {
      return object_writer<policy, model_ops>::seal("root", id(), attempt(), header, chunks, ops);
    };
    model_ops baseline;
    auto receipt = invoke(baseline);
    require(baseline.final_exists && !baseline.private_exists && baseline.read_only, "successful seal state");
    require(receipt.path == std::filesystem::path("root") / object_path(id(), header.kind), "receipt path");
    require(receipt.object == id() && receipt.attempt == attempt(), "receipt identity");
    require(receipt.bytes == 96 + body.size() && receipt.body_crc32c == crc_oracle(body), "receipt bytes/CRC");
    require(baseline.bytes == encode_file(header, body), "streamed envelope mismatch");
    require(validate_file<policy>(baseline.bytes) == header, "streamed envelope invalid");
    std::vector<std::string> expected{
      "open root", "mkdir", "open directory", "mkdir", "open directory", "create private",
      "write", "write", "write", "write", "pwrite", "chmod", "sync file", "link",
      "sync leaf", "sync first", "sync root", "unlink private", "sync leaf", "sync file",
      "close", "close", "close", "close"};
    require(baseline.calls == expected, "persistence ordering changed");

    model_ops partial;
    partial.short_writes = partial.interrupt_write = partial.interrupt_header = true;
    (void)invoke(partial);
    require(partial.bytes == baseline.bytes, "short/interrupted write handling");
    require(partial.writes > baseline.writes && partial.headers > baseline.headers, "partial paths not exercised");

    for (bool header_zero : {false, true}) {
      model_ops zero;
      zero.zero_write = !header_zero;
      zero.zero_header = header_zero;
      try { (void)invoke(zero); require(false, "zero-progress write accepted"); }
      catch (object_write_error const & error) {
        require(error.code().value() == EIO && zero.private_exists && !zero.final_exists,
                "zero-progress write did not preserve the private output");
      }
    }

    for (std::size_t cut = 0; cut < expected.size(); ++cut) {
      model_ops failed;
      failed.fail_at = cut;
      bool rejected = false;
      try { (void)invoke(failed); }
      catch (object_write_error const & error) {
        rejected = true;
        require(error.code().value() == EIO, "lost primary errno");
        require(error.object == id() && error.attempt == attempt(), "lost failure identities");
        require(error.paths.final == receipt.path, "lost output path");
        require(error.paths.private_output == object_output_paths("root", id(), attempt(), header.kind).private_output,
                "lost private attempt path");
        require(error.stage != object_write_stage::complete, "failure returned complete state");
      }
      require(rejected, "injected syscall failure accepted");
      for (std::size_t i = cut + 1; i < failed.calls.size(); ++i)
        require(failed.calls[i] == "close", "I/O resumed or uncertain output removed after failure");
      require(std::ranges::none_of(failed.opened, [](bool value) { return value; }), "leaked model descriptor");
      if (cut >= 6 && cut <= 17) require(failed.private_exists, "uncertain private output removed");
      if (cut >= 14) require(failed.final_exists, "uncertain installed output lost");
    }

    // EINTR during flush is an uncertain attempt, never an automatic retry.
    for (auto cut : {12u, 14u, 19u, 20u}) {
      model_ops failed;
      failed.fail_at = cut;
      failed.fail_errno = EINTR;
      try { (void)invoke(failed); require(false, "sync/close EINTR retried"); }
      catch (object_write_error const & error) { require(error.code().value() == EINTR, "lost interrupted errno"); }
    }
    model_ops lost_ack;
    lost_ack.fail_at = 13;
    lost_ack.install_before_failure = true;
    try { (void)invoke(lost_ack); require(false, "lost link ack accepted"); }
    catch (object_write_error const & error) {
      require(error.stage == object_write_stage::content_synced && lost_ack.final_exists && lost_ack.private_exists,
              "failed install assumed rollback");
    }
    model_ops no_space;
    no_space.short_writes = true;
    no_space.fail_at = 9; // Three successful short writes, then ENOSPC.
    no_space.fail_errno = ENOSPC;
    try { (void)invoke(no_space); require(false, "partial ENOSPC accepted"); }
    catch (object_write_error const & error) {
      require(error.code().value() == ENOSPC && no_space.bytes.size() == 21 && no_space.private_exists &&
              !no_space.final_exists && no_space.writes == 4, "partial ENOSPC output not retained");
    }
    model_ops existing;
    existing.final_exists = true;
    try { (void)invoke(existing); require(false, "existing destination replaced"); }
    catch (object_write_error const & error) {
      require(error.code().value() == EEXIST && existing.private_exists, "no-clobber failure lost output");
    }

    // Invalid inputs fail before any observable filesystem operation.
    model_ops invalid;
    auto short_header = header;
    --short_header.extent;
    rejects([&] { object_writer<policy, model_ops>::seal("root", id(), attempt(), short_header, chunks, invalid); });
    auto long_header = header;
    ++long_header.extent;
    rejects([&] { object_writer<policy, model_ops>::seal("root", id(), attempt(), long_header, chunks, invalid); });
    file_header<bit_policy> bits{file_kind::fractional_index, 3, 1, 0};
    std::array bad_tail{std::byte{0xff}};
    rejects([&] { object_writer<bit_policy, model_ops>::seal("root", id(), attempt(), bits, bad_tail, invalid); });
    require(invalid.calls.empty(), "invalid body performed I/O");
    auto overflowing = header;
    overflowing.extent = std::numeric_limits<std::uint64_t>::max();
    try {
      object_writer<policy, model_ops>::seal("root", id(), attempt(), overflowing, chunks, invalid);
      require(false, "envelope size overflow accepted");
    } catch (std::overflow_error const &) {}
    overflowing.extent = std::uint64_t(std::numeric_limits<std::int64_t>::max());
    try {
      object_writer<policy, model_ops>::seal("root", id(), attempt(), overflowing, chunks, invalid);
      require(false, "signed file offset overflow accepted");
    } catch (std::length_error const &) {}
    require(invalid.calls.empty(), "overflow performed I/O");
    rejects([] { object_attempt_id bad("../../oops"); });

    // Streaming CRC seeds use conventional complemented states, including
    // empty slices and transitions across accelerated dispatch thresholds.
    auto large = body_bytes(70001);
    for (auto split : {0u, 1u, 127u, 128u, 255u, 256u, 65535u, 65536u, 70001u}) {
      auto bytes = std::span<std::byte const>(large);
      require(crc32c(bytes.subspan(split), crc32c(bytes.first(split))) == crc_oracle(bytes), "CRC seed composition");
    }
  }

#if defined(__APPLE__) || defined(__linux__)
  struct temporary_directory {
    std::filesystem::path path;
    temporary_directory() {
      auto pattern = (std::filesystem::temp_directory_path() / "diet-object-writer-XXXXXX").string();
      auto name = ::mkdtemp(pattern.data());
      if (!name) throw std::system_error(errno, std::generic_category(), "mkdtemp");
      path = name;
    }
    ~temporary_directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
  };
  void write_fixture(std::filesystem::path const & path, std::span<std::byte const> bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<char const *>(bytes.data()), std::streamsize(bytes.size()));
    require(bool(output), "fixture write failed");
  }
  struct failed_sync_ops : posix_object_ops {
    bool directory_failure = false;
    unsigned file_syncs = 0, directory_syncs = 0;
    int sync_file(int fd) noexcept {
      ++file_syncs;
      if (!directory_failure) { errno = EIO; return -1; }
      return posix_object_ops::sync_file(fd);
    }
    int sync_directory(int) noexcept {
      ++directory_syncs;
      errno = EIO;
      return -1;
    }
  };
  void real_tests() {
    temporary_directory directory;
    auto source_bytes = body_bytes((std::size_t{1} << 20) + 79);
    auto source_path = directory.path / "mmap-input";
    write_fixture(source_path, source_bytes);
    auto mapping = mapped_file::open(source_path);
    auto source_pin = mapping.slice(0, mapping.size());
    auto source = source_pin.bytes();
    file_header<policy> header{file_kind::native_blob, source.size(), 11, std::nullopt};
    std::array chunks{source.first(31), source.subspan(31, 65537), std::span<std::byte const>{}, source.subspan(65568)};
    auto receipt = object_writer<policy>::seal(directory.path, id(), attempt(), header, chunks);
    auto stored = file<policy>::open(receipt.path);
    stored.scan();
    auto stored_body = stored.body();
    require(stored.header() == header && std::ranges::equal(stored_body.bytes(), source), "mapped body differs");
    require(receipt.body_crc32c == crc_oracle(source), "mapped source CRC");
    auto paths = object_output_paths(directory.path, id(), attempt(), header.kind);
    require(!std::filesystem::exists(paths.private_output), "successful private name retained");
    struct stat info{};
    require(::stat(receipt.path.c_str(), &info) == 0 && (info.st_mode & 0222) == 0, "sealed object still writable");
#if defined(__APPLE__)
    require(receipt.barrier == object_sync_barrier::apple_full_fsync, "Apple barrier silently weakened");
#else
    require(receipt.barrier == object_sync_barrier::fsync, "Linux barrier mismatch");
#endif

    auto altered = body_bytes(source.size());
    altered[0] ^= std::byte{1};
    try {
      (void)object_writer<policy>::seal(directory.path, id(), attempt(2), header, altered);
      require(false, "real destination replaced");
    } catch (object_write_error const & error) {
      require(error.code().value() == EEXIST, "wrong real collision error");
      require(std::filesystem::exists(error.paths.private_output), "real uncertain output removed");
    }
    require(std::ranges::equal(stored_body.bytes(), source), "collision mutated mapped old object");
    auto previous = object_output_paths(directory.path, id(), attempt(2), header.kind);
    auto previous_size = std::filesystem::file_size(previous.private_output);
    try {
      (void)object_writer<policy>::seal(directory.path, id(), attempt(2), header, source.first(0));
      require(false, "bad repeat body accepted");
    } catch (std::invalid_argument const &) {}
    try {
      (void)object_writer<policy>::seal(directory.path, id(), attempt(2), header, source);
      require(false, "private attempt identity reused");
    } catch (object_write_error const & error) {
      require(error.stage == object_write_stage::starting && error.code().value() == EEXIST,
              "private collision was not exclusive");
    }
    require(std::filesystem::file_size(previous.private_output) == previous_size, "retry truncated uncertain output");

    file_header<bit_policy> empty{file_kind::fractional_index, 0, 0, 0};
    auto empty_receipt = object_writer<bit_policy>::seal(directory.path, id(2), attempt(3), empty, std::span<std::byte const>{});
    file<bit_policy>::open(empty_receipt.path).scan();
    require(empty_receipt.bytes == 96 && empty_receipt.body_crc32c == 0, "empty object framing");

    auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    void * allocation = ::mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    require(allocation != MAP_FAILED, "guard mmap failed");
    auto bytes = static_cast<std::byte *>(allocation);
    require(::mprotect(bytes + page, page, PROT_NONE) == 0, "guard mprotect failed");
    auto tail = std::span(bytes + page - 79, 79);
    std::copy_n(source.begin(), tail.size(), tail.begin());
    tail.back() &= std::byte{0xe0};
    file_header<bit_policy> partial{file_kind::fractional_index, tail.size() * 8 - 5, 9, 0};
    auto partial_receipt = object_writer<bit_policy>::seal(directory.path, id(3), attempt(4), partial, tail);
    auto partial_file = file<bit_policy>::open(partial_receipt.path);
    partial_file.scan();
    auto partial_body = partial_file.body();
    require(std::ranges::equal(partial_body.bytes(), tail), "partial mapped tail mismatch");
    require(::munmap(allocation, page * 2) == 0, "guard munmap failed");

    temporary_directory symlink_root;
    std::filesystem::create_directory_symlink(directory.path, symlink_root.path / "01");
    try {
      (void)object_writer<policy>::seal(symlink_root.path, id(), attempt(5), header, source);
      require(false, "shard symlink followed");
    } catch (object_write_error const &) {}
    temporary_directory final_symlink;
    auto target = object_output_paths(final_symlink.path, id(), attempt(6), header.kind);
    std::filesystem::create_directories(target.final.parent_path());
    std::filesystem::create_symlink(source_path, target.final);
    try {
      (void)object_writer<policy>::seal(final_symlink.path, id(), attempt(6), header, source);
      require(false, "final symlink replaced");
    } catch (object_write_error const & error) { require(error.code().value() == EEXIST, "final symlink error"); }
    require(std::filesystem::is_symlink(target.final), "final symlink changed");
    auto private_target = object_output_paths(final_symlink.path, id(8), attempt(8), header.kind);
    std::filesystem::create_symlink(source_path, private_target.private_output);
    try {
      (void)object_writer<policy>::seal(final_symlink.path, id(8), attempt(8), header, source);
      require(false, "private symlink followed");
    } catch (object_write_error const & error) { require(error.code().value() == EEXIST, "private symlink error"); }

    // Real files survive both pre-install and post-install failures. Reading
    // them here checks retention only; no receipt or recovery event is minted.
    for (bool after_install : {false, true}) {
      failed_sync_ops failing;
      failing.directory_failure = after_install;
      auto failed_id = id(after_install ? 11 : 10);
      auto failed_attempt = attempt(after_install ? 11 : 10);
      try {
        object_writer<policy, failed_sync_ops>::seal(directory.path, failed_id, failed_attempt, header, source, failing);
        require(false, "injected real sync error accepted");
      } catch (object_write_error const & error) {
        require(error.code().value() == EIO && std::filesystem::exists(error.paths.private_output),
                "real failed output was removed");
        require(std::filesystem::exists(error.paths.final) == after_install, "real install boundary lost");
        require(std::filesystem::file_size(error.paths.private_output) == source.size() + 96,
                "real retained extent mismatch");
        require(failing.file_syncs == 1 && failing.directory_syncs == unsigned(after_install),
                "real failed sync was retried");
      }
    }
  }
#endif

  struct unsupported_ops { static constexpr bool supported = false; };
  void unsupported_test() {
    try {
      object_writer<policy, unsupported_ops>::seal("missing", id(), attempt(), {}, std::span<std::byte const>{});
      require(false, "unsupported writer accepted");
    } catch (std::system_error const & error) {
      require(error.code() == std::errc::operation_not_supported, "unsupported error code");
    }
  }
}

int main() {
  try {
    model_tests();
    unsupported_test();
#if defined(__APPLE__) || defined(__linux__)
    real_tests();
#endif
    std::cout << "object writer tests passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
