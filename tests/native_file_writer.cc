/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/native_file_merge.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace {
  thread_local bool fail_allocation = false, count_allocations = false;
  thread_local std::size_t allocation_calls = 0;
  void * allocate(std::size_t size) {
    if (count_allocations) ++allocation_calls;
    if (std::exchange(fail_allocation, false)) throw std::bad_alloc();
    if (auto p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
  }
}
void * operator new(std::size_t size) { return allocate(size); }
void * operator new[](std::size_t size) { return allocate(size); }
void operator delete(void * p) noexcept { std::free(p); }
void operator delete[](void * p) noexcept { std::free(p); }
void operator delete(void * p, std::size_t) noexcept { std::free(p); }
void operator delete[](void * p, std::size_t) noexcept { std::free(p); }

namespace {
  using namespace everett;
  using byte_policy = storage_policy<profile_unit::byte, variable_values, 3, exponential_golomb<0>, 16>;
  using bit_policy = storage_policy<profile_unit::bit, variable_values, 7, golomb<1>, 3>;
  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template<class E = std::exception, class F> void rejects(F && f) {
    bool caught = false;
    try { f(); } catch (E const &) { caught = true; }
    require(caught, "invalid native file operation accepted");
  }
  object_id id(unsigned n = 1) {
    char text[33]; std::snprintf(text, sizeof text, "0123456789abcdef01234567%08x", n); return object_id(text);
  }
  object_attempt_id attempt(unsigned n = 1) {
    char text[33]; std::snprintf(text, sizeof text, "fedcba9876543210fedcba98%08x", n); return object_attempt_id(text);
  }
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
    bool direct_source = false;
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
      if (!source.empty() && data.data() == source.data()) direct_source = true;
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

  template <class P> std::vector<profile_record> records(unsigned count,
      std::optional<std::uint64_t> common, unsigned prefix = 137, unsigned value_units = 13) {
    std::vector<profile_record> result;
    for (unsigned i = 0; i != count; ++i) {
      std::string key(prefix, '0');
      for (unsigned j = 16; j; --j) key += char('0' + ((i >> (j - 1)) & 1));
      if constexpr (P::unit == profile_unit::byte) key.append((8 - key.size() % 8) % 8, '0');
      if (!i) key.clear();
      std::string value(common.value_or((i * 7) % (value_units + 1)) * P::bits_per_unit, '0');
      for (std::size_t j = 0; j != value.size(); ++j) if ((i + j) % 3) value[j] = '1';
      result.push_back({bit_string::from_bits(key), bit_string::from_bits(value)});
    }
    return result;
  }
  template <class P> std::vector<std::byte> expected_wire(std::span<profile_record const> input,
      std::optional<std::uint64_t> common) {
    profile_native_writer<P> writer(common);
    for (auto const & record : input) writer.append(record);
    auto native = writer.finish();
    return encode_native_sections(native).materialize();
  }
  template <class P> void oracle(std::span<std::byte const> actual,
      std::span<profile_record const> input, std::optional<std::uint64_t> common) {
    auto expected = expected_wire<P>(input, common);
    require(std::equal(actual.begin(), actual.end(), expected.begin(), expected.end()), "native file wire mismatch");
    auto header = validate_file<P>(actual);
    auto body = actual.subspan(file_detail::header_bytes);
    auto layout = section_detail::parse(header, body);
    auto view = section_detail::profile<P, stream_role::native>(header, body, layout);
    section_detail::scan_profile(view);
    auto cursor = view.cursor();
    for (auto const & original : input) {
      require(!cursor.done(), "short native file input");
      auto check = [](bit_view a, bit_view b) {
        require(a.size() == b.size(), "native file oracle length");
        for (std::uint64_t i = 0; i != a.size(); ++i) {
          auto bit = [](bit_view v, std::uint64_t p) {
            p += v.offset();
            return (std::to_integer<unsigned>(v.storage()[p / 8]) >> (7 - p % 8)) & 1;
          };
          require(bit(a, i) == bit(b, i), "native file oracle content");
        }
      };
      check(cursor.peek().key.prefix, original.key.view()); check(cursor.peek().value, original.value.view());
      cursor.advance();
    }
    require(cursor.done(), "extra native file records");
  }
  template <class P> void matrix() {
    std::vector<std::optional<std::uint64_t>> widths;
    if constexpr (P::fixed_width) widths = {P::value_width};
    else widths = {std::nullopt, 0, 3};
    for (auto common : widths) for (auto count : {0u, 1u, unsigned(P::codec_block_size),
        unsigned(P::codec_block_size + 1), unsigned(P::codec_block_size * 2 + 1), 513u}) {
      auto input = records<P>(count, common);
      model_ops ops;
      native_file_writer<P, model_ops> writer("model-root", id(), attempt(), common, ops);
      require(writer.size() == 0 && !writer.failed() && !writer.finished(), "native file initial state");
      for (auto const & record : input) {
        // Every call uses misaligned borrowed input which dies immediately.
        auto key = bit_string::from_bits("101"), value = bit_string::from_bits("11010");
        profile_detail::append(key, record.key.view()); profile_detail::append(value, record.value.view());
        writer.append(key.view().subview(3, record.key.bit_size), value.view().subview(5, record.value.bit_size));
      }
      require(writer.size() == input.size(), "native file count");
      auto receipt = writer.finish();
      require(writer.finished() && !writer.failed() && receipt.bytes == ops.bytes.size(), "native file receipt");
      oracle<P>(ops.bytes, input, common);
      auto calls = ops.calls.size();
      rejects<std::logic_error>([&] { writer.append({}, {}); });
      rejects<std::logic_error>([&] { (void)writer.finish(); });
      require(ops.calls.size() == calls, "finished writer performed I/O");
    }
  }
  void growing_key_capacity() {
    using P = storage_policy<profile_unit::bit, fixed_values<0>, 3, exponential_golomb<0>, 1024>;
    std::vector<profile_record> input;
    for (unsigned length = 1; length != 513; ++length)
      input.push_back({bit_string::from_bits(std::string(length, '1')), {}});
    model_ops ops;
    native_file_writer<P, model_ops> writer("model-root", id(), attempt(), P::value_width, ops);
    allocation_calls = 0; count_allocations = true;
    for (auto const & record : input) writer.append(record);
    count_allocations = false;
    require(allocation_calls <= 12, "incrementally growing keys caused linear reallocations");
    writer.finish(); oracle<P>(ops.bytes, input, P::value_width);
  }
  void large_frames() {
    // Golomb(1) forces a unary run spanning multiple control/output buffers.
    std::vector<profile_record> input{
      {bit_string::from_bits(std::string(600001, '0')), bit_string::from_bits("101")},
      {bit_string::from_bits("1"), bit_string::from_bits(std::string(1100001, '1'))},
      {bit_string::from_bits("10"), bit_string::from_bits("1")}};
    model_ops ops;
    native_file_writer<bit_policy, model_ops> writer("model-root", id(), attempt(), std::nullopt, ops);
    for (auto const & record : input) writer.append(record);
    writer.finish(); oracle<bit_policy>(ops.bytes, input, std::nullopt);

    // Aligned large values go straight to the sink, without a staging copy.
    auto large = bit_string::from_bytes(std::string(192 * 1024 + 7, 'v'));
    auto key = bit_string::from_bytes("k");
    std::vector<profile_record> byte_input{{key, large}};
    model_ops direct;
    native_file_writer<byte_policy, model_ops> bytes("model-root", id(), attempt(), std::nullopt, direct);
    direct.source = large.bytes;
    bytes.append(key.view(), large.view());
    require(direct.direct_source, "large aligned value was not forwarded directly");
    bytes.finish(); oracle<byte_policy>(direct.bytes, byte_input, std::nullopt);
  }
  struct concatenate {
    bit_string operator()(bit_view older, bit_view newer) const {
      auto result = bit_string::copy(older); profile_detail::append(result, newer); return result;
    }
  };
  struct keyed_concatenate {
    bit_string operator()(bit_view key, bit_view older, bit_view newer) const {
      require(key.size() == 8, "key-aware file merge lost full key");
      return concatenate{}(older, newer);
    }
  };
  template <class Compose> void composition(Compose compose) {
    using P = byte_policy;
    std::vector<profile_record> old_rows{{bit_string::from_bytes("a"), bit_string::from_bytes("A")},
                                       {bit_string::from_bytes("c"), bit_string::from_bytes("C")}};
    std::vector<profile_record> new_rows{{bit_string::from_bytes("b"), bit_string::from_bytes("B")},
                                       {bit_string::from_bytes("c"), bit_string::from_bytes("D")}};
    auto expected = old_rows; expected.insert(expected.begin() + 1, new_rows.front());
    expected.back().value = bit_string::from_bytes(std::is_same_v<Compose, replace_native_value> ? "D" : "CD");
    auto build = [](auto const & rows) {
      profile_native_writer<P> output;
      for (auto const & row : rows) output.append(row);
      return std::make_shared<profile_array<P> const>(output.finish());
    };
    auto older = build(old_rows), newer = build(new_rows);
    std::weak_ptr<profile_array<P> const> old_pin = older, new_pin = newer;
    model_ops ops;
    {
      native_file_merge<P, profile_array<P>, Compose, model_ops> merge("model-root", id(), attempt(), older, newer, ops, compose);
      older.reset(); newer.reset();
      while (!merge.done()) merge.step();
      merge.finish(); oracle<P>(ops.bytes, expected, std::nullopt);
      require(!old_pin.expired() && !new_pin.expired(), "sealed merge dropped pins while cursor views live");
    }
    require(old_pin.expired() && new_pin.expired(), "file merge leaked pins");
  }
  template<class Compose> void failed_composition(Compose compose) {
    using P = storage_policy<profile_unit::byte, fixed_values<1>, 3, exponential_golomb<0>, 7>;
    auto rows = records<P>(1, 1);
    profile_native_writer<P> output;
    for (auto const & row : rows) output.append(row);
    auto source = std::make_shared<profile_array<P> const>(output.finish());
    std::weak_ptr<profile_array<P> const> pin = source;
    model_ops ops;
    {
      native_file_merge<P, profile_array<P>, Compose, model_ops> merge("model-root", id(), attempt(), source, source, ops, compose);
      source.reset();
      rejects([&] { merge.step(); });
      require(merge.failed() && !merge.done() && !pin.expired(), "composition failure lost poison/pins");
      auto calls = ops.calls.size();
      rejects<std::logic_error>([&] { merge.finish(); });
      rejects<std::logic_error>([&] { merge.step(); });
      require(ops.calls.size() == calls && ops.private_exists && !ops.final_exists, "failed merge promoted output");
    }
    require(pin.expired(), "failed merge leaked pins");
  }
  void validation_and_failures() {
    auto input = records<byte_policy>(4, std::nullopt);
    model_ops baseline;
    { native_file_writer<byte_policy, model_ops> writer("model-root", id(), attempt(), std::nullopt, baseline);
      for (auto const & record : input) writer.append(record);
      writer.finish(); }
    for (std::size_t cut = 0; cut != baseline.calls.size(); ++cut) {
      model_ops ops; ops.fail_at = cut;
      bool caught = false;
      try {
        native_file_writer<byte_policy, model_ops> writer("model-root", id(), attempt(), std::nullopt, ops);
        for (auto const & record : input) writer.append(record);
        try { writer.finish(); }
        catch (object_write_error const &) {
          caught = true; require(writer.failed() && !writer.finished(), "final I/O error did not poison writer");
          auto calls = ops.calls.size();
          rejects<std::logic_error>([&] { writer.append({}, {}); });
          rejects<std::logic_error>([&] { writer.finish(); });
          require(calls == ops.calls.size(), "poisoned writer retried I/O");
        }
      } catch (object_write_error const &) { caught = true; }
      require(caught, "fault cut did not fail");
      require(std::none_of(ops.opened.begin(), ops.opened.end(), [](bool value) { return value; }), "fault leaked handles");
    }
    model_ops ops;
    native_file_writer<byte_policy, model_ops> writer("model-root", id(), attempt(), 3, ops);
    auto source = records<byte_policy>(3, 3);
    writer.append(source[0]);
    auto calls = ops.calls.size();
    rejects<std::invalid_argument>([&] { writer.append(source[0]); });
    rejects<std::invalid_argument>([&] { writer.append(source[1].key.view(), {}); });
    auto short_key = bit_string::from_bits("1");
    rejects<std::invalid_argument>([&] { writer.append(short_key.view(), source[1].value.view()); });
    require(writer.size() == 1 && !writer.failed() && calls == ops.calls.size(), "invalid append changed writer");
    for (std::size_t i = 1; i != source.size(); ++i) writer.append(source[i]);
    calls = ops.calls.size();
    fail_allocation = true;
    rejects<std::bad_alloc>([&] { writer.finish(); });
    require(!fail_allocation && !writer.failed() && !writer.finished() && ops.calls.size() == calls,
            "EF allocation failure was not retryable before I/O");
    writer.finish(); oracle<byte_policy>(ops.bytes, source, 3);

    model_ops short_ops; short_ops.short_writes = true; short_ops.interrupt_write = true; short_ops.interrupt_header = true;
    native_file_writer<byte_policy, model_ops> short_writer("model-root", id(), attempt(), std::nullopt, short_ops);
    for (auto const & record : input) short_writer.append(record);
    short_writer.finish(); oracle<byte_policy>(short_ops.bytes, input, std::nullopt);

    model_ops bad_append;
    native_file_writer<byte_policy, model_ops> bad("model-root", id(), attempt(), std::nullopt, bad_append);
    bad_append.fail_at = bad_append.calls.size();
    auto big = bit_string::from_bytes(std::string(128 * 1024, 'x'));
    auto key = bit_string::from_bytes("k");
    rejects<object_write_error>([&] { bad.append(key.view(), big.view()); });
    require(bad.failed() && bad.size() == 0, "append I/O failure did not poison");
    calls = bad_append.calls.size(); rejects<std::logic_error>([&] { bad.finish(); });
    require(calls == bad_append.calls.size(), "failed append retried on finish");
  }
#if defined(__APPLE__) || defined(__linux__)
  struct temporary_directory {
    std::filesystem::path path;
    temporary_directory() {
      auto pattern = (std::filesystem::temp_directory_path() / "everett-native-file-XXXXXX").string();
      auto result = ::mkdtemp(pattern.data());
      if (!result) throw std::runtime_error("native file temp directory");
      path = result;
    }
    ~temporary_directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
  };
  void malformed_mapped_inputs() {
    using P = byte_policy;
    temporary_directory directory;
    auto rows = records<P>(3, std::nullopt);
    profile_native_writer<P> writer;
    for (auto const & row : rows) writer.append(row);
    auto array = writer.finish();
    for (bool first : {false, true}) {
      auto bytes = encode_native_sections(array).materialize();
      auto header = decode_file_header<P>(bytes);
      auto body = std::span<std::byte>(bytes).subspan(file_detail::header_bytes);
      auto fc = file_detail::get(body, section_detail::native_descriptor_offset, 8);
      auto at = first ? 1 : array.view().encoded_at(1).next_offset;
      body[fc + at] = std::byte{127};
      std::array<std::span<std::byte const>, 1> parts{body};
      auto number = first ? 51u : 50u;
      auto receipt = object_writer<P>::seal(directory.path, id(number), attempt(number), header, parts);
      auto source = std::make_shared<mapped_native<P> const>(mapped_native<P>::open(receipt.path));
      std::weak_ptr<mapped_native<P> const> pin = source;
      model_ops ops;
      if (first) {
        rejects<std::invalid_argument>([&] {
          native_file_merge<P, mapped_native<P>, replace_native_value, model_ops> bad(
            "model-root", id(), attempt(), source, source, ops);
        });
        source.reset();
      } else {
        {
          native_file_merge<P, mapped_native<P>, replace_native_value, model_ops> bad(
            "model-root", id(), attempt(), source, source, ops);
          source.reset(); std::filesystem::remove(receipt.path);
          require(bad.step(1).keys == 1, "valid mapped first record failed");
          rejects<std::invalid_argument>([&] { bad.step(1); });
          require(bad.failed() && !bad.done() && !pin.expired(), "late mapped failure lost poison/pin");
          auto calls = ops.calls.size();
          rejects<std::logic_error>([&] { bad.finish(); });
          require(ops.calls.size() == calls && !ops.final_exists, "malformed mapped input sealed output");
        }
      }
      require(pin.expired(), "malformed mapped input leaked owner");
      require(!ops.final_exists, "invalid input constructor promoted output");
    }
  }
  void guarded_sources() {
    auto page = std::size_t(::sysconf(_SC_PAGESIZE));
    auto length = 17 * page;
    auto address = ::mmap(nullptr, length + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    require(address != MAP_FAILED, "guarded input allocation");
    auto data = static_cast<std::byte *>(address);
    require(::mprotect(data + length, page, PROT_NONE) == 0, "guard input end");
    for (std::size_t i = 0; i != length; ++i) data[i] = std::byte(i * 17);
    auto source = bit_view(std::span<std::byte const>(data, length), length * 8 - 3, 3);
    auto key = bit_string::from_bits("101");
    std::vector<profile_record> expected{{key, bit_string::copy(source)}};
    model_ops ops;
    native_file_writer<bit_policy, model_ops> writer("model-root", id(), attempt(), std::nullopt, ops);
    writer.append(key.view(), source);
    require(::mprotect(address, length, PROT_NONE) == 0, "protect consumed value");
    writer.finish();
    require(::munmap(address, length + page) == 0, "unmap consumed value");
    oracle<bit_policy>(ops.bytes, expected, std::nullopt);
  }
  template <class P> void real_files() {
    temporary_directory directory;
    auto input = records<P>(35, P::value_width, 513);
    native_file_writer<P> writer(directory.path, id(1), attempt(1));
    for (auto const & record : input) writer.append(record);
    auto receipt = writer.finish();
    auto mapping = mapped_file::open(receipt.path);
    auto mapped_bytes = mapping.slice(0, mapping.size());
    oracle<P>(mapped_bytes.bytes(), input, P::value_width);
    auto native = mapped_native<P>::open(receipt.path); native.scan();
    {
      native_file_writer<P> duplicate(directory.path, id(1), attempt(99));
      rejects<object_write_error>([&] { duplicate.finish(); });
      require(duplicate.failed() && std::filesystem::exists(duplicate.paths().private_output), "no-clobber lost uncertain attempt");
      auto unchanged = mapped_file::open(receipt.path);
      auto unchanged_bytes = unchanged.slice(0, unchanged.size());
      oracle<P>(unchanged_bytes.bytes(), input, P::value_width);
    }
    std::filesystem::remove(receipt.path);
    require(native.view().size() == input.size(), "unlinked native mapping lost input");

    auto old_input = input, new_input = input;
    old_input.erase(old_input.begin() + 1);
    new_input.erase(new_input.begin() + 2);
    auto map = [&](auto const & rows, unsigned n) {
      native_file_writer<P> output(directory.path, id(n), attempt(n));
      for (auto const & row : rows) output.append(row);
      auto sealed = output.finish();
      return std::make_shared<mapped_native<P> const>(mapped_native<P>::open(sealed.path));
    };
    auto older = map(old_input, 2), newer = map(new_input, 3);
    std::weak_ptr<mapped_native<P> const> old_pin = older, new_pin = newer;
    {
      native_file_merge<P, mapped_native<P>> merge(directory.path, id(4), attempt(4), older, newer);
      older.reset(); newer.reset();
      std::filesystem::remove(directory.path / object_path(id(2), file_kind::native_blob));
      std::filesystem::remove(directory.path / object_path(id(3), file_kind::native_blob));
      require(!merge.done() && !merge.failed() && !merge.finished(), "native file merge initial state");
      require(merge.step(0).keys == 0 && merge.progress().keys == 0, "zero merge budget progressed");
      rejects<std::logic_error>([&] { merge.finish(); });
      while (!merge.done()) {
        auto progress = merge.step(3); require(progress.keys <= 3, "merge exceeded key budget");
        require(!old_pin.expired() && !new_pin.expired(), "paused merge dropped source pins");
      }
      auto merged = merge.finish();
      auto mapped = mapped_file::open(merged.path);
      auto merged_bytes = mapped.slice(0, mapped.size());
      oracle<P>(merged_bytes.bytes(), input, P::value_width);
      require(merge.finished() && !merge.failed(), "native file merge final state");
      require(merge.progress().keys == input.size() && merge.progress().input_records == old_input.size() + new_input.size(),
              "native file merge counters");
    }
    require(old_pin.expired() && new_pin.expired(), "destroyed merge retained source mappings");
  }
#endif
}

int main() {
  try {
    static_assert(!std::is_move_constructible_v<native_file_writer<byte_policy, model_ops>>);
    static_assert(!std::is_copy_constructible_v<native_file_merge<byte_policy, profile_array<byte_policy>, replace_native_value, model_ops>>);
    matrix<byte_policy>();
    matrix<storage_policy<profile_unit::byte, fixed_values<3>, 31, exponential_golomb<0>, 7>>();
    matrix<bit_policy>();
    matrix<storage_policy<profile_unit::bit, fixed_values<0>, 3, exponential_golomb<3>, 16>>();
    matrix<storage_policy<profile_unit::bit, fixed_values<3>, 31, golomb<3>, 7>>();
    matrix<storage_policy<profile_unit::bit, variable_values, 3, exponential_golomb<63>, 7>>();
    growing_key_capacity(); large_frames(); validation_and_failures();
    composition(replace_native_value{}); composition(concatenate{}); composition(keyed_concatenate{});
    failed_composition([](bit_view, bit_view) -> bit_view { throw std::runtime_error("composition fixture"); });
    failed_composition([](bit_view, bit_view) { return bit_string{}; });
#if defined(__APPLE__) || defined(__linux__)
    malformed_mapped_inputs(); guarded_sources();
    real_files<byte_policy>();
    real_files<storage_policy<profile_unit::bit, fixed_values<3>, 3, golomb<3>, 7>>();
#endif
    std::cout << "native file writer/merge: wire, bounded frames, mmap and faults passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
