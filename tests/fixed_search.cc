/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks fixed-word search against independent sorted-array and guarded-memory oracles.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/fixed_search.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <span>
#include <stdexcept>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }

  template <class Error, class F> void rejects(F && action) {
    try { action(); }
    catch (Error const &) { return; }
    throw std::runtime_error("fixed search accepted invalid bounds or wrong exception");
  }

  template <std::size_t Words> using key = std::array<std::uint32_t, Words>;
  template <std::size_t Words> using view = everett::fixed_key_view<Words>;

  template <std::size_t Words> bool less(key<Words> const & a, key<Words> const & b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
  }

  template <std::size_t Words> std::vector<std::byte> encode(std::span<key<Words> const> keys) {
    std::vector<std::byte> bytes(keys.size() * Words * 4);
    auto at = bytes.begin();
    for (auto const & value : keys)
      for (auto word : value)
        for (unsigned byte = 0; byte != 4; ++byte)
          *at++ = std::byte((word >> (byte * 8)) & 255);
    return bytes;
  }

  template <std::size_t Words> key<Words> adjacent(key<Words> value, bool next) {
    for (std::size_t i = Words; i--;) {
      if (next) {
        if (++value[i]) return value;
      } else {
        if (value[i]-- != 0) return value;
      }
    }
    return value; // Wraparound still supplies a valid independent query.
  }

  template <std::size_t Words> std::vector<key<Words>> queries(std::span<key<Words> const> keys) {
    std::vector<key<Words>> result(1);
    key<Words> all{}; all.fill(UINT32_MAX); result.push_back(all);
    for (std::size_t i = 0; i < keys.size(); i += std::max<std::size_t>(1, keys.size() / 24)) {
      result.push_back(keys[i]);
      result.push_back(adjacent(keys[i], false));
      result.push_back(adjacent(keys[i], true));
    }
    if (!keys.empty()) result.push_back(keys.back());
    for (std::size_t word = 0; word != Words; ++word) {
      key<Words> q{}; q.fill(0x80000000);
      q[word] = 0x7fffffff; result.push_back(q);
      q[word] = 0x80000000; result.push_back(q);
    }
    return result;
  }

  template <std::size_t Words> void check(view<Words> data,
      std::span<key<Words> const> keys, std::span<key<Words> const> probes) {
    require(data.size() == keys.size(), "fixed search size");
    for (std::size_t i = 0; i != keys.size(); ++i)
      require(data.key_at(i) == keys[i], "little-endian key load");
    for (auto const & q : probes) {
      auto lower = std::size_t(std::lower_bound(keys.begin(), keys.end(), q, less<Words>) - keys.begin());
      auto upper = std::size_t(std::upper_bound(keys.begin(), keys.end(), q, less<Words>) - keys.begin());
      require(data.lower_bound(q) == lower, "default lower_bound oracle");
      require(data.upper_bound(q) == upper, "default upper_bound oracle");
      require(data.lower_bound_binary(q) == lower, "binary lower_bound oracle");
      require(data.upper_bound_binary(q) == upper, "binary upper_bound oracle");
      require(data.lower_bound_simd(q) == lower, "SIMD lower_bound oracle");
      require(data.upper_bound_simd(q) == upper, "SIMD upper_bound oracle");
    }
  }

  template <std::size_t Words> std::vector<key<Words>> fixture(std::size_t count, unsigned pattern) {
    constexpr std::array<std::uint32_t, 8> edges{0, 1, 0x7ffffffe, 0x7fffffff,
      0x80000000, 0x80000001, 0xfffffffe, UINT32_MAX};
    std::mt19937_64 random(0x4649584544000000ULL + Words * 1024 + count);
    std::vector<key<Words>> keys(count);
    for (std::size_t i = 0; i != count; ++i) {
      auto & k = keys[i];
      switch (pattern) {
        case 0: k.fill(UINT32_MAX); break; // Every query may equal an entire window.
        case 1:
          k.fill(0x80000000); k.back() = std::uint32_t(i / 3); break;
        case 2:
          k.fill(0x80000000); k[i % Words] = edges[(i / Words) % edges.size()]; break;
        default:
          for (auto & word : k) word = std::uint32_t(random());
          if (i && i % 3 == 0) k = keys[i - 1];
          break;
      }
    }
    std::sort(keys.begin(), keys.end(), less<Words>);
    return keys;
  }

  template <std::size_t Words> void check_subranges(view<Words> data,
      std::span<key<Words> const> keys, std::span<key<Words> const> probes) {
    auto n = keys.size();
    if (n <= 8) {
      for (std::size_t first = 0; first <= n; ++first)
        for (std::size_t count = 0; count <= n - first; ++count)
          check<Words>(data.subview(first, count), keys.subspan(first, count), probes);
    } else {
      std::mt19937_64 random(n * 101 + Words);
      for (unsigned trial = 0; trial != 6; ++trial) {
        auto first = std::size_t(random() % (n + 1));
        auto count = std::size_t(random() % (n - first + 1));
        auto sub = data.subview(first, count);
        check<Words>(sub, keys.subspan(first, count), probes);
        auto cut = count / 2;
        check<Words>(sub.subview(cut, count - cut), keys.subspan(first + cut, count - cut), probes);
      }
    }
    check<Words>(data.subview(n, 0), keys.subspan(n, 0), probes);
    rejects<std::out_of_range>([&] { (void)data.key_at(n); });
    rejects<std::out_of_range>([&] { (void)data.key_at(std::numeric_limits<std::size_t>::max()); });
    rejects<std::out_of_range>([&] { (void)data.subview(n + 1, 0); });
    rejects<std::out_of_range>([&] { (void)data.subview(0, n + 1); });
    rejects<std::out_of_range>([&] { (void)data.subview(n, std::numeric_limits<std::size_t>::max()); });
    rejects<std::out_of_range>([&] { (void)data.subview(std::numeric_limits<std::size_t>::max(), 1); });
  }

  template <std::size_t Words> void ordinary() {
    std::vector<key<Words>> empty;
    auto empty_queries = queries<Words>(empty);
    check<Words>(view<Words>{}, empty, empty_queries);
    check_subranges<Words>(view<Words>{}, empty, empty_queries);
    std::vector<std::size_t> sizes;
    for (std::size_t n = 0; n <= 70; ++n) sizes.push_back(n);
    for (auto n : {95, 127, 128, 129, 255, 256, 257, 1025, 4097}) sizes.push_back(std::size_t(n));
    for (auto n : sizes) for (unsigned pattern = 0; pattern != 4; ++pattern) {
      auto keys = fixture<Words>(n, pattern);
      auto bytes = encode<Words>(keys);
      auto probes = queries<Words>(keys);
      // Cover every address residue modulo64, including non-u32 alignment.
      for (std::size_t alignment = 0; alignment != 64; ++alignment) {
        std::vector<std::byte> backing(bytes.size() + 128, std::byte{0xa5});
        std::copy(bytes.begin(), bytes.end(), backing.begin() + alignment);
        auto before = backing;
        view<Words> data(std::span<std::byte const>(backing).subspan(alignment, bytes.size()));
        check<Words>(data, keys, probes);
        if (alignment == n % 64) check_subranges<Words>(data, keys, probes);
        require(backing == before, "search modified caller bytes or surrounding canaries");
      }
    }
    std::array<std::byte, Words * 8> bytes{};
    for (std::size_t extent = 1; extent != bytes.size(); ++extent)
      if (extent % (Words * 4)) rejects<std::invalid_argument>([&] {
        (void)view<Words>(std::span<std::byte const>(bytes).first(extent));
      });
  }

