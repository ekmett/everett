/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/rank.h>
#include <everett/rank_groups.h>
#include "baseline_rank.h"
#include "baseline_rank_groups.h"
#include "neon_rank_groups.h"

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

  template <unsigned K> struct portable_groups {
    everett::rank_groups<K> const & source;
    std::uint64_t class_at(std::uint64_t g) const { return source.view().class_at(g); }
    std::uint64_t rank(std::uint64_t g) const {
      auto groups = source.virtual_count / K + (source.virtual_count % K != 0);
      if (g > groups) throw std::out_of_range("portable group");
      if (g == groups) return source.total;
      auto result = source.checkpoints[g / 128];
      auto count = unsigned(g % 128);
      if (!count) return result;
      constexpr auto bits = everett::rank_groups<K>::class_bits;
      return result + everett::rank_groups_detail::prefix_portable<bits>(source.classes.data() + (g / 128) * (2 * bits), count);
    }
  };

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
      for (unsigned v = 0; v < variants.size(); ++v) {
        auto & ts = times[v]; std::sort(ts.begin(), ts.end());
        std::cout << kind << ',' << size << ',' << (dependent ? "dependent" : "independent") << ','
          << variants[v].name << ',' << positions << ',' << bytes << ',' << qs.size() * 8 << ','
          << query_count << ',' << trials << ',' << std::fixed << std::setprecision(3)
          << ts[ts.size() / 2] << ',' << ts.front() << ',' << ts.back() << ',' << sums[v] << '\n';
      }
      std::cout.flush();
    }
  }

  template <unsigned K> void groups(bool large, std::uint64_t queries, unsigned trials, bool check) {
    auto n = large ? (std::uint64_t{1} << 25) + 37 : 16384ull + 37;
    std::uint64_t seed = 0x11112222;
    std::vector<std::uint64_t> classes(n), oracle(n + 1);
    for (std::uint64_t i = 0; i < n; ++i) { classes[i] = random_word(seed) & K; oracle[i + 1] = oracle[i] + classes[i]; }
    auto index = everett::rank_groups<K>::build(classes, n * K);
    classes.clear(); classes.shrink_to_fit();
    auto candidate = index.view();
    baseline::rank_groups_view<K> old{index.classes, index.checkpoints, index.virtual_count, index.total};
    portable_groups<K> portable{index};
    rank_neon::rank_groups_view<K> neon{index.classes, index.checkpoints, index.virtual_count, index.total};
    std::array variants{make_variant("baseline", old), make_variant("portable_packed", portable), make_variant("neon_packed", neon), make_variant("selected", candidate)};
    auto bytes = 8 * (index.classes.size() + index.checkpoints.size());
    auto kind = "groups" + std::to_string(K);
    measure(kind.c_str(), large ? "large" : "hot", variants, n, bytes, queries, trials, check,
      [&](std::uint64_t g) { return oracle[g]; });
    std::array pairs{make_variant<true>("baseline", old), make_variant<true>("portable_packed", portable), make_variant<true>("neon_packed", neon), make_variant<true>("selected", candidate)};
    kind += "_pair";
    measure(kind.c_str(), large ? "large" : "hot", pairs, n, bytes, queries, trials, check,
      [&](std::uint64_t g) { return oracle[g] + oracle[g + 1]; });
  }

