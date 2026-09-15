/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/object_stream.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <type_traits>
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
    bool install_before_failure = false, header_before_failure = false;
    bool short_writes = false;
    bool interrupt_write = false, interrupt_header = false;
    bool zero_write = false, zero_header = false;
    bool private_exists = false, final_exists = false, read_only = false;
    std::array<bool, 4> opened{};
    std::vector<std::byte> bytes;
    std::size_t offset = 0;
    std::span<std::byte const> source;
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
      if (!source.empty() && offset >= 96)
        require(data.data() == source.data() + offset - 96, "stream copied or reordered a submitted body span");
      auto count = short_writes ? std::min(std::size_t{7}, data.size()) : data.size();
      bytes.resize(offset + count);
      std::copy_n(data.begin(), count, bytes.begin() + std::ptrdiff_t(offset));
      offset += count;
      return std::ptrdiff_t(count);
    }
    std::ptrdiff_t write_at(int, std::span<std::byte const> data, std::uint64_t at) {
      ++headers;
      bool acknowledged = event("pwrite");
      if (!acknowledged && !header_before_failure) return -1;
      if (zero_header) return 0;
      if (std::exchange(interrupt_header, false)) { errno = EINTR; return -1; }
      auto count = short_writes ? std::min(std::size_t{3}, data.size()) : data.size();
      require(at + count <= bytes.size(), "header write exceeded reserved prefix");
      std::copy_n(data.begin(), count, bytes.begin() + std::ptrdiff_t(at));
      return acknowledged ? std::ptrdiff_t(count) : -1;
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

  using model_stream = object_stream<policy, model_ops>;
  static_assert(!std::is_copy_constructible_v<model_stream>);
  static_assert(!std::is_copy_assignable_v<model_stream>);
  static_assert(!std::is_move_constructible_v<model_stream>);
  static_assert(!std::is_move_assignable_v<model_stream>);
  template<class T> concept temporary_paths = requires(T & value) { std::move(value).paths(); };
  static_assert(!temporary_paths<model_stream>);

  template<class F> void inactive(F && action) {
    bool rejected = false;
    try { action(); } catch (std::logic_error const &) { rejected = true; }
    require(rejected, "inactive stream accepted work");
  }
  void model_tests() {
    auto body = body_bytes(79);
    auto source = std::span<std::byte const>(body);
    file_header<policy> header{file_kind::native_blob, body.size(), 5, std::nullopt};
    auto append = [&](auto & stream) {
      stream.append(source.first(1)); stream.append({});
      stream.append(source.subspan(1, 36)); stream.append(source.subspan(37));
    };
    model_ops baseline;
    baseline.source = source;
    std::optional<object_seal_receipt> receipt;
    {
      // Temporary identity/path arguments must not become borrowed references.
      model_stream stream(std::filesystem::path("root"), id(), attempt(), header.kind, baseline);
      require(!stream.failed() && !stream.finished(), "new stream state");
      require(stream.body_bytes() == 0 && stream.body_crc32c() == 0, "new stream accounting");
      require(baseline.private_exists && !baseline.final_exists && baseline.bytes == std::vector<std::byte>(96), "missing private placeholder");
      append(stream);
      require(stream.body_bytes() == body.size() && stream.body_crc32c() == crc_oracle(body), "incremental accounting");
      require(stream.paths().final == object_output_paths("root", id(), attempt(), header.kind).final, "stream path identity");
      receipt = stream.finish(header);
      require(stream.finished() && !stream.failed(), "finished stream state");
      auto calls = baseline.calls.size();
      inactive([&] { stream.append({}); }); inactive([&] { (void)stream.finish(header); });
      require(baseline.calls.size() == calls, "finished stream performed more I/O");
    }
    require(baseline.final_exists && !baseline.private_exists && baseline.read_only, "successful stream state");
    require(receipt->object == id() && receipt->attempt == attempt() && receipt->path == std::filesystem::path("root") / object_path(id(), header.kind), "receipt identities");
    require(receipt->bytes == 96 + body.size() && receipt->body_crc32c == crc_oracle(body), "receipt bytes/CRC");
    require(baseline.bytes == encode_file(header, body), "streamed envelope differs from canonical file");
    require(validate_file<policy>(baseline.bytes) == header, "streamed envelope invalid");
    std::vector<std::string> expected{
      "open root", "mkdir", "open directory", "mkdir", "open directory", "create private",
      "write", "write", "write", "write", "pwrite", "chmod", "sync file", "link",
      "sync leaf", "sync first", "sync root", "unlink private", "sync leaf", "sync file",
      "close", "close", "close", "close"};
    require(baseline.calls == expected, "stream persistence order differs from one-shot writer");

    model_ops partial;
    partial.source = source;
    partial.short_writes = partial.interrupt_write = partial.interrupt_header = true;
    {
      model_stream stream("root", id(), attempt(), header.kind, partial);
      append(stream); (void)stream.finish(header);
    }
    require(partial.bytes == baseline.bytes && partial.writes > baseline.writes && partial.headers > baseline.headers,
      "short or interrupted append/header handling");

    for (std::size_t cut = 0; cut != expected.size(); ++cut) {
      model_ops failed;
      failed.source = source; failed.fail_at = cut;
      std::unique_ptr<model_stream> stream;
      bool rejected = false;
      try {
        stream = std::make_unique<model_stream>("root", id(), attempt(), header.kind, failed);
        append(*stream); (void)stream->finish(header);
      } catch (object_write_error const & error) {
        rejected = true;
        require(error.code().value() == EIO && error.stage != object_write_stage::complete, "incorrect failure evidence");
        require(error.object == id() && error.attempt == attempt() && error.paths.final == receipt->path, "lost failed attempt identity");
        if (stream) {
          require(stream->failed() && !stream->finished(), "I/O failure did not poison live stream");
          auto calls = failed.calls.size();
          inactive([&] { stream->append(source); }); inactive([&] { (void)stream->finish(header); });
          require(failed.calls.size() == calls, "poisoned stream performed more I/O");
        }
      }
      require(rejected, "injected syscall failure accepted");
      stream.reset();
      for (std::size_t i = cut + 1; i < failed.calls.size(); ++i)
        require(failed.calls[i] == "close", "failed stream continued I/O or removed an uncertain name");
      require(std::ranges::none_of(failed.opened, [](bool value) { return value; }), "leaked model descriptor");
      if (cut >= 6 && cut <= 17) require(failed.private_exists, "failed stream removed private output");
      if (cut >= 14) require(failed.final_exists, "failed stream lost installed output");
    }
    for (auto cut : {12u, 14u, 19u, 20u}) {
      model_ops failed; failed.fail_at = cut; failed.fail_errno = EINTR;
      try {
        model_stream stream("root", id(), attempt(), header.kind, failed);
        append(stream); (void)stream.finish(header); require(false, "barrier/close EINTR was retried");
      } catch (object_write_error const & error) { require(error.code().value() == EINTR, "lost EINTR"); }
      require(failed.calls.size() <= expected.size(), "barrier retry changed transcript");
    }
    for (bool zero_header : {false, true}) {
      model_ops zero;
      model_stream stream("root", id(), attempt(), header.kind, zero);
      zero.zero_write = !zero_header; zero.zero_header = zero_header;
      try { append(stream); (void)stream.finish(header); require(false, "zero progress accepted"); }
      catch (object_write_error const & error) {
        require(error.code().value() == EIO && stream.failed() && zero.private_exists && !zero.final_exists, "zero progress did not poison/preserve");
      }
    }
    model_ops lost;
    lost.fail_at = 13; lost.install_before_failure = true;
    try {
      model_stream stream("root", id(), attempt(), header.kind, lost);
      append(stream); (void)stream.finish(header); require(false, "lost link acknowledgement accepted");
    } catch (object_write_error const & error) {
      require(error.stage == object_write_stage::content_synced && lost.final_exists && lost.private_exists, "failed install assumed rollback");
    }
    model_ops abandoned;
    {
      model_stream stream("root", id(), attempt(), header.kind, abandoned);
      append(stream);
    }
    require(abandoned.private_exists && !abandoned.final_exists && abandoned.bytes.size() == body.size() + 96, "unfinished stream discarded its output");
    require(abandoned.installs == 0 && abandoned.removals == 0 && std::ranges::none_of(abandoned.opened, [](bool open) { return open; }), "unfinished destructor sealed, unlinked or leaked");

    model_ops no_space;
    model_stream stream("root", id(), attempt(), header.kind, no_space);
    no_space.short_writes = true; no_space.fail_at = no_space.calls.size() + 3; no_space.fail_errno = ENOSPC;
    try { stream.append(source); require(false, "partial ENOSPC accepted"); }
    catch (object_write_error const & error) {
      require(error.code().value() == ENOSPC && stream.failed() && no_space.bytes.size() == 96 + 21 && no_space.private_exists && !no_space.final_exists,
        "partial append failure lost bytes or poison state");
    }
  }

  void prefix_model_tests() {
    auto prefix = body_bytes(128), suffix = body_bytes(79);
    auto body = prefix; body.insert(body.end(), suffix.begin(), suffix.end());
    auto provisional = std::vector<std::byte>(prefix.size());
    provisional.insert(provisional.end(), suffix.begin(), suffix.end());
    file_header<policy> header{file_kind::native_blob, body.size(), 5, std::nullopt};
    model_ops baseline;
    {
      model_stream stream("root", id(), attempt(), header.kind, prefix.size(), baseline);
      require(stream.body_bytes() == prefix.size() && stream.body_crc32c() == crc_oracle(std::vector<std::byte>(prefix.size())), "reserved prefix accounting");
      require(baseline.bytes == std::vector<std::byte>(96 + prefix.size()), "prefix reservation not zero-filled");
      stream.append(suffix);
      require(stream.body_bytes() == body.size() && stream.body_crc32c() == crc_oracle(provisional), "provisional prefix CRC");
      auto calls = baseline.calls.size();
      rejects([&] { (void)stream.finish(header); });
      rejects([&] { (void)stream.finish(header, std::span(prefix).first(prefix.size() - 1)); });
      auto too_long = prefix; too_long.push_back(std::byte{});
      rejects([&] { (void)stream.finish(header, too_long); });
      auto bad = header; ++bad.extent;
      rejects([&] { (void)stream.finish(bad, prefix); });
      require(baseline.calls.size() == calls && !stream.failed(), "invalid prefix finish performed I/O");
      auto receipt = stream.finish(header, prefix);
      require(receipt.body_crc32c == crc_oracle(body) && stream.body_crc32c() == receipt.body_crc32c && stream.finished(), "final prefix CRC not installed");
      require(baseline.bytes == encode_file(header, body), "prefix backpatch changed suffix or envelope");
    }
    auto expected = baseline.calls;
    for (std::size_t cut = 0; cut != expected.size(); ++cut) {
      model_ops failed; failed.fail_at = cut;
      std::unique_ptr<model_stream> stream;
      bool rejected = false;
      try {
        stream = std::make_unique<model_stream>("root", id(), attempt(), header.kind, prefix.size(), failed);
        stream->append(suffix); (void)stream->finish(header, prefix);
      } catch (object_write_error const & error) {
        rejected = true;
        require(error.code().value() == EIO && error.object == id() && error.attempt == attempt(), "prefix failure lost evidence");
        if (stream) {
          require(stream->failed() && !stream->finished(), "prefix failure did not poison");
          auto calls = failed.calls.size();
          inactive([&] { stream->append({}); }); inactive([&] { (void)stream->finish(header, prefix); });
          require(failed.calls.size() == calls, "poisoned prefix stream resumed I/O");
        }
      }
      require(rejected, "prefix syscall failure accepted"); stream.reset();
      for (std::size_t i = cut + 1; i < failed.calls.size(); ++i)
        require(failed.calls[i] == "close", "prefix failure resumed non-close I/O");
      require(std::ranges::none_of(failed.opened, [](bool open) { return open; }), "prefix failure leaked descriptor");
      if (cut > 5 && cut < std::size_t(std::find(expected.begin(), expected.end(), "unlink private") - expected.begin()))
        require(failed.private_exists, "prefix failure removed private name");
    }
    for (bool acknowledge_effect : {false, true}) {
      model_ops failed;
      model_stream stream("root", id(), attempt(), header.kind, prefix.size(), failed);
      stream.append(suffix);
      failed.short_writes = true;
      failed.fail_at = failed.calls.size() + 1; // One prefix pwrite succeeds, next fails.
      failed.header_before_failure = acknowledge_effect;
      try { (void)stream.finish(header, prefix); require(false, "partial prefix failure accepted"); }
      catch (object_write_error const &) {
        require(stream.failed() && failed.private_exists && !failed.final_exists, "partial prefix failure lost output");
        auto written = acknowledge_effect ? 6u : 3u;
        require(std::equal(prefix.begin(), prefix.begin() + written, failed.bytes.begin() + 96), "partial prefix writes were hidden");
        require(std::ranges::all_of(std::span(failed.bytes).first(96), [](std::byte value) { return value == std::byte{}; }), "envelope written after prefix failure");
      }
    }
    model_ops partial; partial.short_writes = partial.interrupt_write = partial.interrupt_header = true;
    {
      model_stream stream("root", id(), attempt(), header.kind, prefix.size(), partial);
      stream.append(suffix); (void)stream.finish(header, prefix);
    }
    require(partial.bytes == baseline.bytes, "partial prefix/header retry changed final bytes");
    if constexpr (std::numeric_limits<std::size_t>::max() > std::uint64_t(std::numeric_limits<std::int64_t>::max()) - 96) {
      model_ops overflow;
      bool rejected = false;
      try { model_stream stream("root", id(), attempt(), header.kind, std::numeric_limits<std::size_t>::max(), overflow); }
      catch (std::length_error const &) { rejected = true; }
      require(rejected && overflow.calls.empty(), "unrepresentable prefix length reached I/O");
    }
  }
  void prefix_length_matrix() {
    for (std::size_t length : {std::size_t{0}, std::size_t{1}, std::size_t{127}, std::size_t{128},
        std::size_t{129}, std::size_t{256}, std::size_t{4097}, std::size_t{65537}}) {
      auto prefix = body_bytes(length);
      for (bool empty_suffix : {false, true}) {
        auto suffix = body_bytes(empty_suffix ? 0 : 19);
        auto body = prefix; body.insert(body.end(), suffix.begin(), suffix.end());
        auto provisional = std::vector<std::byte>(prefix.size());
        provisional.insert(provisional.end(), suffix.begin(), suffix.end());
        model_ops ops;
        model_stream stream("root", id(), attempt(), file_kind::native_blob, length, ops);
        stream.append(suffix);
        require(stream.body_crc32c() == crc_oracle(provisional), "zero-prefix length CRC");
        file_header<policy> header{file_kind::native_blob, body.size(), 0, std::nullopt};
        auto receipt = stream.finish(header, prefix);
        require(receipt.body_crc32c == crc_oracle(body) && ops.bytes == encode_file(header, body), "prefix length concatenation mismatch");
      }
    }
  }
  void prefix_bit_tests() {
    for (bool suffix_present : {false, true}) {
      model_ops ops;
      object_stream<bit_policy, model_ops> stream("root", id(), attempt(), file_kind::fractional_index, 1, ops);
      std::array prefix{std::byte{0xff}}, suffix{std::byte{0xe0}};
      if (suffix_present) stream.append(suffix);
      file_header<bit_policy> header{file_kind::fractional_index, suffix_present ? 11u : 3u, 0, 0};
      if (!suffix_present) {
        auto calls = ops.calls.size();
        rejects([&] { (void)stream.finish(header, prefix); });
        require(!stream.failed() && ops.calls.size() == calls, "prefix-only padding error changed state");
        prefix[0] = std::byte{0xe0};
      }
      auto receipt = stream.finish(header, prefix);
      std::vector<std::byte> body(prefix.begin(), prefix.end());
      if (suffix_present) body.insert(body.end(), suffix.begin(), suffix.end());
      require(receipt.body_crc32c == crc_oracle(body) && ops.bytes == encode_file(header, body), "prefix/suffix tail selection or CRC");
    }
    model_ops bad_suffix;
    object_stream<bit_policy, model_ops> stream("root", id(), attempt(), file_kind::fractional_index, 1, bad_suffix);
    std::array prefix{std::byte{0xe0}}, suffix{std::byte{0xff}};
    stream.append(suffix);
    auto calls = bad_suffix.calls.size();
    rejects([&] { (void)stream.finish({file_kind::fractional_index, 11, 0, 0}, prefix); });
    require(!stream.failed() && bad_suffix.calls.size() == calls, "padding checked prefix instead of final suffix");
    auto receipt = stream.finish({file_kind::fractional_index, 16, 0, 0}, prefix);
    std::array body{prefix[0], suffix[0]};
    require(receipt.body_crc32c == crc_oracle(body), "corrected bit extent CRC");
  }
  void identity_tests() {
    model_ops invalid;
    auto old_attempt = attempt();
    auto consumed_attempt = std::move(old_attempt);
    require(consumed_attempt == attempt(), "attempt fixture move");
    bool rejected = false;
    try { model_stream stream("root", id(), old_attempt, file_kind::native_blob, invalid); }
    catch (std::exception const &) { rejected = true; }
    require(rejected && invalid.calls.empty(), "moved-from attempt reached the filesystem");
    auto old_id = id(); auto consumed_id = std::move(old_id);
    require(consumed_id == id(), "identity fixture move");
    rejected = false;
    try { model_stream stream("root", old_id, attempt(), file_kind::native_blob, invalid); }
    catch (std::exception const &) { rejected = true; }
    require(rejected && invalid.calls.empty(), "moved-from object identity reached the filesystem");
    rejects([&] { model_stream stream("root", id(), attempt(), static_cast<file_kind>(255), invalid); });
    require(invalid.calls.empty(), "invalid kind reached the filesystem");
  }
  void metadata_tests() {
    auto body = body_bytes(79);
    file_header<policy> header{file_kind::native_blob, body.size(), 5, std::nullopt};
    model_ops ops;
    model_stream stream("root", id(), attempt(), header.kind, ops);
    stream.append(body);
    auto calls = ops.calls.size();
    auto bad = header; --bad.extent;
    rejects([&] { (void)stream.finish(bad); });
    bad = header; ++bad.extent;
    rejects([&] { (void)stream.finish(bad); });
    bad = header; bad.kind = file_kind::fractional_index; bad.common_value_width = 0;
    rejects([&] { (void)stream.finish(bad); });
    bad = header; bad.extent = std::numeric_limits<std::uint64_t>::max();
    try { (void)stream.finish(bad); require(false, "overflowing final extent accepted"); }
    catch (std::overflow_error const &) {}
    require(ops.calls.size() == calls && !stream.failed() && !stream.finished(), "metadata rejection performed I/O or poisoned");
    auto receipt = stream.finish(header);
    require(receipt.body_crc32c == crc_oracle(body), "metadata retry changed CRC");

    // A rejected bit extent cannot cause finish to reread the caller's buffer.
    model_ops bit_ops;
    object_stream<bit_policy, model_ops> bits("root", id(), attempt(), file_kind::fractional_index, bit_ops);
    std::array source{std::byte{0xff}};
    bits.append(source); source[0] = std::byte{0};
    file_header<bit_policy> bit_header{file_kind::fractional_index, 3, 1, 0};
    calls = bit_ops.calls.size();
    rejects([&] { (void)bits.finish(bit_header); });
    require(bit_ops.calls.size() == calls && !bits.failed(), "tail padding rejection changed stream");
    bit_header.extent = 8;
    auto bit_receipt = bits.finish(bit_header);
    std::array original{std::byte{0xff}};
    require(bit_receipt.body_crc32c == crc_oracle(original), "stream retained a borrowed append buffer");

    model_ops empty_ops;
    object_stream<bit_policy, model_ops> empty("root", id(), attempt(), file_kind::fractional_index, empty_ops);
    auto before = empty_ops.calls.size(); empty.append({});
    require(empty_ops.calls.size() == before && empty.body_bytes() == 0 && empty.body_crc32c() == 0, "empty append changed state");
    auto empty_receipt = empty.finish({file_kind::fractional_index, 0, 0, 0});
    require(empty_receipt.bytes == 96 && empty_receipt.body_crc32c == 0, "empty stream envelope");
  }

#if defined(__APPLE__) || defined(__linux__)
  struct temporary_directory {
    std::filesystem::path path;
    temporary_directory() {
      auto pattern = (std::filesystem::temp_directory_path() / "diet-object-stream-XXXXXX").string();
      auto name = ::mkdtemp(pattern.data());
      if (!name) throw std::system_error(errno, std::generic_category(), "mkdtemp");
      path = name;
    }
    ~temporary_directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
  };
  struct failed_sync_ops : posix_object_ops {
    bool after_install;
    unsigned file_syncs = 0, directory_syncs = 0;
    int sync_file(int fd) noexcept {
      ++file_syncs;
      if (!after_install) { errno = EIO; return -1; }
      return posix_object_ops::sync_file(fd);
    }
    int sync_directory(int) noexcept { ++directory_syncs; errno = EIO; return -1; }
  };
  void real_tests() {
    temporary_directory directory;
    auto body = body_bytes((std::size_t{1} << 20) + 79);
    file_header<policy> header{file_kind::native_blob, body.size(), 11, std::nullopt};
    object_stream<policy> stream(directory.path, id(), attempt(), header.kind);
    auto paths = stream.paths();
    require(std::filesystem::exists(paths.private_output) && !std::filesystem::exists(paths.final), "stream installed before finish");
    std::size_t done = 0;
    // Deliberately varying sizes cross CRC and write-unit boundaries.
    for (auto size : {1u, 127u, 65536u, 7u}) {
      stream.append(std::span(body).subspan(done, size)); done += size;
    }
    stream.append(std::span(body).subspan(done));
    require(!std::filesystem::exists(paths.final), "append installed final object");
    auto receipt = stream.finish(header);
    auto stored = file<policy>::open(receipt.path); stored.scan();
    auto stored_body = stored.body();
    require(stored.header() == header && std::ranges::equal(stored_body.bytes(), body), "streamed real file differs");
    require(receipt.body_crc32c == crc_oracle(body), "real stream CRC");
    require(!std::filesystem::exists(paths.private_output), "successful stream retained private name");
    struct stat info{};
    require(::stat(receipt.path.c_str(), &info) == 0 && (info.st_mode & 0222) == 0, "final object writable");
#if defined(__APPLE__)
    require(receipt.barrier == object_sync_barrier::apple_full_fsync, "Apple barrier weakened");
#else
    require(receipt.barrier == object_sync_barrier::fsync, "Linux barrier mismatch");
#endif
    {
      object_stream<policy> collision(directory.path, id(), attempt(2), header.kind);
      collision.append(body);
      try { (void)collision.finish(header); require(false, "stream replaced existing final object"); }
      catch (object_write_error const & error) {
        require(error.code().value() == EEXIST && collision.failed() && std::filesystem::exists(error.paths.private_output), "collision lost private name or poison state");
      }
    }
    require(std::ranges::equal(stored_body.bytes(), body), "collision changed existing mapped object");
    auto previous = object_output_paths(directory.path, id(), attempt(2), header.kind);
    auto prior_size = std::filesystem::file_size(previous.private_output);
    try { object_stream<policy> repeated(directory.path, id(), attempt(2), header.kind); require(false, "private attempt identity reused"); }
    catch (object_write_error const & error) { require(error.code().value() == EEXIST, "wrong private collision error"); }
    require(std::filesystem::file_size(previous.private_output) == prior_size, "repeat truncated prior output");

    auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    void * allocation = ::mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    require(allocation != MAP_FAILED, "guard mmap");
    auto bytes = static_cast<std::byte *>(allocation);
    require(::mprotect(bytes + page, page, PROT_NONE) == 0, "guard mprotect");
    auto tail = std::span(bytes + page - 79, 79);
    std::copy_n(body.begin(), tail.size(), tail.begin()); tail.back() &= std::byte{0xe0};
    std::vector<std::byte> expected(tail.begin(), tail.end());
    object_stream<bit_policy> bits(directory.path, id(3), attempt(3), file_kind::fractional_index);
    bits.append(tail.first(31)); bits.append(tail.subspan(31));
    require(::munmap(allocation, page * 2) == 0, "guard munmap");
    // All append buffers are now inaccessible; finish only needs cached tail,
    // extent and CRC, with no retained mapping or body scan.
    auto bit_receipt = bits.finish({file_kind::fractional_index, expected.size() * 8 - 5, 9, 0});
    auto bit_file = file<bit_policy>::open(bit_receipt.path); bit_file.scan();
    auto bit_body = bit_file.body();
    require(std::ranges::equal(bit_body.bytes(), expected) && bit_receipt.body_crc32c == crc_oracle(expected), "guarded bit tail or lifetime failure");

    allocation = ::mmap(nullptr, page * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    require(allocation != MAP_FAILED, "prefix guard mmap"); bytes = static_cast<std::byte *>(allocation);
    require(::mprotect(bytes + page, page, PROT_NONE) == 0 && ::mprotect(bytes + page * 3, page, PROT_NONE) == 0, "prefix guard mprotect");
    auto guarded_prefix = std::span(bytes + page - 128, 128);
    auto guarded_suffix = std::span(bytes + page * 3 - 79, 79);
    std::copy_n(body.begin(), guarded_prefix.size(), guarded_prefix.begin());
    std::copy_n(body.begin() + 128, guarded_suffix.size(), guarded_suffix.begin());
    auto prefixed_body = std::vector<std::byte>(guarded_prefix.begin(), guarded_prefix.end());
    prefixed_body.insert(prefixed_body.end(), guarded_suffix.begin(), guarded_suffix.end());
    object_stream<policy> prefixed(directory.path, id(7), attempt(7), file_kind::native_blob, guarded_prefix.size());
    prefixed.append(guarded_suffix);
    require(::mprotect(bytes + page * 2, page, PROT_NONE) == 0, "hide appended suffix");
    auto prefixed_receipt = prefixed.finish({file_kind::native_blob, prefixed_body.size(), 0, std::nullopt}, guarded_prefix);
    require(::munmap(allocation, page * 4) == 0, "prefix guard munmap");
    auto prefixed_file = file<policy>::open(prefixed_receipt.path); prefixed_file.scan();
    auto prefixed_payload = prefixed_file.body();
    require(std::ranges::equal(prefixed_payload.bytes(), prefixed_body) && prefixed_receipt.body_crc32c == crc_oracle(prefixed_body), "guarded prefix backpatch reread suffix or changed bytes");

    for (bool installed : {false, true}) {
      failed_sync_ops failed; failed.after_install = installed;
      auto object = id(installed ? 5 : 4); auto job = attempt(installed ? 5 : 4);
      object_stream<policy, failed_sync_ops> sink(directory.path, object, job, header.kind, failed);
      sink.append(body);
      try { (void)sink.finish(header); require(false, "real injected sync failure accepted"); }
      catch (object_write_error const & error) {
        require(sink.failed() && !sink.finished() && error.code().value() == EIO, "real sync failure state");
        require(std::filesystem::exists(error.paths.private_output) && std::filesystem::exists(error.paths.final) == installed, "real failure lost surviving names");
        require(failed.file_syncs == 1 && failed.directory_syncs == unsigned(installed), "real sync failure retried");
        auto calls = failed.file_syncs + failed.directory_syncs;
        inactive([&] { (void)sink.finish(header); }); inactive([&] { sink.append({}); });
        require(failed.file_syncs + failed.directory_syncs == calls, "poisoned stream resumed barriers");
      }
    }
    auto abandoned = object_output_paths(directory.path, id(6), attempt(6), header.kind);
    {
      object_stream<policy> sink(directory.path, id(6), attempt(6), header.kind);
      sink.append(std::span(body).first(17));
    }
    require(std::filesystem::exists(abandoned.private_output) && !std::filesystem::exists(abandoned.final) &&
      std::filesystem::file_size(abandoned.private_output) == 113, "unfinished real stream discarded or published its prefix");
  }
#endif
}

int main() {
  try {
    model_tests(); identity_tests(); metadata_tests(); prefix_model_tests(); prefix_length_matrix(); prefix_bit_tests();
#if defined(__APPLE__) || defined(__linux__)
    real_tests();
#endif
    std::cout << "Object stream: incremental CRC, poison, publication and borrowed-buffer lifetime checks passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Exercises incremental object sealing, every persistence boundary and append buffer lifetimes.
 */
