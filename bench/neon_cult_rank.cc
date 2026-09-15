/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Compares NEON rank15 reductions with an externally supplied Cult bitmap rank.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

// The runner supplies a pinned Diet header and an unchanged external Cult
// header. No Cult source is copied into this repository or uploaded anywhere.
#include <diet/rank15.h>
#define diet diet_neon_qword
#include DIET_NEON_QWORD_HEADER
#undef diet
#include DIET_EXTERNAL_CULT_RANK_HEADER

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

#if !defined(__aarch64__) || !defined(__ARM_NEON)
#error This comparison requires an AArch64 NEON host.
#endif

namespace {
  std::uint64_t observed = 0;
  std::uint64_t random_word(std::uint64_t & state) {
    state += 0x9e3779b97f4a7c15ull;
    auto value = state;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
  }
  std::uint64_t reduce(std::uint64_t value, std::uint64_t count) {
    return std::uint64_t((static_cast<unsigned __int128>(value) * count) >> 64);
  }
  unsigned population(std::span<std::uint64_t const> words, std::uint64_t first, unsigned count) {
    auto value = words[first / 64] >> (first % 64);
    if (first % 64 + count > 64) value |= words[first / 64 + 1] << (64 - first % 64);
    return unsigned(std::popcount(value & ((std::uint64_t{1} << count) - 1)));
  }

  template <unsigned N> struct cult_adapter {
    cult::sim::rank_view<N> view;
    std::uint64_t rank(std::uint64_t group) const { return view.rank(unsigned(group * 15)); }
    unsigned class_at(std::uint64_t group) const {
      auto first = unsigned(group * 15), word = first / 32, shift = first % 32;
      auto value = view.data->raw[word] >> shift;
      if (shift + 15 > 32) value |= view.data->raw[word + 1] << (32 - shift);
      return std::popcount(value & 32767u);
    }
  };

  using function = std::uint64_t (*)(void const *, std::uint64_t);
  template <class V, unsigned Mode> [[gnu::noinline]] std::uint64_t query(void const * pointer, std::uint64_t group) {
    auto const & view = *static_cast<V const *>(pointer);
    auto lo = view.rank(group);
    if constexpr (Mode == 0) return lo;
    else if constexpr (Mode == 1) return 2 * lo + view.class_at(group);
    else return lo + view.rank(group + 1);
  }
  struct variant {
    char const * name;
    void const * pointer;
    std::array<function, 3> functions;
    std::uint64_t data_bytes, metadata_bytes, endpoint_bytes;
    unsigned data_mod64, metadata_mod64;
  };
  template <class V> variant make_variant(char const * name, V const & view,
      std::uint64_t data, std::uint64_t metadata, std::uint64_t endpoint, void const * dp, void const * mp) {
    return {name, &view, {query<V, 0>, query<V, 1>, query<V, 2>}, data, metadata, endpoint,
      unsigned(reinterpret_cast<std::uintptr_t>(dp) % 64), unsigned(reinterpret_cast<std::uintptr_t>(mp) % 64)};
  }
  double measure(variant const & v, unsigned mode, bool dependent,
      std::span<std::uint64_t const> queries, std::uint64_t groups,
      std::uint64_t count, std::uint64_t & checksum) {
    auto run = v.functions[mode];
    auto begin = std::chrono::steady_clock::now();
    std::uint64_t sum = 0, previous = 0;
    if (dependent) {
      for (std::uint64_t i = 0; i != count; ++i) {
        auto group = reduce(queries[i & (queries.size() - 1)] ^ (previous * 0x9e3779b97f4a7c15ull), groups);
        previous = run(v.pointer, group);
        sum += previous;
      }
    } else {
      for (std::uint64_t i = 0; i != count; ++i) sum += run(v.pointer, queries[i & (queries.size() - 1)]);
    }
    auto end = std::chrono::steady_clock::now();
    observed ^= sum; checksum = sum;
    return std::chrono::duration<double, std::nano>(end - begin).count() / double(count);
  }

