/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Benchmarks Everett's packed rank15 summation and lookup.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/rank15.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <string_view>
#include <vector>

// Compile with -O3 -DNDEBUG -std=c++20 -Iinclude. To compare complete lookups
// across revisions, compile this same source against each revision's headers.
namespace {
  [[gnu::noinline]] unsigned sum_widen(std::uint64_t value) {
    value = (value & 0x0f0f0f0f0f0f0f0full) + ((value >> 4) & 0x0f0f0f0f0f0f0f0full);
    value = (value & 0x00ff00ff00ff00ffull) + ((value >> 8) & 0x00ff00ff00ff00ffull);
    value = (value & 0x0000ffff0000ffffull) + ((value >> 16) & 0x0000ffff0000ffffull);
    return unsigned((value & 0xffffffffu) + (value >> 32));
  }

  [[gnu::noinline]] unsigned sum_multiply(std::uint64_t value) {
    value = (value & 0x0f0f0f0f0f0f0f0full) + ((value >> 4) & 0x0f0f0f0f0f0f0f0full);
    return unsigned((value * 0x0101010101010101ull) >> 56);
  }

  [[gnu::noinline]] std::uint64_t public_rank(everett::rank15_view const & view,
                                           std::uint64_t group) {
    return view.rank(group);
  }

  [[gnu::noinline]] std::uint64_t public_range(everett::rank15_view const & view,
                                            std::uint64_t first, std::uint64_t last) {
    if (first == last) return 0;
    auto hi = view.rank(last - 1) + view.class_at(last - 1);
    return hi - view.rank(first);
  }

  constexpr std::size_t repetitions = 1 << 20;
  std::uint64_t observed = 0;

  template <class F> double measure(F run) {
    std::uint64_t checksum = 0;
    auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < repetitions; ++i) {
      auto result = run(i);
      // Keep each call and its input loads inside the timed loop, including
      // repeated queries. The checksum is consumed after timing.
      __asm__ volatile("" : "+r"(result) : : "memory");
      checksum += result;
    }
    auto end = std::chrono::steady_clock::now();
    observed += checksum;
    return std::chrono::duration<double, std::nano>(end - start).count() / repetitions;
  }

  template <class F> void report(char const * kind, char const * variant,
                                std::size_t groups, char const * queries, F run) {
    measure(run);
    std::array<double, 7> samples{};
    for (auto & sample : samples) sample = measure(run);
    std::sort(samples.begin(), samples.end());
    std::cout << kind << ',' << variant << ',' << groups << ',' << queries << ','
              << repetitions << ',' << samples.size() << ',' << std::fixed
              << std::setprecision(3) << samples[samples.size() / 2] << '\n';
  }
}

int main(int argc, char ** argv) {
  std::string_view selected = argc > 1 ? argv[1] : "";
  std::mt19937_64 random(0x1515);
  std::array<std::uint64_t, 4096> words{};
  for (auto & value : words) {
    value = random();
    unsigned expected = 0;
    for (unsigned i = 0; i < 16; ++i) expected += unsigned((value >> (4 * i)) & 15);
    if (sum_widen(value) != expected || sum_multiply(value) != expected) return 1;
  }
  std::cout << "kind,variant,groups,queries,repetitions,trials,median_ns\n";
  if (selected.empty() || selected == "sum") {
    report("sum", "widen", 16, "random", [&](std::size_t i) {
      return sum_widen(words[i % words.size()]);
    });
    report("sum", "multiply", 16, "random", [&](std::size_t i) {
      return sum_multiply(words[i % words.size()]);
    });
  }
  if (selected.empty() || selected == "rank") {
    for (std::size_t groups : {std::size_t{1} << 16, std::size_t{1} << 22}) {
      std::vector<std::uint8_t> source(groups);
      for (auto & value : source) value = random() & 15;
      auto index = everett::rank15_index::build(source, groups * 15);
      auto view = index.view();
      std::array<std::uint64_t, 65536> queries{};
      for (char const * mode : {"random", "tail15", "checkpoint"}) {
        for (auto & query : queries) {
          query = random() % groups;
          if (std::string_view(mode) == "tail15") query |= 127;
          if (std::string_view(mode) == "checkpoint") query &= ~std::uint64_t{127};
        }
        report("rank", "public", groups, mode, [&](std::size_t i) {
          return public_rank(view, queries[i % queries.size()]);
        });
      }
      std::array<std::uint64_t, 65536> ends{};
      for (char const * mode : {"near15", "independent"}) {
        for (std::size_t i = 0; i < queries.size(); ++i) {
          queries[i] = random() % groups;
          ends[i] = std::string_view(mode) == "near15"
            ? std::min<std::uint64_t>(queries[i] + 15, groups) : random() % groups;
          if (queries[i] > ends[i]) std::swap(queries[i], ends[i]);
        }
        report("range", "public", groups, mode, [&](std::size_t i) {
          auto slot = i % queries.size();
          return public_range(view, queries[slot], ends[slot]);
        });
      }
    }
  }
  std::cerr << "observed=" << observed << '\n';
}
