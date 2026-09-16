/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks adaptive IX03 ownership, late spill, metadata limits and failure isolation.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/cola_adaptive_index.h>
#include <everett/sort_profile_file.h>

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
  using namespace everett;
  void check(bool value, char const * why) { if (!value) throw std::runtime_error(why); }
  template <class F> void rejects(F && fn) { try { fn(); } catch (std::exception const &) { return; } throw std::runtime_error("expected index rejection"); }
  object_id id(unsigned n) { char s[33]; std::snprintf(s, sizeof s, "%032x", n); return object_id(s); }
  object_attempt_id attempt(unsigned n) { return object_attempt_id(id(n).hex()); }
  struct temporary {
    std::filesystem::path root;
    temporary() { auto s = (std::filesystem::temp_directory_path() / "everett-world-file-XXXXXX").string();
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
  template <class P> struct factory {
    using destination = cola_detail::index_destination<P, file_ops, spool_ops>;
    std::filesystem::path root;
    file_ops * ops;
    spool_ops * spool;
    unsigned * calls;
    cola_file_dependencies deps;
    std::unique_ptr<destination> operator()() {
      ++*calls;
      return std::make_unique<destination>(root, id(5), attempt(1), deps, *ops, *spool);
    }
  };
  using P = storage_policy<string_registry, 3, exponential_golomb<2>, 16>;
  using Native = profile_array<P>;
  using Index = cola_index<P>;
  using Builder = cola_adaptive_index_builder<P, Native, void, factory<P>, file_ops, spool_ops>;
  cola_file_dependencies dependencies{id(1), blob_identity{id(2), id(3)}, id(4)};
  void parity() {
    auto a = native<P>(2), b = native<P>(3), c = native<P>(5);
    auto main = std::make_shared<Index const>(Index::adopt_native(b));
    auto reference = Index::adopt_native(a, main, c);
    auto expected = encode_cola_sections(reference, id(1), dependencies.main, id(4)).materialize();
    for (unsigned mode = 0; mode != 5; ++mode) {
      temporary dir; file_ops ops; spool_ops spool; unsigned calls = 0;
      output_budget budget(mode == 2 ? 0 : 1u << 20);
      auto held = mode == 3 ? budget.try_acquire(budget.limit()) : std::nullopt;
      auto before = budget.used();
      Builder out({dir.root, &ops, &spool, &calls, dependencies}, budget, mode == 1 ? 0 : mode == 4 ? 1 : 128u << 10, a, main, c);
      check(out.spilled() == (mode == 1 || mode == 2), "eager output options");
      if (!out.spilled()) rejects([&] { (void)out.paths(); });
      check(out.step(0) == 0, "zero work advanced");
      while (!out.done()) out.step(3);
      auto result = out.finish();
      check(out.finished() && !out.failed(), "adaptive completion");
      if (!mode) {
        auto owned = std::get<std::shared_ptr<Index const>>(std::move(result));
        check(!out.spilled() && calls == 0 && ops.calls == 0 && spool.calls == 0, "small output touched filesystem");
        check(budget.used() > before && budget.used() <= 128u << 10, "retained output charge");
        check(encode_cola_sections(*owned, id(1), dependencies.main, id(4)).materialize() == expected, "owned IX03 mismatch");
        auto copy = owned; owned.reset(); check(budget.used() > before, "early lease release");
        copy.reset(); check(budget.used() == before, "lease leaked");
      } else {
        auto receipt = std::get<object_seal_receipt>(std::move(result));
        check(out.spilled() && calls == 1 && out.spooled_bytes(), "spill was not unique");
        check(bytes(receipt.path) == expected, "adaptive streamed IX03 mismatch");
        auto mapped = mapped_cola_index<P>::open(receipt.path); mapped.scan();
        check(budget.used() == before, "streamed output retained lease");
      }
      rejects([&] { out.finish(); }); rejects([&] { out.step(1); });
    }
  }
  template <class Policy> struct blind_native {
    std::uint64_t size() const noexcept { return 100000; }
    profile_view<Policy> view() const { throw std::runtime_error("terminal native payload read"); }
  };
  void metadata_spill() {
    temporary dir; file_ops ops; spool_ops spool; unsigned calls = 0; output_budget budget(1u << 20);
    using N = blind_native<P>;
    cola_adaptive_index_builder<P, N, void, factory<P>, file_ops, spool_ops> out(
      {dir.root, &ops, &spool, &calls, {id(1), {}, {}}}, budget, 128u << 10, std::make_shared<N const>());
    while (!out.done()) out.step(256);
    check(!out.spilled() && !calls, "terminal opened early");
    auto receipt = std::get<object_seal_receipt>(out.finish());
    check(out.spilled() && calls == 1 && !out.spooled_bytes() && !budget.used(), "large terminal retained navigation");
    auto mapped = mapped_cola_index<P>::open(receipt.path); mapped.scan();
    check(mapped.virtual_size() == 100000, "terminal count");
  }
  void partial_routes() {
    auto empty = std::make_shared<Native const>(Native::build({}));
    std::array<profile_record, 1> large{{{bit_string::from_bytes(std::string(70000, 'b')), {}}}};
    auto secondary = std::make_shared<Native const>(Native::build(large));
    for (unsigned tail = 0; tail != 8; ++tail) {
      auto key = bit_string::from_bytes(std::string("a\0\0", 3));
      // Length 15 crosses an exponential-Golomb width boundary; 21 gives
      // remainder seven after the retained-prefix and suffix-length controls.
      profile_detail::resize(key, tail == 7 ? 21 : 8 + tail);
      std::array<profile_record, 1> records{{{std::move(key), {}}}};
      auto own = std::make_shared<Native const>(Native::build(records));
      auto main = std::make_shared<Index const>(Index::adopt_native(own));
      auto expected = Index::adopt_native(empty, main, secondary);
      check((expected.borrowed(0).metadata().extent & 7) == tail, "partial route fixture");
      temporary dir; file_ops ops; spool_ops spool; unsigned calls = 0; output_budget budget(1u << 20);
      Builder out({dir.root, &ops, &spool, &calls, dependencies}, budget, 128u << 10, empty, main, secondary);
      while (!out.done()) out.step(1);
      check(out.spilled(), "large secondary did not spill");
      auto receipt = std::get<object_seal_receipt>(out.finish());
      check(calls == 1 && bytes(receipt.path) == encode_cola_sections(expected, id(1), dependencies.main, id(4)).materialize(),
        "spill lost other route's partial byte");
    }
  }
  void failures() {
    auto a = native<P>(2), b = native<P>(3), c = native<P>(5);
    auto main = std::make_shared<Index const>(Index::adopt_native(b));
    for (unsigned mode = 1; mode != 8; ++mode) {
      temporary dir; file_ops ops; spool_ops spool; unsigned calls = 0; output_budget budget(1u << 20);
      {
        Builder out({dir.root, &ops, &spool, &calls, dependencies}, budget, 1, a, main, c);
        check(!calls, "failure fixture did I/O early");
        if (mode < 4) ops.fail = mode; else spool.fail = mode - 3;
        rejects([&] { while (!out.done()) out.step(1); (void)out.finish(); });
        check(out.failed() && !out.finished() && !budget.used(), "failure not poisoned");
        auto io = ops.calls + spool.calls; rejects([&] { out.finish(); }); rejects([&] { out.step(1); });
        check(io == ops.calls + spool.calls, "failed output retried I/O");
      }
      check(spool.opens == spool.closes, "spool descriptor leak");
      for (auto const & entry : std::filesystem::recursive_directory_iterator(dir.root))
        check(entry.path().extension() != ".secondary", "spool name leak");
    }
  }
  void bounded_payload() {
    auto a = native<P>(1, 97, 32768), b = native<P>(1, 97, 32768);
    auto main = std::make_shared<Index const>(Index::adopt_native(b));
    temporary dir; file_ops ops; spool_ops spool; unsigned calls = 0; output_budget budget(1u << 20);
    Builder out({dir.root, &ops, &spool, &calls, dependencies}, budget, 128u << 10, a, main, b);
    largest_allocation = 0; allocation_limit = 256 * 1024;
    try { while (!out.done()) out.step(1); (void)out.finish(); }
    catch (...) { allocation_limit = std::numeric_limits<std::size_t>::max(); throw; }
    allocation_limit = std::numeric_limits<std::size_t>::max();
    check(largest_allocation <= 256 * 1024 && out.spooled_bytes() > 1024 * 1024 && calls == 1 && !budget.used(),
      "adaptive borrowed payload allocated in RAM");
  }
}
int main() {
  try { parity(); metadata_spill(); partial_routes(); failures(); bounded_payload(); }
  catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
