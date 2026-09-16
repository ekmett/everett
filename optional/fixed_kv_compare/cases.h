/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Identical logical records for fixed and front-coded Metal merge measurements.
 * \license
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once
#include "../fixed_gpu_merge/fixtures.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <iterator>
#include <stdexcept>

namespace matched_fixture {
  struct specification { bool variable; std::uint32_t count; bool hashed; };
  inline constexpr std::array<specification, 12> cases{{
    {false,4096,false}, {false,65536,false}, {false,262144,false},
    {true,4096,false}, {true,65536,false}, {true,131072,false},
    {false,4096,true}, {false,65536,true}, {false,262144,true},
    {true,4096,true}, {true,65536,true}, {true,131072,true}}};
  inline std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
  }
  inline fixed_fixture::key hash_key(fixed_fixture::key key) {
    // Invert the structured generator, then apply a bijection to the high
    // half. This proves distinct logical IDs remain distinct 128-bit keys.
    auto id = (std::uint64_t(key[0] - 0xf0000000u) << 18) |
      (std::uint64_t(key[1]) << 12) | (std::uint64_t(key[2] - 0x80000000u) << 6) | key[3];
    auto high = mix(id + 0x9e3779b97f4a7c15ULL), low = mix(id ^ 0xd1b54a32d192ed03ULL);
    return {std::uint32_t(high >> 32), std::uint32_t(high), std::uint32_t(low >> 32), std::uint32_t(low)};
  }
  inline std::string key_bytes(fixed_fixture::key const & key) {
    std::string result;
    for (auto lane : key) for (int byte = 3; byte >= 0; --byte) result.push_back(char(lane >> (byte * 8)));
    return result;
  }
  inline fixed_fixture::fixture make(unsigned index) {
    if (index >= cases.size()) throw std::out_of_range("matched case index");
    auto spec = cases[index];
    auto result = fixed_fixture::make_fixture("case-" + std::to_string(index), spec.variable,
      spec.count, spec.count, 0);
    if (spec.hashed) for (auto *rows : {&result.a, &result.b}) {
      for (auto &row : *rows) row.k = hash_key(row.k);
      std::sort(rows->begin(), rows->end(), [](auto const &a, auto const &b) { return a.k < b.k; });
    }
    return result;
  }
  inline std::vector<fixed_fixture::record> merged(fixed_fixture::fixture const & fixture) {
    std::vector<fixed_fixture::record> result;
    result.reserve(fixture.a.size() + fixture.b.size());
    std::merge(fixture.a.begin(), fixture.a.end(), fixture.b.begin(), fixture.b.end(), std::back_inserter(result),
      [](auto const &a, auto const &b) { return a.k < b.k; });
    for (std::size_t i = 1; i < result.size(); ++i)
      if (!(result[i - 1].k < result[i].k)) throw std::runtime_error("nonunique matched keys");
    return result;
  }
  inline void logical_file(std::filesystem::path const &path, std::vector<fixed_fixture::record> const &rows) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    auto number = [&](std::uint64_t n) { for (unsigned i = 0; i < 8; ++i) out.put(char(n >> (8 * i))); };
    number(rows.size());
    for (auto const &row : rows) {
      auto key = key_bytes(row.k); out.write(key.data(), key.size()); number(row.value.size());
      out.write(reinterpret_cast<char const *>(row.value.data()), row.value.size());
    }
    if (!out) throw std::runtime_error("write logical fixture identity");
  }
  inline void save(std::filesystem::path const &root, fixed_fixture::fixture const &fixture,
      std::vector<fixed_fixture::record> const &output) {
    logical_file(root / "logical-a.bin", fixture.a); logical_file(root / "logical-b.bin", fixture.b);
    logical_file(root / "logical-output.bin", output);
  }
  inline void header() {
    std::cout << std::setprecision(17);
    std::cout << "case,layout,distribution,older,newer,cancelled,trial,input_a_bytes,input_b_bytes,output_bytes,"
      "output_records,value_bytes,complete_ms,internal_ms,device_ms,checksum_ms\n";
  }
  inline void row(unsigned index, char const *layout, int trial, std::size_t a, std::size_t b, std::size_t output,
      std::size_t count, std::size_t values, double total, double internal, double device, double checksum) {
    std::cout << index << ',' << layout << ',' << (cases[index].hashed ? "hash-like" : "structured") << ','
      << cases[index].count << ',' << cases[index].count << ",0," << trial << ',' << a << ',' << b << ','
      << output << ',' << count << ',' << values << ',' << total << ',' << internal << ',' << device << ',' << checksum << '\n';
  }
}
