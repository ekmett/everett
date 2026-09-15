/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
// Run with bench/rank_bounds.py; both revisions encode the same logical input.
#include <everett/rank.h>
#include <everett/rank_groups.h>
#include "old_rank.h"
#include "old_rank15.h"
#include "old_rank_groups.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif
namespace {
  std::uint64_t random_word(std::uint64_t & x) {
    x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x;
  }
  using function = std::uint64_t (*)(void const *, std::uint64_t);
  template <class V, bool Pair> [[gnu::noinline]] std::uint64_t query(void const * p, std::uint64_t g) {
    auto const & view = *static_cast<V const *>(p);
    auto lo = view.rank(g);
    if constexpr (Pair) return 2 * lo + view.class_at(g);
    else return lo;
  }
  struct variant { char const * name; void const * view; function run; std::size_t bytes; };
  template <bool Pair, class V> variant variant_of(char const * name, V const & view) {
    return {name, &view, query<V, Pair>, sizeof(V)};
  }
  template <class Oracle> void measure(char const * type, char const * operation,
      std::array<variant, 2> variants, std::uint64_t size, std::size_t bytes,
      unsigned trials, std::size_t queries, Oracle oracle) {
    if (!std::has_single_bit(size)) throw std::runtime_error("power-of-two query domain required");
    std::uint64_t seed = 0x102030405060708;
    std::vector<std::uint64_t> positions(65536);
    for (auto & q : positions) q = random_word(seed);
    for (bool dependent : {false, true}) {
      std::uint64_t previous = 0;
      for (auto q : positions) {
        auto g = (q ^ (dependent ? previous * 0x9e3779b97f4a7c15ull : 0)) & (size - 1);
        previous = oracle(g);
        for (auto const & v : variants) if (v.run(v.view, g) != previous)
          throw std::runtime_error("original population oracle mismatch");
      }
      std::array<std::vector<double>, 2> times;
      std::array<std::uint64_t, 2> checksums{};
      for (unsigned trial = 0; trial < trials; ++trial) for (unsigned slot = 0; slot < 2; ++slot) {
        auto which = (slot + trial) % 2;
        auto const & v = variants[which];
        std::uint64_t previous = 0, checksum = 0;
        auto start = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < queries; ++i) {
          auto g = (positions[i & 65535] ^ (dependent ? previous * 0x9e3779b97f4a7c15ull : 0)) & (size - 1);
          previous = v.run(v.view, g); checksum += previous;
        }
        auto elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
        times[which].push_back(elapsed / double(queries)); checksums[which] = checksum;
      }
      if (checksums[0] != checksums[1]) throw std::runtime_error("timed checksums differ");
      for (unsigned i = 0; i < 2; ++i) {
        auto & t = times[i]; std::sort(t.begin(), t.end());
        std::cout << type << ',' << operation << ',' << (dependent ? "dependent" : "independent") << ','
          << variants[i].name << ',' << size << ',' << bytes << ',' << variants[i].bytes << ','
          << t.front() << ',' << t[t.size()/2] << ',' << t.back() << ',' << checksums[i] << '\n';
      }
      std::cout.flush();
    }
  }
  template <unsigned K> void groups(unsigned trials, std::size_t queries) {
    constexpr std::size_t n = 65536;
    std::uint64_t seed = 0xabcdef123456;
    std::vector<std::uint64_t> populations(n), oracle(n + 1);
    for (std::size_t i = 0; i < n; ++i) { populations[i] = random_word(seed) & K; oracle[i+1] = oracle[i] + populations[i]; }
    auto index = everett::rank_groups<K>::build(populations, n * K);
    auto current = index.view();
    old::rank_groups_view<K> previous(index.classes, index.checkpoints, n * K, oracle.back());
    auto bytes = 8 * (index.classes.size() + index.checkpoints.size());
    auto type = "groups" + std::to_string(K);
    measure(type.c_str(), "rank", {variant_of<false>("cached_total", previous), variant_of<false>("bounded", current)},
      n, bytes, trials, queries, [&](auto g) { return oracle[g]; });
    measure(type.c_str(), "rank_plus_class", {variant_of<true>("cached_total", previous), variant_of<true>("bounded", current)},
      n, bytes, trials, queries, [&](auto g) { return oracle[g] + oracle[g+1]; });
  }
  template <class V> struct bitmap {
    V view; std::span<std::uint64_t const> words;
    auto rank(std::uint64_t p) const { return view.rank(p); }
    auto class_at(std::uint64_t p) const { return (words[p / 64] >> (p % 64)) & 1; }
  };
  void full(unsigned trials, std::size_t queries) {
    constexpr std::size_t n = 262144;
    std::uint64_t seed = 0xabcdef123456;
    std::vector<std::uint64_t> words(n / 64), oracle(n + 1);
    for (auto & word : words) word = random_word(seed);
    for (std::size_t i = 0; i < n; ++i) oracle[i+1] = oracle[i] + ((words[i / 64] >> (i % 64)) & 1);
    auto index = everett::rank_index::build(words, n);
    auto old_index = old::rank_index::build(words, n);
    bitmap current{index.view(), std::span<std::uint64_t const>(index.words)};
    bitmap previous{old_index.view(), std::span<std::uint64_t const>(old_index.words)};
    auto bytes = 8 * (index.words.size() + index.blocks.size() + index.supers.size());
    measure("bitmap", "rank", {variant_of<false>("cached_total", previous), variant_of<false>("bounded", current)},
      n, bytes, trials, queries, [&](auto g) { return oracle[g]; });
  }
}
int main(int argc, char ** argv) {
  try {
#if defined(__APPLE__)
    if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0)) throw std::runtime_error("set QoS");
#endif
    auto trials = argc > 1 ? unsigned(std::stoul(argv[1])) : 5u;
    auto queries = argc > 2 ? std::stoull(argv[2]) : 1048576ull;
    if (!trials || !queries) throw std::invalid_argument("positive trials and queries required");
    std::cout << "type,operation,pattern,variant,positions,array_bytes,view_bytes,min_ns,median_ns,max_ns,checksum\n";
    groups<3>(trials, queries); groups<7>(trials, queries); groups<15>(trials, queries); groups<31>(trials, queries);
    full(trials, queries);
  } catch (std::exception const & e) { std::cerr << e.what() << '\n'; return 1; }
}
/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Compares cached-total rank with bounded rank on identical inputs.
 */