  template <unsigned N> void exercise(char const * name, unsigned trials, std::uint64_t count, bool check_only) {
    static_assert(N % 15 == 0 && N % 64 == 0);
    constexpr std::uint64_t groups = N / 15;
    std::cerr << "building " << name << ", " << N << " logical bits\n";
    std::vector<std::uint64_t> words(N / 64);
    std::uint64_t seed = 0x123456789abcdefull;
    for (auto & word : words) word = random_word(seed);
    std::vector<std::uint32_t> oracle(words.size() + 1);
    for (std::size_t i = 0; i != words.size(); ++i) oracle[i + 1] = oracle[i] + unsigned(std::popcount(words[i]));
    auto expected = [&](std::uint64_t group, unsigned mode) {
      auto at = group * 15;
      std::uint64_t result = oracle[at / 64];
      if (at % 64) result += unsigned(std::popcount(words[at / 64] & ((std::uint64_t{1} << (at % 64)) - 1)));
      return mode ? 2 * result + population(words, at, 15) : result;
    };
    auto cult = std::unique_ptr<cult::sim::rank_index<N>>(new cult::sim::rank_index<N>);
    for (std::size_t i = 0; i != words.size(); ++i) {
      cult->raw[2 * i] = unsigned(words[i]); cult->raw[2 * i + 1] = unsigned(words[i] >> 32);
    }
    cult::sim::build_rank(*cult);
    std::vector<std::uint8_t> classes(groups);
    for (std::uint64_t g = 0; g != groups; ++g) classes[g] = std::uint8_t(population(words, g * 15, 15));
    auto packed = diet::rank15_index::build(classes, N);
    classes.clear(); classes.shrink_to_fit();
    packed.classes.shrink_to_fit(); packed.checkpoints.shrink_to_fit();
    auto byte_first = packed.view();
    diet_neon_qword::rank15_view qword_first{packed.classes, packed.checkpoints, N, packed.total};
    cult_adapter<N> cult_view{{cult.get()}};
    auto data_bytes = packed.classes.size() * 8, metadata_bytes = packed.checkpoints.size() * 8;
    static_assert(sizeof(*cult) == sizeof(cult->raw) + sizeof(cult->directory) + sizeof(cult->total));
    std::array variants{
      make_variant("neon_byte_first", byte_first, data_bytes, metadata_bytes, 8, packed.classes.data(), packed.checkpoints.data()),
      make_variant("neon_qword_first", qword_first, data_bytes, metadata_bytes, 8, packed.classes.data(), packed.checkpoints.data()),
      make_variant("cult_bitmap_rank", cult_view, sizeof(cult->raw), sizeof(cult->directory), sizeof(cult->total), cult->raw, cult->directory)
    };
    for (auto const & v : variants) {
      if (v.functions[0](v.pointer, groups) != oracle.back()) throw std::runtime_error("terminal rank mismatch");
      for (unsigned mode = 0; mode != 3; ++mode) {
        for (std::uint64_t g = 0; g != std::min<std::uint64_t>(groups, 8192); ++g)
          if (v.functions[mode](v.pointer, g) != expected(g, mode)) throw std::runtime_error("prefix oracle mismatch");
        for (auto g : {groups - 1, groups / 2})
          if (v.functions[mode](v.pointer, g) != expected(g, mode)) throw std::runtime_error("endpoint oracle mismatch");
      }
    }
    auto query_count = std::bit_floor(std::min<std::uint64_t>(count, std::max<std::uint64_t>(groups, 1024)));
    for (bool dependent : {false, true}) {
      std::vector<std::uint64_t> queries(query_count);
      for (auto & q : queries) { auto r = random_word(seed); q = dependent ? r : reduce(r, groups); }
      for (unsigned mode = 0; mode != 3; ++mode) {
        // Validate every actual query and the complete answer-dependent chain.
        std::array<std::uint64_t, 3> previous{};
        std::uint64_t reference = 0;
        for (std::uint64_t i = 0; i != (dependent ? count : query_count); ++i) {
          auto q = queries[i & (queries.size() - 1)];
          auto g = dependent ? reduce(q ^ (reference * 0x9e3779b97f4a7c15ull), groups) : q;
          reference = expected(g, mode);
          for (std::size_t v = 0; v != variants.size(); ++v) {
            auto actual_g = dependent ? reduce(q ^ (previous[v] * 0x9e3779b97f4a7c15ull), groups) : g;
            previous[v] = variants[v].functions[mode](variants[v].pointer, actual_g);
            if (previous[v] != reference) throw std::runtime_error("query oracle mismatch");
          }
        }
        if (check_only) continue;
        std::array<std::vector<double>, 3> samples;
        std::array<std::uint64_t, 3> sums{};
        for (auto const & v : variants) { std::uint64_t warmup; (void)measure(v, mode, dependent, queries, groups, std::min<std::uint64_t>(count, 32768), warmup); }
        for (unsigned trial = 0; trial != trials; ++trial) {
          for (std::size_t j = 0; j != variants.size(); ++j) {
            auto v = (j + trial) % variants.size();
            samples[v].push_back(measure(variants[v], mode, dependent, queries, groups, count, sums[v]));
          }
          if (sums[0] != sums[1] || sums[0] != sums[2]) throw std::runtime_error("timed checksum mismatch");
        }
        constexpr char const * modes[] = {"rank", "pair_rank_plus_class", "pair_two_ranks"};
        for (std::size_t v = 0; v != variants.size(); ++v) {
          auto & times = samples[v]; std::sort(times.begin(), times.end());
          auto const & item = variants[v];
          std::cout << name << ',' << N << ',' << groups << ',' << (dependent ? "dependent" : "independent") << ',' << modes[mode] << ',' << item.name << ','
            << item.data_bytes << ',' << item.metadata_bytes << ',' << item.endpoint_bytes << ',' << item.data_bytes + item.metadata_bytes + item.endpoint_bytes << ',' << item.data_mod64 << ',' << item.metadata_mod64 << ','
            << queries.size() * 8 << ',' << count << ',' << trials << ',' << std::fixed << std::setprecision(3)
            << times[times.size() / 2] << ',' << times.front() << ',' << times.back() << ',' << sums[v] << '\n';
        }
        std::cout.flush();
      }
    }
    std::cerr << "verified " << name << '\n';
  }
}

int main(int argc, char ** argv) try {
#if defined(__APPLE__)
  if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0)) throw std::runtime_error("benchmark QoS request failed");
#endif
  auto selected = argc > 1 ? std::string(argv[1]) : "hot";
  auto trials = argc > 2 ? unsigned(std::stoul(argv[2])) : 5u;
  auto queries = argc > 3 ? std::stoull(argv[3]) : 1048576ull;
  if (!trials || !queries) throw std::invalid_argument("positive trials and query count required");
  bool check_only = selected == "check";
  std::cout << "case,logical_bits,groups,pattern,operation,variant,data_bytes,metadata_bytes,endpoint_bytes,total_bytes,data_mod64,metadata_mod64,query_bytes,queries,trials,median_ns,min_ns,max_ns,checksum\n";
  if (selected == "hot" || selected == "all" || check_only) exercise<253440>("hot_same_bits", trials, queries, check_only);
  if (selected == "large" || selected == "all") exercise<780902400>("large_same_bits", trials, queries, false);
  if (selected != "hot" && selected != "large" && selected != "all" && !check_only) throw std::invalid_argument("unknown case");
  std::cerr << "observed=" << observed << '\n';
} catch (std::exception const & e) { std::cerr << e.what() << '\n'; return 1; }
