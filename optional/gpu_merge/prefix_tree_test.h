/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks complete GPU prefix-min heaps against a scalar oracle.
 *
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// Include after gpu. Correctness only: the descriptor parser and full-key
// resolver have separate tests. Synthetic retained prefixes exercise every
// heap node, even when a real merge would not query that particular subtree.
namespace gpu_prefix_tree_test {
inline std::vector<std::uint32_t> descriptors(std::uint32_t count,
                                              std::uint32_t older,
                                              unsigned pattern) {
  std::vector<std::uint32_t> result(std::size_t(count) * 8);
  std::uint32_t random = 0x51eaf317u;
  for (std::uint32_t i = 0; i != count; ++i) {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    auto key_bytes = pattern == 0 ? 0u : pattern == 4 ? random % 97 : 64u;
    result[i * 8 + 1] = key_bytes;
    result[i * 8 + 6] = i < older ? 0u : 1u;
    if (i == 0 || i == older)
      continue;
    auto limit = 8 * std::min(key_bytes, result[(i - 1) * 8 + 1]);
    auto retained = pattern == 1 ? i % (limit + 1)
                    : pattern == 2 ? limit - i % (limit + 1)
                    : pattern == 3 ? (i % 127 == 0 ? 0u : limit)
                                   : random % (limit + 1);
    result[i * 8 + 4] = retained;
  }
  return result;
}

inline void check(gpu &context, std::string const &name,
                  std::vector<std::uint32_t> const &source,
                  std::uint32_t older, bool invalid = false) {
  constexpr std::uint32_t guard = 0xa5a5a5a5u;
  constexpr std::uint32_t initial_status = 2u;
  auto count = static_cast<std::uint32_t>(source.size() / 8);
  require(count != 0 && source.size() == std::size_t(count) * 8 && older <= count,
          "prefix tree fixture dimensions");
  auto base = std::bit_ceil(count);
  std::vector<std::uint32_t> expected(std::size_t(base) * 2 + 3, guard);
  for (std::uint32_t i = 0; i != base; ++i)
    expected[base + i] = i < count ? source[i * 8 + 4] : 0xffffffffu;
  for (auto node = base; --node != 0;)
    expected[node] = std::min(expected[node * 2], expected[node * 2 + 1]);

  auto input = context.buffer((source.size() + 2) * 4);
  std::memset(input.contents, 0xa5, (source.size() + 2) * 4);
  std::memcpy(input.contents, source.data(), source.size() * 4);
  for (bool tiled : {false, true}) {
    auto tree = context.buffer(expected.size() * 4);
    std::memset(tree.contents, 0xa5, expected.size() * 4);
    std::uint32_t status_words[]{initial_status, guard, guard};
    auto status = context.buffer(sizeof(status_words), status_words);
    auto command = [context.queue commandBuffer];
    context.build_prefix_tree(command, tree, status, input, count, older, tiled);
    gpu::finish(command);
    auto fail = [&](char const *reason) {
      throw std::runtime_error("prefix-tree " + name + (tiled ? " tiled: " : " levelwise: ") + reason);
    };
    auto actual = static_cast<std::uint32_t const *>(tree.contents);
    if (!std::equal(expected.begin(), expected.end(), actual))
      fail("heap or output canary mismatch");
    auto checked = static_cast<std::uint32_t const *>(status.contents);
    if (checked[0] != (initial_status | (invalid ? 16u : 0u)) ||
        checked[1] != guard || checked[2] != guard)
      fail("validation flag or status canary mismatch");
    auto original = static_cast<std::uint32_t const *>(input.contents);
    if (!std::equal(source.begin(), source.end(), original) ||
        original[source.size()] != guard || original[source.size() + 1] != guard)
      fail("descriptor input changed");
  }
  std::cout << "prefix-tree," << name << ",passed," << count << ',' << older << '\n';
}
}

inline void prefix_tree_adversarial(gpu &context) {
  using namespace gpu_prefix_tree_test;
  unsigned pattern = 0;
  for (std::uint32_t count : {1u, 2u, 3u, 15u, 127u, 128u, 129u, 255u, 256u, 257u,
                               511u, 512u, 513u, 1023u, 4097u, 65535u, 65536u, 65537u}) {
    auto older = count / 2;
    check(context, "size/" + std::to_string(count), descriptors(count, older, pattern++ % 5), older);
  }
  // A split exactly on, just before and just after a tile boundary. Both
  // one-empty-input directions and a one-record input exercise source roots.
  for (std::uint32_t older : {0u, 1u, 255u, 256u, 257u, 512u, 513u})
    check(context, "split/" + std::to_string(older), descriptors(513, older, 4), older);
  for (unsigned shape = 0; shape != 5; ++shape)
    check(context, "shape/" + std::to_string(shape), descriptors(1025, 257, shape), 257);

  auto valid = descriptors(513, 256, 4);
  for (std::uint32_t index : {0u, 255u, 256u, 512u}) {
    auto bad = valid;
    bad[index * 8 + 6] = 2;
    check(context, "bad-source/" + std::to_string(index), bad, 256, true);
  }
  for (std::uint32_t index : {0u, 256u}) {
    auto bad = valid;
    bad[index * 8 + 4] = 1;
    check(context, "bad-root/" + std::to_string(index), bad, 256, true);
  }
  for (std::uint32_t index : {1u, 255u, 257u, 512u}) {
    auto bad = valid;
    bad[index * 8 + 4] = bad[(index - 1) * 8 + 1] * 8 + 1;
    check(context, "bad-retention/" + std::to_string(index), bad, 256, true);
  }
  auto bad_empty_left = descriptors(1, 0, 0);
  bad_empty_left[4] = 1;
  check(context, "bad-empty-left-root", bad_empty_left, 0, true);
  auto bad_empty_right = descriptors(1, 1, 0);
  bad_empty_right[4] = 1;
  check(context, "bad-empty-right-root", bad_empty_right, 1, true);
}
