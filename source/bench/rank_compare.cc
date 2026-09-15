/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

// Use: sh bench/rank_compare.sh all 5 1048576 core (requires Git history).
// The runner pins historical headers; compiling against current include/ would
// silently change the baselines. Optional candidates use distinct namespaces;
// encoded bytes
// are shared, so every backend answers the same g*15 boundary on the same bits.
#include <everett/rank.h>
#include <everett/rank15.h>
#include <everett/rank_groups.h>
#ifdef EVERETT_RANK_CANDIDATE
#define everett everett_candidate
#include EVERETT_RANK_CANDIDATE
#undef everett
#endif
#ifdef EVERETT_RANK_SIMD
#define everett everett_simd
#include EVERETT_RANK_SIMD
#undef everett
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#if defined(__APPLE__)
#include <pthread/qos.h>
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
  unsigned range_population(std::span<std::uint64_t const> words, std::uint64_t first, unsigned count) {
    auto value = words[first / 64] >> (first % 64);
    if (first % 64 + count > 64) value |= words[first / 64 + 1] << (64 - first % 64);
    return unsigned(std::popcount(value & ((std::uint64_t{1} << count) - 1)));
  }

  struct bitmap {
    everett::rank_view view;
    std::span<std::uint64_t const> words;
    std::span<everett::rank_block const> blocks;
    std::uint64_t groups;
    std::uint64_t rank(std::uint64_t group) const { return view.rank(group * 15); }
    unsigned class_at(std::uint64_t group) const { return range_population(words, group * 15, 15); }
  };

  // CPU translation of the Poppy 2048/512 layout in ekmett/vr shaders/poppy.glsl.
  // Uses all four 128-bit vectors of the selected 512-bit run. Instead of the
  // shader's lane prefix scan, it masks lanes at the query then reduces NEON
  // byte populations. This is CPU resident-memory timing, not GPU timing.
  // The original unparenthesized packed-count expression is grouped explicitly.
  struct poppy512 : bitmap {
    std::uint64_t rank(std::uint64_t group) const {
      if (group == groups) return view.count();
      auto position = group * 15;
      auto block = blocks[position / 2048];
      auto run = unsigned((position / 512) % 4);
      auto packed = block.runs & ((std::uint32_t{1} << (10 * run)) - 1);
      auto before = std::uint64_t(block.before) + (packed & 1023u) +
        ((packed >> 10) & 1023u) + ((packed >> 20) & 1023u);
#if defined(__aarch64__) && defined(__ARM_NEON)
      auto data = reinterpret_cast<std::uint32_t const *>(words.data() + (position / 512) * 8);
      auto lane = vdupq_n_u32(unsigned(position % 512) / 32);
      auto tail = vdupq_n_u32((std::uint32_t{1} << (position % 32)) - 1);
      std::uint32_t const offsets[4] = {0, 1, 2, 3};
      auto index = vld1q_u32(offsets);
      auto mask0 = vorrq_u32(vcltq_u32(index, lane), vandq_u32(vceqq_u32(index, lane), tail));
      index = vaddq_u32(index, vdupq_n_u32(4));
      auto mask1 = vorrq_u32(vcltq_u32(index, lane), vandq_u32(vceqq_u32(index, lane), tail));
      index = vaddq_u32(index, vdupq_n_u32(4));
      auto mask2 = vorrq_u32(vcltq_u32(index, lane), vandq_u32(vceqq_u32(index, lane), tail));
      index = vaddq_u32(index, vdupq_n_u32(4));
      auto mask3 = vorrq_u32(vcltq_u32(index, lane), vandq_u32(vceqq_u32(index, lane), tail));
      auto a = vcntq_u8(vreinterpretq_u8_u32(vandq_u32(vld1q_u32(data), mask0)));
      auto b = vcntq_u8(vreinterpretq_u8_u32(vandq_u32(vld1q_u32(data + 4), mask1)));
      auto c = vcntq_u8(vreinterpretq_u8_u32(vandq_u32(vld1q_u32(data + 8), mask2)));
      auto d = vcntq_u8(vreinterpretq_u8_u32(vandq_u32(vld1q_u32(data + 12), mask3)));
      return before + vaddlvq_u8(vaddq_u8(vaddq_u8(a, b), vaddq_u8(c, d)));
#else
      return bitmap::rank(group);
#endif
    }
  };

  using function = std::uint64_t (*)(void const *, std::uint64_t);
  template <class V, unsigned Mode> [[gnu::noinline]] std::uint64_t query(void const * pointer, std::uint64_t group) {
    auto const & view = *static_cast<V const *>(pointer);
    auto lo = view.rank(group);
    if constexpr (Mode == 0) return lo;
    else if constexpr (Mode == 1) return lo + view.rank(group + 1);
    else return 2 * lo + view.class_at(group);
  }
  struct variant {
    std::string name;
    void const * pointer;
    std::array<function, 3> functions;
    std::uint64_t data_bytes, metadata_bytes, capacity_bytes;
    unsigned data_mod64, metadata_mod64;
  };
  template <class V> variant make_variant(char const * name, V const & view,
      std::uint64_t data, std::uint64_t metadata, std::uint64_t capacity) {
    return {name, &view, {query<V, 0>, query<V, 1>, query<V, 2>}, data, metadata, capacity, 0, 0};
  }
  struct pattern {
    std::string name;
    std::vector<std::uint64_t> queries;
    bool sequential = false;
    bool dependent = false;
  };
  double measure(variant const & implementation, unsigned mode, pattern const & input,
      std::uint64_t groups, std::uint64_t count, std::uint64_t & checksum) {
    auto run = implementation.functions[mode];
    auto begin = std::chrono::steady_clock::now();
    std::uint64_t sum = 0, previous = 0;
    if (input.sequential) {
      std::uint64_t group = 0;
      for (std::uint64_t i = 0; i != count; ++i) {
        sum += run(implementation.pointer, group);
        if (++group == groups) group = 0;
      }
    } else if (input.dependent) {
      for (std::uint64_t i = 0; i != count; ++i) {
        auto group = reduce(input.queries[i & (input.queries.size() - 1)] ^ (previous * 0x9e3779b97f4a7c15ull), groups);
        previous = run(implementation.pointer, group);
        sum += previous;
      }
    } else {
      for (std::uint64_t i = 0; i != count; ++i)
        sum += run(implementation.pointer, input.queries[i & (input.queries.size() - 1)]);
    }
    auto end = std::chrono::steady_clock::now();
    observed ^= sum;
    checksum = sum;
    return std::chrono::duration<double, std::nano>(end - begin).count() / double(count);
  }

  void exercise(std::string const & case_name, std::uint64_t groups, unsigned trials,
      std::uint64_t requested_queries, bool check_only, bool core_only) {
    auto bits = groups * 15;
    if (bits >= (std::uint64_t{1} << 32)) throw std::invalid_argument("Poppy comparison requires less than 2^32 bits");
    std::cerr << "building " << case_name << ", " << groups << " groups\n";
    std::vector<std::uint64_t> source((bits + 63) / 64);
    std::uint64_t seed = 0x123456789abcdef;
    for (auto & word : source) word = random_word(seed);
    auto full = everett::rank_index::build(source, bits);
    source.clear(); source.shrink_to_fit();
    auto logical_words = full.words.size();
    // All four vector loads are readable even in the final partial run.
    full.words.resize((logical_words + 7) / 8 * 8, 0);
    full.words.shrink_to_fit(); full.blocks.shrink_to_fit(); full.supers.shrink_to_fit();
    std::vector<std::uint8_t> counts(groups);
    for (std::uint64_t g = 0; g != groups; ++g) counts[g] = std::uint8_t(range_population(full.words, g * 15, 15));
    auto packed = everett::rank15_index::build(counts, bits);
    counts.clear(); counts.shrink_to_fit();
    packed.classes.shrink_to_fit(); packed.checkpoints.shrink_to_fit();
    bitmap raw{{std::span(full.words).first(logical_words), full.blocks, full.supers, bits, full.total}, full.words, full.blocks, groups};
    poppy512 poppy{raw};
    auto rank15 = packed.view();
    everett::rank_groups_view<15> typed{packed.classes, packed.checkpoints, bits, packed.total};
    auto raw_data = full.words.size() * 8, raw_metadata = full.blocks.size() * 8 + full.supers.size() * 8;
    auto packed_data = packed.classes.size() * 8, packed_metadata = packed.checkpoints.size() * 8;
    std::vector<variant> variants{
      make_variant("bitmap_rank", raw, raw_data, raw_metadata, full.words.capacity() * 8 + full.blocks.capacity() * 8 + full.supers.capacity() * 8),
#if defined(__aarch64__) && defined(__ARM_NEON)
      make_variant("poppy512_neon_cpu", poppy, raw_data, raw_metadata, full.words.capacity() * 8 + full.blocks.capacity() * 8 + full.supers.capacity() * 8),
#endif
      make_variant("rank15_baseline", rank15, packed_data, packed_metadata, packed.classes.capacity() * 8 + packed.checkpoints.capacity() * 8),
      make_variant("rank_groups15_baseline", typed, packed_data, packed_metadata, packed.classes.capacity() * 8 + packed.checkpoints.capacity() * 8)
    };
#ifdef EVERETT_RANK_CANDIDATE
    everett_candidate::rank15_view candidate{packed.classes, packed.checkpoints, bits, packed.total};
    variants.push_back(make_variant("rank15_candidate", candidate, packed_data, packed_metadata, packed.classes.capacity() * 8 + packed.checkpoints.capacity() * 8));
#endif
#ifdef EVERETT_RANK_SIMD
    everett_simd::rank15_view simd{packed.classes, packed.checkpoints, bits, packed.total};
    variants.push_back(make_variant("rank15_simd", simd, packed_data, packed_metadata, packed.classes.capacity() * 8 + packed.checkpoints.capacity() * 8));
#endif
    for (auto & variant : variants) {
      bool is_bitmap = variant.name == "bitmap_rank" || variant.name == "poppy512_neon_cpu";
      variant.data_mod64 = unsigned(reinterpret_cast<std::uintptr_t>(is_bitmap ? full.words.data() : packed.classes.data()) % 64);
      variant.metadata_mod64 = unsigned(reinterpret_cast<std::uintptr_t>(is_bitmap ? static_cast<void const *>(full.blocks.data()) : packed.checkpoints.data()) % 64);
    }
    // Independent oracle: one prefix per raw 64-bit word, never packed classes.
    std::vector<std::uint32_t> oracle(logical_words + 1);
    for (std::size_t i = 0; i != logical_words; ++i) oracle[i + 1] = oracle[i] + unsigned(std::popcount(full.words[i]));
    auto expected = [&](std::uint64_t g, unsigned mode) {
      auto at = g * 15;
      auto lo = std::uint64_t(oracle[at / 64]);
      if (at % 64) lo += unsigned(std::popcount(full.words[at / 64] & ((std::uint64_t{1} << (at % 64)) - 1)));
      return mode ? 2 * lo + range_population(full.words, at, 15) : lo;
    };
    for (auto const & variant : variants) for (unsigned mode = 0; mode != 3; ++mode) {
      for (std::uint64_t g = 0; g != std::min<std::uint64_t>(groups, 8192); ++g)
        if (variant.functions[mode](variant.pointer, g) != expected(g, mode)) throw std::runtime_error("prefix oracle mismatch");
      for (auto g : {groups - 1, groups / 2})
        if (variant.functions[mode](variant.pointer, g) != expected(g, mode)) throw std::runtime_error("endpoint oracle mismatch");
    }
    std::vector<pattern> patterns;
    auto queries = std::bit_floor(std::min<std::uint64_t>(requested_queries, std::max<std::uint64_t>(groups, 1024)));
    auto append_pattern = [&](std::string name, unsigned modulus, unsigned offset, bool dependent = false) {
      pattern p{std::move(name), {}, false, dependent};
      p.queries.resize(queries);
      for (auto & g : p.queries) {
        auto value = random_word(seed);
        g = dependent ? value : modulus ? reduce(value, (groups - offset + modulus - 1) / modulus) * modulus + offset : reduce(value, groups);
      }
      patterns.push_back(std::move(p));
    };
    append_pattern("random", 0, 0);
    append_pattern("random_dependent", 0, 0, true);
    if (!core_only) patterns.push_back({"sequential_full_sweep", {}, true, false});
    for (unsigned offset : {0u, 16u, 64u, 127u}) if (!core_only && groups > offset)
      append_pattern("group_mod128_" + std::to_string(offset), 128, offset);
    for (unsigned offset : {0u, 255u, 511u}) if (!core_only && groups > 512)
      append_pattern("bit_mod512_" + std::to_string(offset), 512, (239 * offset) % 512);
    for (auto const & input : patterns) {
      auto modes = input.name == "random" ? 3u : 1u;
      auto count = input.sequential ? std::max<std::uint64_t>(groups, requested_queries) : std::max<std::uint64_t>(queries, requested_queries);
      for (unsigned mode = 0; mode != modes; ++mode) {
        // Validate every actual random/offset query, including dependent chains.
        if (!input.sequential) {
          std::vector<std::uint64_t> last(variants.size());
          std::uint64_t reference_last = 0;
          for (std::uint64_t i = 0; i != (input.dependent ? count : queries); ++i) {
            auto item = input.queries[i & (input.queries.size() - 1)];
            auto g = input.dependent ? reduce(item ^ (reference_last * 0x9e3779b97f4a7c15ull), groups) : item;
            reference_last = expected(g, mode);
            for (std::size_t v = 0; v != variants.size(); ++v) {
              auto actual_g = input.dependent ? reduce(item ^ (last[v] * 0x9e3779b97f4a7c15ull), groups) : g;
              last[v] = variants[v].functions[mode](variants[v].pointer, actual_g);
              if (last[v] != reference_last) throw std::runtime_error("query oracle mismatch");
            }
          }
        }
        if (check_only) continue;
        std::vector<std::vector<double>> samples(variants.size());
        std::vector<std::uint64_t> sums(variants.size());
        for (unsigned trial = 0; trial != trials; ++trial) {
          for (std::size_t j = 0; j != variants.size(); ++j) {
            auto v = (j + trial) % variants.size();
            samples[v].push_back(measure(variants[v], mode, input, groups, count, sums[v]));
          }
          if (!std::all_of(sums.begin(), sums.end(), [&](auto sum) { return sum == sums.front(); }))
            throw std::runtime_error("timed checksum mismatch");
        }
        constexpr char const * mode_names[] = {"rank", "pair_two_ranks", "pair_rank_plus_class"};
        for (std::size_t v = 0; v != variants.size(); ++v) {
          auto & times = samples[v]; std::sort(times.begin(), times.end());
          auto const & variant = variants[v];
          std::cout << case_name << ',' << bits << ',' << groups << ',' << input.name << ',' << mode_names[mode] << ',' << variant.name << ','
            << variant.data_bytes << ',' << variant.metadata_bytes << ',' << variant.data_bytes + variant.metadata_bytes << ',' << variant.capacity_bytes << ',' << variant.data_mod64 << ',' << variant.metadata_mod64 << ','
            << input.queries.size() * 8 << ',' << count << ',' << trials << ',' << std::fixed << std::setprecision(3)
            << times[times.size() / 2] << ',' << times.front() << ',' << times.back() << ',' << sums[v] << '\n';
        }
        std::cout.flush();
      }
    }
    std::cerr << "verified " << case_name << '\n';
  }
}