#if defined(__unix__) || defined(__APPLE__)
  struct guarded_pages {
    std::byte * address = nullptr;
    std::size_t page = 0;
    guarded_pages() {
      auto n = ::sysconf(_SC_PAGESIZE);
      require(n > 0, "read page size"); page = std::size_t(n);
      auto p = ::mmap(nullptr, page * 4, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      require(p != MAP_FAILED, "map guarded search pages"); address = static_cast<std::byte *>(p);
      if (::mprotect(address + page, page * 2, PROT_READ | PROT_WRITE)) {
        ::munmap(address, page * 4); address = nullptr;
        throw std::runtime_error("enable search pages");
      }
    }
    guarded_pages(guarded_pages const &) = delete;
    guarded_pages & operator=(guarded_pages const &) = delete;
    ~guarded_pages() { if (address) ::munmap(address, page * 4); }
    std::byte * begin() const { return address + page; }
    std::byte * end() const { return address + page * 3; }
  };

  template <std::size_t Words> void guarded() {
    guarded_pages memory;
    std::vector<key<Words>> empty;
    auto empty_queries = queries<Words>(empty);
    // Both pointers are inside inaccessible mappings, not readable sentinels.
    for (auto p : {memory.address + 1, memory.end() + 1}) {
      view<Words> data(std::span<std::byte const>(p, std::size_t{0}));
      check<Words>(data, empty, empty_queries);
      check_subranges<Words>(data, empty, empty_queries);
      for (std::size_t extent = 1; extent < Words * 4; ++extent)
        rejects<std::invalid_argument>([&] { (void)view<Words>(std::span<std::byte const>(p, extent)); });
    }
    auto run = [&](std::byte * start, std::vector<key<Words>> const & keys) {
      auto bytes = encode<Words>(keys);
      require(start >= memory.begin() && start + bytes.size() <= memory.end(), "guard fixture extent");
      std::fill(memory.begin(), memory.end(), std::byte{0xa5});
      std::copy(bytes.begin(), bytes.end(), start);
      std::vector<std::byte> before(memory.begin(), memory.end());
      require(!::mprotect(memory.begin(), memory.page * 2, PROT_READ), "protect search input from writes");
      auto probes = queries<Words>(keys);
      view<Words> data(std::span<std::byte const>(start, bytes.size()));
      check<Words>(data, keys, probes);
      check_subranges<Words>(data, keys, probes);
      require(std::equal(before.begin(), before.end(), memory.begin()), "guarded search modified bytes");
      require(!::mprotect(memory.begin(), memory.page * 2, PROT_READ | PROT_WRITE), "restore writable fixture");
    };
    for (std::size_t count = 1; count <= 32; ++count) for (unsigned pattern = 0; pattern != 4; ++pattern) {
      auto keys = fixture<Words>(count, pattern);
      auto length = count * Words * 4;
      // Exact end at PROT_NONE makes every read beyond a short tail fault;
      // the opposite placement likewise rejects aligning a load before input.
      run(memory.end() - length, keys);
      run(memory.begin(), keys);
      if (count > 1)
        for (std::size_t shift = 0; shift < Words * 4; ++shift)
          run(memory.begin() + memory.page - length / 2 + shift, keys);
      // A longer sorted window crosses two readable pages and still ends
      // exactly against the upper guard, exercising fallback and final tails.
      auto large = fixture<Words>(memory.page / (Words * 4) + count, pattern);
      run(memory.end() - large.size() * Words * 4, large);
    }
  }
#else
  template <std::size_t Words> void guarded() {}
#endif

  template <std::size_t Words> void test() { ordinary<Words>(); guarded<Words>(); }
}

int main() {
  try {
    test<1>(); test<2>(); test<4>();
    std::cout << "fixed search sorted-array and boundary oracles passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