#if defined(__AVX2__)
  inline unsigned builder_avx2(std::uint64_t const * words) {
    auto lookup = _mm256_setr_epi8(0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4,
                                  0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4);
    auto mask = _mm256_set1_epi8(15);
    auto population = [&](auto data) {
      auto low = _mm256_shuffle_epi8(lookup, _mm256_and_si256(data, mask));
      auto high = _mm256_shuffle_epi8(lookup, _mm256_and_si256(_mm256_srli_epi16(data, 4), mask));
      return _mm256_add_epi8(low, high);
    };
    auto a = population(_mm256_loadu_si256(reinterpret_cast<__m256i const *>(words)));
    auto b = population(_mm256_loadu_si256(reinterpret_cast<__m256i const *>(words + 4)));
    auto counts = _mm256_sad_epu8(_mm256_add_epi8(a, b), _mm256_setzero_si256());
    auto pair = _mm_add_epi64(_mm256_castsi256_si128(counts), _mm256_extracti128_si256(counts, 1));
    return unsigned(_mm_cvtsi128_si32(_mm_add_epi64(pair, _mm_srli_si128(pair, 8))));
  }
  template <bool Simd> [[gnu::noinline]] std::uint64_t builder_scan(
      std::span<std::uint64_t const> words, std::span<unsigned> counts) {
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < counts.size(); ++i) {
      if constexpr (Simd) counts[i] = builder_avx2(words.data() + i * 8);
      else counts[i] = baseline::rank_detail::popcount512(words.data() + i * 8);
      total += counts[i];
    }
    return total;
  }
  void builder(bool large, std::uint64_t queries, unsigned trials, bool check) {
    auto runs = large ? 1048576u : 512u;
    std::vector<std::uint64_t> words(runs * 8);
    std::vector<unsigned> counts(runs), oracle(runs);
    std::uint64_t seed = 0x512512;
    for (std::size_t i = 0; i < words.size(); ++i) {
      words[i] = random_word(seed); oracle[i / 8] += std::popcount(words[i]);
    }
    (void)builder_scan<false>(words, counts);
    if (counts != oracle) throw std::runtime_error("scalar builder oracle");
    (void)builder_scan<true>(words, counts);
    if (counts != oracle) throw std::runtime_error("AVX2 builder oracle");
    if (check) return;
    auto repetitions = std::max<std::uint64_t>(1, queries / runs);
    std::array<std::vector<double>, 2> times;
    std::array<std::uint64_t, 2> sums{};
    for (unsigned t = 0; t < trials; ++t) for (unsigned k = 0; k < 2; ++k) {
      auto v = (t + k) % 2; sums[v] = 0;
      auto begin = std::chrono::steady_clock::now();
      if (v) for (std::uint64_t i = 0; i < repetitions; ++i) sums[v] += builder_scan<true>(words, counts);
      else for (std::uint64_t i = 0; i < repetitions; ++i) sums[v] += builder_scan<false>(words, counts);
      auto elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count();
      times[v].push_back(elapsed / (repetitions * runs)); observed ^= sums[v];
    }
    if (sums[0] != sums[1]) throw std::runtime_error("builder checksum mismatch");
    for (unsigned v = 0; v < 2; ++v) {
      auto & ts = times[v]; std::sort(ts.begin(), ts.end());
      std::cout << "builder," << (large ? "large" : "hot") << ",sequential," << (v ? "avx2_lookup" : "baseline")
        << ',' << runs << ',' << words.size() * 8 << ",0," << repetitions * runs << ',' << trials << ','
        << ts[ts.size() / 2] << ',' << ts.front() << ',' << ts.back() << ',' << sums[v] << '\n';
    }
  }
#endif

  void bitmap(bool large, std::uint64_t queries, unsigned trials, bool check) {
    auto bits = large ? (std::uint64_t{1} << 29) + 137 : 253440ull + 137;
    std::uint64_t seed = 0x11112222;
    std::vector<std::uint64_t> words((bits + 63) / 64), oracle(words.size() + 1);
    for (std::uint64_t i = 0; i < words.size(); ++i) { words[i] = random_word(seed); oracle[i + 1] = oracle[i] + std::popcount(words[i]); }
    auto index = everett::rank_index::build(words, bits);
    auto candidate = index.view();
    std::vector<baseline::rank_block> blocks;
    for (auto b : index.blocks) blocks.push_back({b.before, b.runs});
    baseline::rank_view old{index.words, blocks, index.supers, index.bit_count, index.total};
    std::array variants{make_variant("baseline", old), make_variant("selected", candidate)};
    auto bytes = 8 * (index.words.size() + index.blocks.size() + index.supers.size());
    measure("bitmap", large ? "large" : "hot", variants, bits, bytes, queries, trials, check,
      [&](std::uint64_t p) { auto n = p % 64; return oracle[p / 64] + std::popcount(words[p / 64] & ((std::uint64_t{1} << n) - 1)); });
  }
}

int main(int argc, char ** argv) try {
#if defined(__APPLE__)
  if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0)) throw std::runtime_error("benchmark QoS request failed");
#endif
  auto mode = argc > 1 ? std::string(argv[1]) : "hot";
  auto trials = argc > 2 ? unsigned(std::stoul(argv[2])) : 5;
  auto queries = argc > 3 ? std::stoull(argv[3]) : 1048576;
  if (!trials || !queries) throw std::invalid_argument("positive counts required");
  bool check = mode == "check";
  if (!check && mode != "large" && mode != "hot" && mode != "all") throw std::invalid_argument("unknown mode");
  std::cout << "kind,size,pattern,variant,positions,encoded_bytes,query_bytes,queries,trials,median_ns,min_ns,max_ns,checksum\n";
  for (bool large : mode == "all" ? std::vector<bool>{false, true} : std::vector<bool>{mode == "large"}) {
    groups<3>(large, queries, trials, check);
    groups<7>(large, queries, trials, check);
    groups<31>(large, queries, trials, check);
    bitmap(large, queries, trials, check);
#if defined(__AVX2__)
    builder(large, queries, trials, check);
#endif
  }
  std::cerr << "verified; observed=" << observed << '\n';
} catch (std::exception const & e) { std::cerr << e.what() << '\n'; return 1; }

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Measures packed group and full bitmap rank against a pinned baseline.
 */
