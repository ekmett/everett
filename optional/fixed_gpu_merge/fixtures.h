/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Deterministic logical records shared by native-format merge experiments.
 * \license
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fixed_fixture {
  using u32 = std::uint32_t;
  using u64 = std::uint64_t;
  using key = std::array<u32, 4>;
  struct record { key k; std::vector<std::byte> value; };
  enum class value_shape { mixed, tiny, large };

  struct fixture {
    std::string name;
    bool variable;
    std::vector<record> a, b;
    std::vector<u32> due;
    value_shape shape = value_shape::mixed;
  };
  inline key make_key(u32 n) { return {0xf0000000u + (n >> 18), (n >> 12) & 63, 0x80000000u + ((n >> 6) & 63), n & 63}; }
  inline record make_record(u32 ordinal, u32 length, u32 salt) {
    record result{make_key(ordinal), std::vector<std::byte>(length)};
    for (u32 i = 0; i < length; ++i) result.value[i] = std::byte((ordinal * 17 + i * 31 + salt) & 255);
    return result;
  }
  inline fixture make_fixture(std::string name, bool variable, u32 na, u32 nb, u32 cancel_percent,
                       bool giant = false, bool scattered = false, value_shape shape = value_shape::mixed) {
    fixture f{std::move(name), variable, {}, {}, {0x31434445u, 0, na, 1001, 0, 0, 0, 0}};
    f.shape = shape;
    auto length = [&](u32 i, u32 salt) {
      if (!variable) return 16u;
      auto seed = i * 107 + salt * 19;
      if (shape == value_shape::tiny) return seed % 7;
      if (shape == value_shape::large) return 2048 + seed % 2049;
      return seed % 513;
    };
    for (u32 i = 0; i < na; ++i) f.a.push_back(make_record(i * 2, length(i, 1), 1));
    // A contiguous deletion run covers long canceled ranges, including starts
    // and ends. Half of B's available entries replace these exact canceled keys.
    auto nc = u32(u64(na) * cancel_percent / 100);
    for (u32 i = 0; i < nc; ++i) {
      auto target = scattered ? u32(u64(i) * na / nc) : i;
      f.due.push_back(target); f.due[4] += u32(f.a[target].value.size());
    }
    f.due[1] = nc;
    auto replacements = std::min(nc, nb / 2);
    for (u32 i = 0; i < replacements; ++i) f.b.push_back(make_record(f.due[8 + i] * 2, length(i, 2), 2));
    for (u32 i = replacements; i < nb; ++i) f.b.push_back(make_record((i - replacements) * 2 + 1, length(i, 2), 2));
    std::sort(f.b.begin(), f.b.end(), [](auto const &a, auto const &b) { return a.k < b.k; });
    if (giant) {
      // Thousands of zero-length values and one large gap force sparse EF
      // exceptions, including in the merged output, not only in an input.
      for (auto &r : f.a) r.value.clear();
      for (auto &r : f.b) r.value.clear();
      if (!f.a.empty()) f.a[std::min<std::size_t>(7, f.a.size() - 1)].value.assign(1 << 20, std::byte{0xa5});
      f.due[4] = 0;
      for (u32 i = 0; i < nc; ++i) f.due[4] += u32(f.a[f.due[8 + i]].value.size());
    }
    return f;
  }
}
