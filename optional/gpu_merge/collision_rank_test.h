/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks temporary GPU collision-rank construction and queries.
 *
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Include after the host gpu helper. These are correctness-only synthetic
// collision maps, not a timed general-rank comparison. Final rank15 format and
// query code are unchanged. Actual mark/scatter chronology is checked by the
// complete merge's CPU byte and decoded-row oracles.
namespace gpu_collision_rank_test {
inline void check(gpu &context, std::string const &name,
                  std::vector<std::uint8_t> const &flags, bool rank2048) {
  constexpr std::uint32_t guard = 0xa5a5a5a5u;
  auto bits = static_cast<std::uint32_t>(flags.size());
  auto bitmap_words = (bits + 31) / 32;
  auto block_bits = rank2048 ? 2048u : 512u;
  auto blocks = (bits + block_bits - 1) / block_bits;
  std::vector<std::uint32_t> bitmap(bitmap_words), prefix(std::size_t(bits) + 1);
  for (std::uint32_t i = 0; i != bits; ++i) {
    require(flags[i] <= 1, "collision fixture bit");
    prefix[i + 1] = prefix[i] + flags[i];
    bitmap[i / 32] |= std::uint32_t(flags[i]) << (i & 31);
  }

  auto storage_words = rank2048 ? bitmap_words : bitmap_words + blocks * 3 + 1;
  auto storage = context.buffer(std::size_t(storage_words + 2) * 4);
  std::memset(storage.contents, 0xa5, std::size_t(storage_words + 2) * 4);
  auto command = [context.queue commandBuffer];
  context.dispatch(command, rank2048 ? "collision_rank2048_clear" : "collision_clear",
                   storage_words, nil, storage, nil, bits);
  gpu::finish(command);
  auto map_words = static_cast<std::uint32_t *>(storage.contents);
  for (std::uint32_t i = 0; i != storage_words; ++i)
    require(map_words[i] == 0, "collision clear missed a word");
  require(map_words[storage_words] == guard && map_words[storage_words + 1] == guard,
          "collision clear output guard");
  if (!bitmap.empty())
    std::memcpy(storage.contents, bitmap.data(), bitmap.size() * 4);

  auto directory_words = blocks * 2 + 1;
  auto directory = rank2048 ? context.buffer(std::size_t(directory_words + 2) * 4) : nil;
  if (rank2048)
    std::memset(directory.contents, 0xa5, std::size_t(directory_words + 2) * 4);
  auto totals = context.buffer(std::size_t(blocks + 2) * 4);
  std::memset(totals.contents, 0xa5, std::size_t(blocks + 2) * 4);
  command = [context.queue commandBuffer];
  context.dispatch(command, rank2048 ? "rank_count" : "collision_rank512_count",
                   blocks, storage, rank2048 ? directory : storage, totals, bits);
  auto scanned = context.scan(command, totals, blocks);
  context.dispatch(command, rank2048 ? "collision_rank2048_finish" : "collision_rank512_finish",
                   std::max(1u, blocks), scanned, rank2048 ? directory : storage, totals, bits);
  gpu::finish(command);

  auto block_totals = static_cast<std::uint32_t const *>(totals.contents);
  for (std::uint32_t i = 0; i != blocks; ++i)
    require(block_totals[i] == prefix[std::min(bits, (i + 1) * block_bits)] - prefix[i * block_bits],
            "collision block population");
  require(block_totals[blocks] == guard && block_totals[blocks + 1] == guard,
          "collision population output guard");
  require(std::equal(bitmap.begin(), bitmap.end(), map_words), "collision bitmap changed");
  require(map_words[storage_words] == guard && map_words[storage_words + 1] == guard,
          "collision directory output guard");
  auto directory_data = rank2048 ? static_cast<std::uint32_t const *>(directory.contents)
                                : map_words + bitmap_words;
  for (std::uint32_t block = 0; block != blocks; ++block) {
    auto first = block * block_bits;
    if (rank2048) {
      std::uint32_t packed = 0;
      for (unsigned run = 0; run != 3; ++run) {
        auto start = std::min(bits, first + run * 512);
        auto end = std::min(bits, first + (run + 1) * 512);
        packed |= (prefix[end] - prefix[start]) << (run * 11);
      }
      require(directory_data[block * 2] == prefix[first] &&
                  directory_data[block * 2 + 1] == packed,
              "collision rank2048 packed bytes");
    } else {
      std::uint64_t packed = 0;
      for (unsigned pair = 1; pair != 8; ++pair)
        packed |= std::uint64_t(prefix[std::min(bits, first + pair * 64)] - prefix[first])
                  << ((pair - 1) * 9);
      require(directory_data[block * 3] == prefix[first] &&
                  directory_data[block * 3 + 1] == std::uint32_t(packed) &&
                  directory_data[block * 3 + 2] == std::uint32_t(packed >> 32),
              "collision rank512 packed bytes");
    }
  }
  require(directory_data[blocks * (rank2048 ? 2 : 3)] == prefix.back(),
          "collision final total");
  if (rank2048)
    require(directory_data[directory_words] == guard && directory_data[directory_words + 1] == guard,
            "collision rank2048 directory guard");

  std::vector<std::uint32_t> queries(prefix.size());
  for (std::uint32_t i = 0; i != queries.size(); ++i)
    queries[i] = i;
  auto positions = context.buffer(queries.size() * 4, queries.data());
  auto answers = context.buffer((queries.size() + 2) * 4);
  std::memset(answers.contents, 0xa5, (queries.size() + 2) * 4);
  command = [context.queue commandBuffer];
  context.dispatch(command, rank2048 ? "collision_rank2048_probe" : "collision_rank_probe",
                   static_cast<std::uint32_t>(queries.size()), nil, answers, directory,
                   static_cast<std::uint32_t>(queries.size()), nil, positions, storage, bits);
  gpu::finish(command);
  auto actual = static_cast<std::uint32_t const *>(answers.contents);
  require(std::equal(prefix.begin(), prefix.end(), actual), "collision exclusive rank query");
  require(actual[queries.size()] == guard && actual[queries.size() + 1] == guard,
          "collision query output guard");
  std::cout << "collision-rank," << (rank2048 ? 2048 : 512) << ',' << name << ",passed," << bits
            << '\n';
}

inline void cases(gpu &context, bool rank2048) {
  std::uint32_t random = 0xc0111510u;
  for (std::uint32_t bits : {0u, 1u, 31u, 32u, 33u, 63u, 64u, 65u, 511u, 512u, 513u,
                             2047u, 2048u, 2049u, 4097u}) {
    for (unsigned pattern = 0; pattern != 4; ++pattern) {
      std::vector<std::uint8_t> flags(bits);
      for (std::uint32_t i = 0; i != bits; ++i) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        flags[i] = std::uint8_t(pattern == 1 || (pattern == 2 && i % 31 == 0) ||
                                (pattern == 3 && random % 7 == 0));
      }
      check(context, std::to_string(bits) + "/" + std::to_string(pattern), flags, rank2048);
    }
  }
}
}

inline void collision_rank_adversarial(gpu &context) {
  gpu_collision_rank_test::cases(context, false);
}
inline void collision_rank2048_adversarial(gpu &context) {
  gpu_collision_rank_test::cases(context, true);
}
