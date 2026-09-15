/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Measures complete bitmap rank before and after stored spacer lanes.
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

// Derived from other_rank.cc's hot bitmap workload; use rank_spacers.py.
#include <diet/rank.h>
#include "baseline_rank.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace {
  std::uint64_t observed = 0;
  std::uint64_t random_word(std::uint64_t & state) {
    state += 0x9e3779b97f4a7c15ull;
    auto x = state;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
  }
  std::uint64_t reduce(std::uint64_t x, std::uint64_t n) {
    return std::uint64_t((static_cast<unsigned __int128>(x) * n) >> 64);
  }
  template <class V, bool Pair = false> [[gnu::noinline]] std::uint64_t query(void const * p, std::uint64_t g) {
    auto const & view = *static_cast<V const *>(p);
    auto lo = view.rank(g);
    if constexpr (Pair) return 2 * lo + view.class_at(g);
    else return lo;
  }
  struct variant {
    char const * name;
    void const * view;
    std::uint64_t (*run)(void const *, std::uint64_t);
  };
  template <bool Pair = false, class V> variant make_variant(char const * name, V const & v) { return {name, &v, query<V, Pair>}; }

  template <class Expected> void measure(char const * kind, char const * size,
      std::span<variant const> variants, std::uint64_t positions, std::uint64_t bytes,
      std::uint64_t query_count, unsigned trials, bool check, Expected expected) {
    std::uint64_t seed = 0x123456abcdef;
    auto n = std::bit_floor(std::min<std::uint64_t>(query_count, positions < 1048576 ? 65536 : query_count));
    for (bool dependent : {false, true}) {
      std::vector<std::uint64_t> qs(n);
      for (auto & q : qs) { auto x = random_word(seed); q = dependent ? x : reduce(x, positions); }
      std::uint64_t previous = 0;
      for (std::uint64_t i = 0; i < query_count; ++i) {
        auto q = qs[i & (n - 1)];
        auto g = dependent ? reduce(q ^ (previous * 0x9e3779b97f4a7c15ull), positions) : q;
        previous = expected(g);
        for (auto const & v : variants)
          if (v.run(v.view, g) != previous) throw std::runtime_error("independent prefix oracle mismatch");
      }
      if (check) continue;
      auto run = [&](variant const & v, std::uint64_t count, std::uint64_t & sum) {
        std::uint64_t previous = 0; sum = 0;
        auto begin = std::chrono::steady_clock::now();
        if (dependent) {
          for (std::uint64_t i = 0; i < count; ++i) {
            auto g = reduce(qs[i & (n - 1)] ^ (previous * 0x9e3779b97f4a7c15ull), positions);
            previous = v.run(v.view, g); sum += previous;
          }
        } else {
          for (std::uint64_t i = 0; i < count; ++i) sum += v.run(v.view, qs[i & (n - 1)]);
        }
        auto elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count();
        observed ^= sum; return elapsed / count;
      };
      std::vector<std::vector<double>> times(variants.size());
      std::vector<std::uint64_t> sums(variants.size());
      for (auto const & v : variants) { std::uint64_t sum; run(v, std::min<std::uint64_t>(32768, query_count), sum); }
      for (unsigned t = 0; t < trials; ++t) {
        for (unsigned j = 0; j < variants.size(); ++j) {
          auto v = (t + j) % variants.size();
          times[v].push_back(run(variants[v], query_count, sums[v]));
        }
        for (auto sum : sums) if (sum != sums.front()) throw std::runtime_error("timed checksum mismatch");
      }
      for (unsigned v = 0; v < variants.size(); ++v)
        for (unsigned trial = 0; trial < trials; ++trial)
          std::cout << kind << ',' << size << ',' << (dependent ? "dependent" : "independent") << ','
            << variants[v].name << ',' << positions << ',' << bytes << ',' << qs.size() * 8 << ','
            << query_count << ',' << trial << ',' << std::fixed << std::setprecision(3)
            << times[v][trial] << ',' << sums[v] << '\n';
      std::cout.flush();
    }
  }

  void bitmap(std::uint64_t queries, unsigned trials, bool check) {
    constexpr std::uint64_t bits = 253440 + 137;
    std::uint64_t seed = 0x11112222;
    std::vector<std::uint64_t> words((bits + 63) / 64), oracle(bits + 1);
    for (auto & word : words) word = random_word(seed);
    for (std::uint64_t i = 0; i < bits; ++i)
      oracle[i + 1] = oracle[i] + ((words[i / 64] >> (i % 64)) & 1);
    // Each revision builds its own directory: the packed run positions differ.
    auto index = diet::rank_index::build(words, bits);
    auto old_index = baseline::rank_index::build(words, bits);
    auto candidate = index.view();
    auto old = old_index.view();
    if (candidate.count() != oracle.back() || old.count() != oracle.back())
      throw std::runtime_error("independent total oracle mismatch");
    for (std::uint64_t i = 0; i < bits; ++i)
      if (candidate.rank(i) != oracle[i] || old.rank(i) != oracle[i])
        throw std::runtime_error("every-position prefix oracle mismatch");
    auto bytes = [](auto const & value) {
      return value.words.size() * sizeof(std::uint64_t) +
        value.blocks.size() * sizeof(typename decltype(value.blocks)::value_type) +
        value.supers.size() * sizeof(std::uint64_t);
    };
    if (bytes(index) != bytes(old_index)) throw std::runtime_error("directory size changed");
    std::array variants{make_variant("baseline", old), make_variant("candidate", candidate)};
    measure("bitmap", "hot", variants, bits, bytes(index), queries, trials, check,
      [&](std::uint64_t p) { return oracle[p]; });
  }
}

int main(int argc, char ** argv) try {
#if defined(__APPLE__)
  if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0))
    throw std::runtime_error("benchmark QoS request failed");
#endif
  auto trials = argc > 1 ? unsigned(std::stoul(argv[1])) : 5;
  auto queries = argc > 2 ? std::stoull(argv[2]) : 1048576;
  auto check = argc > 3 && std::string(argv[3]) == "check";
  if (!trials || !queries) throw std::invalid_argument("positive counts required");
  std::cout << "kind,size,pattern,variant,positions,encoded_bytes,query_bytes,queries,trial,ns_per_query,checksum\n";
  bitmap(queries, trials, check);
  std::cerr << "verified; observed=" << observed << '\n';
} catch (std::exception const & e) { std::cerr << e.what() << '\n'; return 1; }
