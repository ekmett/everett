/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks streamed sort records, exact wire bytes, bounded allocation and poisoned attempts.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/sort_profile_file_merge.h>
#include <diet/sort_profile_file.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <new>

namespace {
  thread_local std::size_t allocation_limit = std::numeric_limits<std::size_t>::max(), largest_allocation = 0;
  void * allocate(std::size_t size) {
    largest_allocation = std::max(largest_allocation, size);
    if (size > allocation_limit) throw std::bad_alloc();
    if (auto p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
  }
}
void * operator new(std::size_t n) { return allocate(n); }
void * operator new[](std::size_t n) { return allocate(n); }
void operator delete(void * p) noexcept { std::free(p); }
void operator delete[](void * p) noexcept { std::free(p); }
void operator delete(void * p, std::size_t) noexcept { std::free(p); }
void operator delete[](void * p, std::size_t) noexcept { std::free(p); }

namespace {
  using namespace diet;
  void check(bool value, char const * why) { if (!value) throw std::runtime_error(why); }
  template <class F> void rejects(F && fn) {
    try { fn(); } catch (std::exception const &) { return; }
    throw std::runtime_error("expected sort file rejection");
  }
  object_id id(unsigned n = 1) { char out[33]; std::snprintf(out, sizeof(out), "%032x", n); return object_id(out); }
  object_attempt_id attempt(unsigned n = 1) { char out[33]; std::snprintf(out, sizeof(out), "%032x", n); return object_attempt_id(out); }
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
      check(at + count <= bytes.size(), "header write exceeded reserved prefix");
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
      check(fd >= 10 && fd <= 12, "wrong directory sync handle");
      return event(fd == 12 ? "sync leaf" : fd == 11 ? "sync first" : "sync root") ? 0 : -1;
    }
    int install(int, char const *, char const *) {
      ++installs;
      bool acknowledged = event("link");
      if (!acknowledged && !install_before_failure) return -1;
      check(private_exists && read_only, "installed writable or absent private output");
      if (final_exists) { errno = EEXIST; return -1; }
      final_exists = true;
      return acknowledged ? 0 : -1;
    }
    int remove_private(int, char const *) {
      ++removals;
      if (!event("unlink private")) return -1;
      check(final_exists, "removed only output name");
      private_exists = false;
      return 0;
    }
    int close(int fd) {
      check(fd >= 10 && fd <= 13 && opened[std::size_t(fd - 10)], "closed descriptor twice");
      opened[std::size_t(fd - 10)] = false; // close errors can consume the descriptor.
      return event("close") ? 0 : -1;
    }
  };
  struct discard_ops {
    static constexpr bool supported = true;
    std::uint64_t bytes = 0;
    object_sync_barrier barrier() const noexcept { return object_sync_barrier::fsync; }
    int open_root(std::filesystem::path const &) { return 10; }
    int make_directory(int, char const *) { return 0; }
    int open_directory(int parent, char const *) { return parent + 1; }
    int create_private(int, char const *) { return 13; }
    std::ptrdiff_t write(int, std::span<std::byte const> data) { bytes += data.size(); return std::ptrdiff_t(data.size()); }
    std::ptrdiff_t write_at(int, std::span<std::byte const> data, std::uint64_t) { return std::ptrdiff_t(data.size()); }
    int make_read_only(int) { return 0; }
    int sync_file(int) { return 0; }
    int sync_directory(int) { return 0; }
    int install(int, char const *, char const *) { return 0; }
    int remove_private(int, char const *) { return 0; }
    int close(int) { return 0; }
  };
  struct strings {
    using encoding = bit_encoding<>;
    using key_codec = fc_string_key<>;
    using value_codec = tombstone_value<string_value<>>;
  };
  struct integers {
    using encoding = bit_encoding<fixed_values<64>>;
    using key_codec = unsigned_key<64>;
    using value_codec = unsigned_value<64>;
  };
  struct bits {
    using encoding = bit_encoding<fixed_values<3>>;
    using key_codec = fc_bit_key<golomb<1>>;
    using value_codec = unsigned_value<3>;
  };
  using registry = bin<tip<strings>, bin<tip<integers>, tip<bits>>>;
  using standard = storage_policy<registry>;
  using unusual = storage_policy<registry, 3, golomb<1>, 16>;
  template <class P> auto fixture(unsigned count, bool variable = true) {
    sort_profile_writer<P> output;
    for (unsigned i = 0; i != count; ++i) {
      auto key = std::string(37, '\0') + std::to_string(1000 + i);
      output.template append<strings>(key, std::string(variable ? i % 13 : 7, char(i)));
    }
    for (unsigned i = 0; i != count; ++i) output.template append<integers>(i, i + 7);
    for (unsigned i = 0; i != 9; ++i) output.template append<bits>(bit_string::from_bits(std::string(i, '1')), i & 7);
    return output.finish();
  }
  template <class P, class Selector> void wire(sort_profile_array<P, Selector> const & array, bool short_writes = false) {
    model_ops ops; ops.short_writes = short_writes;
    sort_profile_file_writer<P, Selector, model_ops> writer("unused-root", id(), attempt(), ops);
    auto cursor = array.view().cursor();
    while (!cursor.done()) { auto row = cursor.peek(); writer.append_encoded(row.key.prefix, row.value); cursor.advance(); }
    auto receipt = writer.finish();
    auto expected = encoded_sort_sections<P>::from(array).materialize();
    check(ops.bytes == expected && receipt.bytes == expected.size(), "streaming KV03 bytes differ");
    check(writer.finished() && !writer.failed() && writer.size() == array.size(), "finished writer state");
    rejects([&] { writer.finish(); });
    check(ops.final_exists && !ops.private_exists, "seal did not install immutable object");
  }
  template <class P> void merges() {
    auto older = std::make_shared<sort_profile_array<P> const>(fixture<P>(47));
    auto newer = std::make_shared<sort_profile_array<P> const>(fixture<P>(31, false));
    sort_profile_merge_builder<P> expected(older, newer);
    while (!expected.done()) expected.step(7);
    auto array = expected.finish();
    model_ops ops;
    sort_profile_file_merge<P, sort_profile_array<P>, replace_native_value, registry_selector<typename P::registry_type>, model_ops>
      actual("unused-root", id(), attempt(), older, newer, ops);
    check(actual.step(0).keys == 0, "zero merge budget made progress");
    while (!actual.done()) actual.step(3);
    actual.finish();
    check(actual.materialized_keys() == 0, "file merge materialized replacement keys");
    check(ops.bytes == encoded_sort_sections<P>::from(array).materialize(), "file merge differs from owned merge");
    check(actual.progress().keys == array.size() && actual.finished() && !actual.failed(), "file merge progress");
  }
  struct deep_selector {
    template <class Input, class F> static decltype(auto) select(Input & in, F && fn) {
      for (unsigned i = 0; i != 8; ++i) if (in.read_bits(64)) throw std::invalid_argument("deep selector");
      return std::forward<F>(fn)(std::type_identity<strings>{}, in);
    }
    template <class S, class Out> static void write(Out & out) {
      static_assert(std::same_as<S, strings>); for (unsigned i = 0; i != 8; ++i) out.write_bits(0, 64);
    }
  };
  void shared_seeds_and_widths() {
    using p = storage_policy<bin<tip<strings>, sort_undefined>>;
    sort_profile_writer<p, deep_selector> deep;
    for (unsigned i = 0; i != 67; ++i) deep.append<strings>(std::to_string(1000 + i), "seven!!");
    auto a = deep.finish(); wire(a);
    check(a.dictionary().bit_size == 512 && a.seeds().bit_size == 0 && a.metadata().common_value_width.has_value(), "shared deep seed/fixed actualwidth");
    sort_profile_writer<p> variable;
    variable.append<strings>("a", "a"); variable.append<strings>("b", "b");
    variable.append<strings>("c", "longer"); variable.append<strings>("d", "a");
    auto b = variable.finish(); check(!b.metadata().common_value_width, "common width recovered after mismatch"); wire(b, true);
    sort_profile_writer<p> empty; wire(empty.finish());
  }
  void failures() {
    auto array = fixture<standard>(20);
    model_ops baseline;
    {
      sort_profile_file_writer<standard, registry_selector<registry>, model_ops> out("root", id(), attempt(), baseline);
      auto cursor = array.view().cursor();
      while (!cursor.done()) { auto item = cursor.peek(); out.append_encoded(item.key.prefix, item.value); cursor.advance(); }
      out.finish();
    }
    unsigned tested = 0;
    for (std::size_t point = 0; point != baseline.calls.size(); ++point) {
      auto const & name = baseline.calls[point];
      if (name != "write" && name != "pwrite" && name != "sync file" && name != "sync leaf" && name != "link") continue;
      model_ops ops;
      std::unique_ptr<sort_profile_file_writer<standard, registry_selector<registry>, model_ops>> writer;
      try { writer = std::make_unique<sort_profile_file_writer<standard, registry_selector<registry>, model_ops>>("root", id(), attempt(), ops); }
      catch (...) { throw; }
      if (point < ops.calls.size()) continue; // Constructor barriers have separate object_stream coverage.
      ops.fail_at = point;
      rejects([&] {
        auto cursor = array.view().cursor();
        while (!cursor.done()) { auto row = cursor.peek(); writer->append_encoded(row.key.prefix, row.value); cursor.advance(); }
        writer->finish();
      });
      check(writer->failed() && !writer->finished(), "I/O error did not poison output");
      auto calls = ops.calls.size(); rejects([&] { writer->finish(); });
      check(ops.calls.size() == calls && (ops.private_exists || ops.final_exists), "failed attempt retried or lost every name"); ++tested;
    }
    check(tested >= 5, "failure fixture missed sealing operations");
    // A payload failure before finish must poison the same shared merger.
    sort_profile_writer<standard> large;
    large.append<strings>("key", std::string(200'000, 'x'));
    auto source = std::make_shared<sort_profile_array<standard> const>(large.finish());
    model_ops ops;
    sort_profile_file_merge<standard, sort_profile_array<standard>, replace_native_value, registry_selector<registry>, model_ops>
      merge("root", id(), attempt(), source, source, ops);
    ops.fail_at = ops.calls.size();
    rejects([&] { merge.step(1); });
    check(merge.failed() && !merge.finished(), "merge payload failure not poisoned");
    rejects([&] { merge.step(1); }); rejects([&] { merge.finish(); });
  }
  void bounded_allocations() {
    using p = storage_policy<bin<tip<strings>, tip<bits>>, 15, golomb<1>>;
    sort_profile_writer<p> values;
    values.append<strings>("key", std::string(8 * 1024 * 1024, 'v'));
    auto source = std::make_shared<sort_profile_array<p> const>(values.finish());
    discard_ops ops;
    sort_profile_file_merge<p, sort_profile_array<p>, replace_native_value, registry_selector<typename p::registry_type>, discard_ops>
      merge("root", id(), attempt(), source, source, ops);
    largest_allocation = 0; allocation_limit = 128 * 1024;
    try { while (!merge.done()) merge.step(1); merge.finish(); }
    catch (...) { allocation_limit = std::numeric_limits<std::size_t>::max(); throw; }
    allocation_limit = std::numeric_limits<std::size_t>::max();
    check(largest_allocation <= 128 * 1024 && ops.bytes > 8 * 1024 * 1024 && merge.materialized_keys() == 0,
      "huge value allocated an output-sized temporary");
    // Both the FC suffix length and the next backspace have long unary codes.
    auto long_key = bit_string::from_bits(std::string(2 * 1024 * 1024, '0'));
    auto short_key = bit_string::from_bits("1");
    sort_profile_writer<p> keys; keys.append<bits>(long_key, 1); keys.append<bits>(short_key, 2);
    auto key_source = std::make_shared<sort_profile_array<p> const>(keys.finish());
    discard_ops key_ops;
    sort_profile_file_merge<p, sort_profile_array<p>, replace_native_value, registry_selector<typename p::registry_type>, discard_ops>
      key_merge("root", id(), attempt(), key_source, key_source, key_ops);
    largest_allocation = 0; allocation_limit = 128 * 1024;
    try { while (!key_merge.done()) key_merge.step(1); key_merge.finish(); }
    catch (...) { allocation_limit = std::numeric_limits<std::size_t>::max(); throw; }
    allocation_limit = std::numeric_limits<std::size_t>::max();
    check(largest_allocation <= 128 * 1024 && key_merge.materialized_keys() == 0 && key_ops.bytes > 512 * 1024,
      "long unary control or inherited key was materialized");
  }
  void actual_file(std::filesystem::path const & root) {
    auto older = std::make_shared<sort_profile_array<standard> const>(fixture<standard>(101));
    auto newer = std::make_shared<sort_profile_array<standard> const>(fixture<standard>(47, false));
    auto old_file = encoded_sort_sections<standard>::from(*older).seal(root, id(2), attempt(2));
    auto new_file = encoded_sort_sections<standard>::from(*newer).seal(root, id(3), attempt(3));
    using mapped_native = mapped_sort_profile<standard>;
    auto old_map = std::make_shared<mapped_native const>(mapped_native::open(old_file.path));
    auto new_map = std::make_shared<mapped_native const>(mapped_native::open(new_file.path));
    std::weak_ptr<mapped_native const> old_pin = old_map, new_pin = new_map;
    sort_profile_file_merge<standard, mapped_native> output(root, id(), attempt(), old_map, new_map);
    output.step(1); old_map.reset(); new_map.reset();
    std::filesystem::remove(old_file.path); std::filesystem::remove(new_file.path);
    check(!old_pin.expired() && !new_pin.expired(), "paused file merge released mapped inputs");
    while (!output.done()) output.step(7);
    auto receipt = output.finish(); auto mapped = mapped_sort_profile<standard>::open(receipt.path); mapped.scan();
    sort_profile_merge_builder<standard> reference(older, newer);
    while (!reference.done()) reference.step(3);
    auto expected = reference.finish();
    auto a = mapped.view().cursor(), b = expected.view().cursor();
    while (!b.done()) {
      check(!a.done() && !compare_bits(a.peek().key.prefix, b.peek().key.prefix) && !compare_bits(a.peek().value, b.peek().value), "mapped streamed result");
      a.advance(); b.advance();
    }
    check(a.done(), "mapped output trailing records");
  }
}
int main() {
  auto root = std::filesystem::temp_directory_path() / ("diet-sort-file-" + std::to_string(
    std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root);
  try {
    wire(fixture<standard>(47)); wire(fixture<unusual>(47));
    merges<standard>(); merges<unusual>(); shared_seeds_and_widths(); failures(); bounded_allocations(); actual_file(root);
    std::filesystem::remove_all(root); std::cout << "sort file tests passed\n";
  } catch (...) { std::filesystem::remove_all(root); throw; }
}