int main(int argc, char ** argv) try {
#if defined(__APPLE__)
  if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0))
    throw std::runtime_error("could not set benchmark user-initiated QoS");
#endif
  auto selected = argc > 1 ? std::string(argv[1]) : "all";
  auto trials = argc > 2 ? unsigned(std::stoul(argv[2])) : 3u;
  auto queries = argc > 3 ? std::stoull(argv[3]) : 1048576ull;
  if (!trials || !queries) throw std::invalid_argument("trials and queries must be positive");
  bool check_only = selected == "check";
  bool core_only = argc > 4 && std::string(argv[4]) == "core";
  std::cout << "case,logical_bits,groups,pattern,operation,variant,data_bytes,metadata_bytes,total_bytes,capacity_bytes,data_mod64,metadata_mod64,query_bytes,queries,trials,median_ns,min_ns,max_ns,checksum\n";
  // Equal-footprint pairs give different logical universes; same-case rows
  // always use exactly the same universe and positions. Target rounding is
  // visible in exact reported byte counts. Both large_packed representations
  // exceed the M2 Max's exposed 16 MiB CPU cache; construction and I/O are untimed.
  std::array<std::pair<char const *, std::uint64_t>, 4> cases{{
    {"hot_full32KiB", (32768ull * 2048 / (264 * 15) / 128) * 128},
    {"hot_packed32KiB", (32768ull * 16 / 9 / 128) * 128},
    {"large_full96MiB", ((96ull << 20) * 2048 / (264 * 15) / 128) * 128},
    {"large_packed96MiB", ((96ull << 20) * 16 / 9 / 128) * 128}
  }};
  for (auto [name, groups] : cases)
    if (selected == "all" || selected == name || (check_only && std::string(name).starts_with("hot")))
      exercise(name, groups, trials, queries, check_only, core_only);
  std::cerr << "observed=" << observed << '\n';
} catch (std::exception const & error) {
  std::cerr << error.what() << '\n';
  return 1;
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Compares full bit-vector and packed population rank on shared queries.
 */
