/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Counts requested C++ allocations for one complete borrowed-only index stage.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include "policy_compat.h"

#include <everett/index_builder.h>
#include <everett/sections.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#define EVERETT_BENCH_ALLOCATIONS 1
namespace allocation_probe {
  struct totals { std::uint64_t requested = 0, peak = 0, live = 0, calls = 0; };
#if defined(EVERETT_BENCH_ALLOCATIONS)
  thread_local totals counts;
  thread_local std::uint64_t epoch = 0;
  thread_local bool active = false;
  struct header { void * raw; std::size_t size; std::uint64_t epoch; };
  void begin() { ++epoch; counts = {}; active = true; }
  totals end() { active = false; return counts; }
  void * allocate(std::size_t size, std::size_t alignment) {
    alignment = std::max(alignment, alignof(header));
    auto overhead = sizeof(header) + alignment - 1;
    auto extent = std::max(size, std::size_t{1});
    if (extent > std::numeric_limits<std::size_t>::max() - overhead) throw std::bad_alloc();
    auto raw = std::malloc(extent + overhead);
    if (!raw) throw std::bad_alloc();
    auto address = (reinterpret_cast<std::uintptr_t>(raw) + overhead) & ~(alignment - 1);
    ::new (reinterpret_cast<void *>(address - sizeof(header))) header{raw, size, active ? epoch : 0};
    if (active) {
      counts.requested += size; counts.live += size; ++counts.calls;
      counts.peak = std::max(counts.peak, counts.live);
    }
    return reinterpret_cast<void *>(address);
  }
  void release(void * pointer) noexcept {
    if (!pointer) return;
    auto stored = reinterpret_cast<header *>(reinterpret_cast<std::uintptr_t>(pointer) - sizeof(header));
    if (stored->epoch && stored->epoch == epoch) counts.live -= stored->size;
    std::free(stored->raw);
  }
#else
  void begin() {}
  totals end() { return {}; }
#endif
}
#if defined(EVERETT_BENCH_ALLOCATIONS)
void * operator new(std::size_t size) { return allocation_probe::allocate(size, alignof(std::max_align_t)); }
void * operator new[](std::size_t size) { return allocation_probe::allocate(size, alignof(std::max_align_t)); }
void * operator new(std::size_t size, std::align_val_t align) { return allocation_probe::allocate(size, std::size_t(align)); }
void * operator new[](std::size_t size, std::align_val_t align) { return allocation_probe::allocate(size, std::size_t(align)); }
void operator delete(void * p) noexcept { allocation_probe::release(p); }
void operator delete[](void * p) noexcept { allocation_probe::release(p); }
void operator delete(void * p, std::size_t) noexcept { allocation_probe::release(p); }
void operator delete[](void * p, std::size_t) noexcept { allocation_probe::release(p); }
void operator delete(void * p, std::align_val_t) noexcept { allocation_probe::release(p); }
void operator delete[](void * p, std::align_val_t) noexcept { allocation_probe::release(p); }
void operator delete(void * p, std::size_t, std::align_val_t) noexcept { allocation_probe::release(p); }
void operator delete[](void * p, std::size_t, std::align_val_t) noexcept { allocation_probe::release(p); }
#endif

namespace {
  using namespace everett;
  void require(bool value, char const * message) {
    if (!value) throw std::runtime_error(message);
  }
  template <class P> void run(char const * profile, unsigned prefix, bool coded) {
    constexpr unsigned count = 4096;
    std::vector<bit_string> keys;
    std::vector<profile_coded_sample<P>> frames;
    profile_sample_encoder<P> encoder;
    for (unsigned i = 0; i < count; ++i) {
      std::string bytes(prefix, 'p');
      for (unsigned shift = 32; shift; shift -= 8) bytes += char(i >> (shift - 8));
      auto key = bit_string::from_bytes(bytes);
      if constexpr (P::unit == profile_unit::bit) {
        key.bytes.push_back(std::byte{0xa0}); key.bit_size += 3;
      }
      frames.push_back(encoder.encode(key.view(), std::uint64_t(i) * P::group_size));
      keys.push_back(std::move(key));
    }
    auto source = std::make_shared<profile_array<P> const>(profile_array<P>::build({}));
    allocation_probe::begin();
    index_builder<P> builder(source);
    for (unsigned i = 0; i < count; ++i) {
      if (coded) builder.push(frames[i]);
      else builder.push(keys[i].view(), std::uint64_t(i) * P::group_size);
      builder.step(1);
      if (builder.has_output()) (void)builder.take_coded_output();
    }
    builder.close_input();
    require(builder.done(), "allocation probe did not drain");
    auto index = builder.finish_index(std::uint64_t(count) * P::group_size);
    auto allocations = allocation_probe::end();
    auto cursor = index.borrowed().view().cursor();
    for (auto const & key : keys) {
      require(!cursor.done(), "missing borrowed occurrence");
      auto actual = cursor.peek().key.prefix;
      require(actual.size() == key.bit_size, "borrowed key extent");
      for (std::uint64_t i = 0; i < actual.size(); ++i)
        require(actual.at(i) == key.view().at(i), "borrowed key bits");
      cursor.advance();
    }
    require(cursor.done(), "extra borrowed occurrence");
    auto bytes = encode_index_sections(index, object_id("11111111111111111111111111111111"),
      blob_identity{object_id("22222222222222222222222222222222"), object_id("33333333333333333333333333333333")}).materialize();
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (auto byte : bytes) hash = (hash ^ std::to_integer<unsigned>(byte)) * 0x100000001b3ull;
    std::cout << profile << ',' << prefix << ',' << (coded ? "coded" : "full") << ',' << count << ','
      << allocations.calls << ',' << allocations.requested << ',' << allocations.peak << ','
      << sizeof(builder) << ',' << hash << '\n';
  }
}
int main() try {
  std::cout << "profile,prefix_bytes,input,records,allocations,requested_bytes,peak_bytes,builder_bytes,checksum\n";
  for (auto prefix : {0u, 4096u}) for (bool coded : {false, true}) {
    run<everett_bench::policy<everett::profile_unit::byte>>("byte", prefix, coded);
    run<everett_bench::policy<everett::profile_unit::bit>>("bit", prefix, coded);
  }
  return 0;
} catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
