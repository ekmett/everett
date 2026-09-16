/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Compares bounded fixed-key search paths on identical windows.
 * \license
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/fixed_search.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using clock_type = std::chrono::steady_clock;
template <std::size_t W> using key = typename everett::fixed_key_view<W>::key_type;

template <std::size_t W> struct fixture {
  std::vector<std::byte> bytes;
  std::vector<everett::fixed_key_view<W>> windows;
  struct request { std::size_t window; key<W> query; std::size_t expected; };
  std::vector<request> requests;
  fixture(std::size_t count, std::size_t window_count, bool shared) {
    std::mt19937_64 rng(42 + W + count * 100);
    auto stride = count * W * 4 + 64;
    bytes.resize(window_count * stride);
    std::vector<std::vector<key<W>>> keys(window_count);
    for (std::size_t w = 0; w < window_count; ++w) {
      auto &a = keys[w];
      for (std::size_t i = 0; i < count; ++i) {
        key<W> k;
        for (auto &word : k) word = std::uint32_t(rng());
        if (shared) for (std::size_t j = 0; j + 1 < W; ++j) k[j] = 0xabcdef12u;
        a.push_back(k);
      }
      std::sort(a.begin(), a.end());
      auto *p = bytes.data() + w * stride + (w & 63);
      for (auto const &k : a) for (auto word : k) for (unsigned j = 0; j < 4; ++j)
        *p++ = std::byte((word >> (j * 8)) & 255);
      windows.emplace_back(std::span<std::byte const>(bytes.data() + w * stride + (w & 63), count * W * 4));
    }
    for (unsigned i = 0; i < 8192; ++i) {
      auto w = std::size_t(rng() % window_count);
      auto const &a = keys[w];
      auto q = a[std::size_t(rng() % count)];
      if (i & 1) q.back() = std::uint32_t(rng());
      auto expected = std::size_t(std::lower_bound(a.begin(), a.end(), q) - a.begin());
      requests.push_back({w, q, expected});
    }
  }
};

#if defined(_MSC_VER)
#define EVERETT_BENCH_NOINLINE __declspec(noinline)
#else
#define EVERETT_BENCH_NOINLINE __attribute__((noinline))
#endif
template <std::size_t W, unsigned Method, bool Dependent>
EVERETT_BENCH_NOINLINE std::uint64_t run(fixture<W> const &f, std::size_t iterations) {
  std::uint64_t sum = 0;
  std::size_t last = 0;
  for (std::size_t i = 0; i < iterations; ++i) {
    auto const &r = f.requests[(i + (Dependent ? last * 97 : 0)) & 8191];
    auto const &v = f.windows[r.window];
    if constexpr (Method == 0) last = v.lower_bound_binary(r.query);
    else if constexpr (Method == 1) last = v.lower_bound_simd(r.query);
    else last = v.lower_bound(r.query);
    sum += last;
  }
  return sum;
}

template <std::size_t W, bool Dependent> void measure(fixture<W> const &f,
    std::size_t count, bool shared, unsigned repeats, std::size_t iterations) {
  // Oracle, including the dependency chain, stays outside every timed region.
  std::uint64_t expected = 0;
  std::size_t last = 0;
  for (std::size_t i = 0; i < iterations; ++i) {
    auto const &r = f.requests[(i + (Dependent ? last * 97 : 0)) & 8191];
    last = r.expected; expected += last;
  }
  using function = std::uint64_t (*)(fixture<W> const &, std::size_t);
  function methods[]{run<W, 0, Dependent>, run<W, 1, Dependent>, run<W, 2, Dependent>};
  char const *names[]{"binary", "simd", "automatic"};
  for (auto method : methods) if (method(f, iterations) != expected) std::abort();
  for (unsigned repeat = 0; repeat < repeats; ++repeat) for (unsigned j = 0; j < 3; ++j) {
    auto method = (j + repeat) % 3;
    auto start = clock_type::now();
    auto checksum = methods[method](f, iterations);
    auto ns = std::chrono::duration<double, std::nano>(clock_type::now() - start).count() / double(iterations);
    if (checksum != expected) std::abort();
    std::cout << W * 32 << ',' << count << ',' << (shared ? "shared" : "random") << ','
      << f.windows.size() << ',' << f.bytes.size() << ',' << (Dependent ? "dependent" : "independent") << ','
      << names[method] << ',' << repeat << ',' << iterations << ',' << ns << ',' << checksum << '\n';
  }
}

template <std::size_t W> void suite(unsigned repeats, std::size_t iterations) {
  for (auto count : {3u, 4u, 7u, 8u, 15u, 16u, 31u, 32u, 63u})
    for (bool shared : {false, true}) for (auto windows : {1u, 8192u}) {
      fixture<W> f(count, windows, shared);
      measure<W, false>(f, count, shared, repeats, iterations);
      measure<W, true>(f, count, shared, repeats, iterations);
    }
}
int main(int argc, char **argv) {
  unsigned repeats = argc > 1 ? unsigned(std::stoul(argv[1])) : 3;
  std::size_t iterations = argc > 2 ? std::stoull(argv[2]) : 131072;
  std::cout << "key_bits,count,shape,windows,window_bytes,pattern,method,repeat,iterations,ns_per_lookup,checksum\n";
  suite<1>(repeats, iterations); suite<2>(repeats, iterations); suite<4>(repeats, iterations);
}
