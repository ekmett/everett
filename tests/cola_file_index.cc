/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks streamed IX03 equivalence, exact dependencies, bounded buffering and failures.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/cola_file_index.h>
#include <diet/sort_profile_file.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <new>

namespace {
  thread_local std::size_t allocation_limit = std::numeric_limits<std::size_t>::max(), largest_allocation = 0;
  void * allocate(std::size_t n) {
    largest_allocation = std::max(largest_allocation, n);
    if (n > allocation_limit) throw std::bad_alloc();
    if (auto p = std::malloc(n ? n : 1)) return p;
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
  template <class F> void rejects(F && fn) { try { fn(); } catch (std::exception const &) { return; } throw std::runtime_error("expected index rejection"); }
  object_id id(unsigned n) { char s[33]; std::snprintf(s, sizeof s, "%032x", n); return object_id(s); }
  object_attempt_id attempt(unsigned n) { return object_attempt_id(id(n).hex()); }
  struct temporary {
    std::filesystem::path root;
    temporary() { auto s = (std::filesystem::temp_directory_path() / "diet-cola-file-XXXXXX").string();
      if (!::mkdtemp(s.data())) throw std::runtime_error("mkdtemp");
      root = s; }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  std::vector<std::byte> bytes(std::filesystem::path const & path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate); auto n = input.tellg(); check(n >= 0, "read size");
    std::vector<std::byte> out(static_cast<std::size_t>(n)); input.seekg(0); input.read(reinterpret_cast<char *>(out.data()), n);
    check(bool(input), "read bytes"); return out;
  }
  template <class P> auto native(unsigned divisor, unsigned n = 97, unsigned key_bytes = 18) {
    std::vector<profile_record> records;
    for (unsigned i = 0; i != n; ++i) if (i % divisor == 0) {
      char prefix[16]; std::snprintf(prefix, sizeof prefix, "%08x", i);
      auto key = std::string(prefix) + std::string(key_bytes, char('a' + i % 19));
      auto bits = bit_string::from_bytes(key); if constexpr (P::unit == profile_unit::bit) profile_detail::resize(bits, bits.bit_size - i % 5);
      records.push_back({std::move(bits), {}});
    }
    return std::make_shared<profile_array<P> const>(profile_array<P>::build(records));
  }
  template <class P, class Native> void equivalent(std::shared_ptr<Native const> a,
      std::shared_ptr<Native const> b, std::shared_ptr<Native const> c) {
    using Index = cola_index<P, Native>;
    auto main = std::make_shared<Index const>(Index::adopt_native(b));
    auto reference = Index::adopt_native(a, main, c);
    temporary dir;
    cola_file_dependencies deps{id(1), blob_identity{id(2), id(3)}, id(4)};
    cola_file_index_builder<P, Native> file(dir.root, id(5), attempt(1), deps, a, main, c);
    check(file.step(0) == 0, "zero budget advanced");
    while (!file.done()) file.step(3);
    auto receipt = file.finish();
    check(file.finished() && !file.failed() && file.spooled_bytes(), "index completion/spool");
    auto expected = encode_cola_sections(reference, deps.native, deps.main, deps.secondary).materialize();
    check(bytes(receipt.path) == expected, "streamed IX03 differs from owning bytes");
    auto mapped = mapped_cola_index<P>::open(receipt.path); mapped.scan();
    check(mapped.native_id() == deps.native && mapped.main_id() == deps.main && mapped.secondary_id() == deps.secondary, "lost exact dependencies");
    bool false_borrow = false;
    for (unsigned route = 0; route != 2; ++route)
      for (auto flag : reference.false_borrow_bits(route)) false_borrow |= flag != std::byte{0};
    check(false_borrow, "fixture did not cover false borrows");
    for (auto const & entry : std::filesystem::recursive_directory_iterator(dir.root))
      check(entry.path().extension() != ".secondary", "named spool survived success");
    rejects([&] { file.step(1); }); rejects([&] { file.finish(); });
  }
  struct integers { using encoding = bit_encoding<fixed_values<8>>; using key_codec = unsigned_key<16>; using value_codec = unsigned_value<8>; };
  using strings = unsorted<std::optional<std::string>>;
  template <std::uint64_t K> void mixed() {
    using P = storage_policy<bin<tip<integers>, tip<strings>>, K, golomb<3>, 16>;
    auto build = [](unsigned divisor) {
      sort_profile_writer<P> writer;
      for (unsigned n = 0; n != 71; ++n) if (n % divisor == 0) writer.template append<integers>(n, n);
      for (unsigned n = 0; n != 71; ++n) if (n % divisor == 0) {
        char key[16]; std::snprintf(key, sizeof key, "key-%08x", n);
        writer.template append<strings>(key, std::to_string(n));
      }
      return std::make_shared<sort_profile_array<P> const>(writer.finish());
    };
    equivalent<P>(build(2), build(3), build(5));
  }
  template <class P> struct blind_native {
    std::uint64_t size() const noexcept { return 513; }
    profile_view<P> view() const { throw std::runtime_error("terminal index read native payload"); }
  };
  void terminal() {
    using P = string_policy;
    auto source = std::make_shared<blind_native<P> const>(); temporary dir;
    cola_file_index_builder<P, blind_native<P>> out(dir.root, id(2), attempt(2), {id(1), {}, {}}, source);
    while (!out.done()) out.step(19);
    auto receipt = out.finish(); check(!out.spooled_bytes(), "terminal index created spool");
    auto mapped = mapped_cola_index<P>::open(receipt.path); mapped.scan(); check(mapped.virtual_size() == 513, "terminal count");
  }
  struct file_ops : posix_object_ops {
    unsigned fail = 0, calls = 0; bool short_io = false;
    std::ptrdiff_t write(int fd, std::span<std::byte const> data) noexcept {
      ++calls; if (fail == 1) { fail = 0; errno = EIO; return -1; }
      return posix_object_ops::write(fd, short_io ? data.first(std::min(data.size(), std::size_t(17))) : data);
    }
    std::ptrdiff_t write_at(int fd, std::span<std::byte const> data, std::uint64_t at) noexcept {
      ++calls; if (fail == 2) { fail = 0; errno = EIO; return -1; }
      return posix_object_ops::write_at(fd, data, at);
    }
    int sync_file(int fd) noexcept { ++calls; if (fail == 3) { fail = 0; errno = EIO; return -1; } return posix_object_ops::sync_file(fd); }
  };
  struct spool_ops : posix_index_spool_ops {
    unsigned fail = 0, calls = 0, opens = 0, closes = 0; bool short_io = false;
    int create(std::filesystem::path const & path) noexcept {
      ++calls; if (fail == 1) { fail = 0; errno = EIO; return -1; } ++opens; return posix_index_spool_ops::create(path);
    }
    int remove(std::filesystem::path const & path) noexcept {
      ++calls; if (fail == 2) { fail = 0; errno = EIO; return -1; } return posix_index_spool_ops::remove(path);
    }
    std::ptrdiff_t write(int fd, std::span<std::byte const> data) noexcept {
      ++calls; if (fail == 3) { fail = 0; errno = EIO; return -1; }
      return posix_index_spool_ops::write(fd, short_io ? data.first(std::min(data.size(), std::size_t(13))) : data);
    }
    std::ptrdiff_t read_at(int fd, std::span<std::byte> data, std::uint64_t at) noexcept {
      ++calls; if (fail == 4) { fail = 0; errno = EIO; return -1; }
      return posix_index_spool_ops::read_at(fd, short_io ? data.first(std::min(data.size(), std::size_t(11))) : data, at);
    }
    int close(int fd) noexcept { ++closes; return posix_index_spool_ops::close(fd); }
  };
  void failures() {
    using P = string_policy; using Native = profile_array<P>; using Index = cola_index<P>;
    auto a = native<P>(2), b = native<P>(3), c = native<P>(5);
    auto main = std::make_shared<Index const>(Index::adopt_native(b));
    auto expected = Index::adopt_native(a, main, c);
    for (unsigned mode = 0; mode != 8; ++mode) {
      temporary dir; file_ops ops; spool_ops spool;
      {
        cola_file_index_builder<P, Native, void, file_ops, spool_ops> out(dir.root, id(5), attempt(1),
          {id(1), blob_identity{id(2), id(3)}, id(4)}, a, main, c, ops, spool);
        if (!mode) { ops.short_io = true; spool.short_io = true; }
        else if (mode < 4) ops.fail = mode;
        else spool.fail = mode - 3;
        auto run = [&] { while (!out.done()) out.step(1); return out.finish(); };
        if (!mode) {
          auto receipt = run(); check(bytes(receipt.path) == encode_cola_sections(expected, id(1), blob_identity{id(2), id(3)}, id(4)).materialize(), "short I/O bytes");
        } else {
          rejects(run); check(out.failed() && !out.finished(), "index failure not poisoned");
          auto calls = ops.calls + spool.calls; rejects([&] { out.finish(); }); rejects([&] { out.step(1); });
          check(calls == ops.calls + spool.calls, "failed index retried I/O");
        }
      }
      check(spool.opens == spool.closes, "spool leaked descriptor");
      for (auto const & entry : std::filesystem::recursive_directory_iterator(dir.root))
        check(entry.path().extension() != ".secondary", "named spool survived failure");
    }
  }
  void bounded_payload() {
    using P = storage_policy<unsorted<std::optional<std::string>>, 3>;
    auto a = native<P>(1, 97, 32768), b = native<P>(1, 97, 32768);
    auto main = std::make_shared<cola_index<P> const>(cola_index<P>::adopt_native(b));
    temporary dir;
    cola_file_index_builder<P> out(dir.root, id(5), attempt(1), {id(1), blob_identity{id(2), id(3)}, id(4)}, a, main, b);
    largest_allocation = 0; allocation_limit = 256 * 1024;
    try { while (!out.done()) out.step(1); out.finish(); }
    catch (...) { allocation_limit = std::numeric_limits<std::size_t>::max(); throw; }
    allocation_limit = std::numeric_limits<std::size_t>::max();
    check(largest_allocation <= 256 * 1024 && out.spooled_bytes() > 1024 * 1024, "borrowed payload allocated in RAM");
  }
}
int main() {
  try {
    using Byte = storage_policy<unsorted<std::optional<std::string>>, 3>;
    using Bit = storage_policy<string_registry, 15, exponential_golomb<3>, 16>;
    equivalent<Byte>(native<Byte>(2), native<Byte>(3), native<Byte>(5));
    equivalent<Bit>(native<Bit>(2), native<Bit>(3), native<Bit>(5));
    mixed<3>(); mixed<15>(); terminal(); failures(); bounded_payload();
  } catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
