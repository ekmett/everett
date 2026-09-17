/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks fused augmented-origin rank packing against CPU COLA indexes.
 *
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#pragma once

#include "../host_backend.h"
#include <everett/sort_profile.h>

// Included after the optional host's gpu helper. This tests a navigation pass
// whose input is an existing augmented origin stream, not a GPU index merger.
namespace gpu_index_rank_test {
using p = everett_experiment::string_policy;
using array = everett::sort_profile_array<p>;
using index = everett::cola_index<p, array>;
using ranks = std::array<everett::rank_groups<15>, 2>;

inline void check(gpu &context, std::string const &name, std::vector<std::uint32_t> const &origins,
                  ranks const &expected) {
  auto n = static_cast<std::uint32_t>(origins.size());
  auto groups = (n + 14) / 15, words = ((groups + 15) / 16) * 2;
  auto blocks = (groups + 127) / 128;
  auto input = context.buffer(origins.size() * 4, origins.data());
  auto classes = context.buffer(std::size_t(words * 2 + 2) * 4);
  std::memset(classes.contents, 0xa5, std::size_t(words * 2 + 2) * 4);
  auto status = context.buffer(4);
  std::memset(status.contents, 0, 4);
  auto main = context.buffer(blocks * 4), secondary = context.buffer(blocks * 4);
  std::array<id<MTLBuffer>, 2> checkpoints{context.buffer(blocks * 8), context.buffer(blocks * 8)};
  auto command = [context.queue commandBuffer];
  context.dispatch(command, "index_rank_classes", words, input, classes, status, n);
  context.dispatch(command, "index_rank_blocks", blocks, classes, main, secondary, n);
  auto main_prefix = context.scan(command, main, blocks);
  auto secondary_prefix = context.scan(command, secondary, blocks);
  context.dispatch(command, "rank15_finish", blocks, main_prefix, checkpoints[0], nil, blocks);
  context.dispatch(command, "rank15_finish", blocks, secondary_prefix, checkpoints[1], nil, blocks);
  gpu::finish(command);
  require(*static_cast<std::uint32_t const *>(status.contents) == 0,
          "index rank origin validation");
  auto actual = static_cast<std::uint32_t const *>(classes.contents);
  for (unsigned route = 0; route != 2; ++route) {
    require(expected[route].classes.size() * 2 == words &&
                expected[route].checkpoints.size() == blocks,
            "index rank oracle shape");
    for (std::uint32_t word = 0; word != words; ++word)
      require(actual[route * words + word] ==
                  std::uint32_t(expected[route].classes[word / 2] >> ((word & 1) * 32)),
              "index rank class bytes");
    auto points = static_cast<std::uint32_t const *>(checkpoints[route].contents);
    for (std::uint32_t block = 0; block != blocks; ++block)
      require(points[block * 2] == expected[route].checkpoints[block] && points[block * 2 + 1] == 0,
              "index rank checkpoint bytes");
  }
  require(actual[words * 2] == 0xa5a5a5a5u && actual[words * 2 + 1] == 0xa5a5a5a5u,
          "index rank output guard");
  std::cout << "index-rank," << name << ",passed," << n << '\n';
}

inline ranks reference(std::vector<std::uint32_t> const &origins) {
  std::array<std::vector<std::uint64_t>, 2> populations;
  for (std::size_t at = 0; at < origins.size(); at += 15) {
    std::array<std::uint64_t, 2> count{};
    for (auto i = at; i < std::min(at + 15, origins.size()); ++i)
      if (origins[i])
        ++count.at(origins[i] - 1);
    for (unsigned route = 0; route != 2; ++route)
      populations[route].push_back(count[route]);
  }
  return {everett::rank_groups<15>::build(populations[0], origins.size()),
          everett::rank_groups<15>::build(populations[1], origins.size())};
}

inline std::shared_ptr<array const> native(std::uint32_t count, std::uint32_t shift) {
  everett::sort_profile_writer<p> writer;
  for (std::uint32_t i = 0; i != count; ++i) {
    std::uint32_t number = i * 2 + shift;
    std::string key(4, '\0');
    for (unsigned byte = 0; byte != 4; ++byte)
      key[byte] = char(number >> ((3 - byte) * 8));
    std::optional<std::string> value;
    if (i % 3)
      value = std::string("value"); // Tombstones remain native occurrences.
    writer.append<everett::unsorted<std::optional<std::string>>>(key, value);
  }
  return std::make_shared<array const>(writer.finish());
}

// Reconstruct only this test's augmented order from the completed CPU node.
// Stable origin order preserves all borrowed duplicates. These host loops are
// correctness setup, not a proposed implementation or timed GPU preprocessing.
inline void real_index(gpu &context, std::uint32_t count) {
  auto old = native(count, 0), side = native(count, 0), own = native(count, 0);
  auto leaf = std::make_shared<index const>(index::adopt_native(old));
  auto middle = std::make_shared<index const>(index::adopt_native(native(count, 1), leaf, side));
  auto root = index::adopt_native(own, middle, side);
  struct occurrence {
    everett::bit_string key;
    unsigned origin;
  };
  std::vector<occurrence> order;
  auto gather = [&](auto cursor, unsigned origin) {
    while (!cursor.done()) {
      order.push_back({everett::bit_string::copy(cursor.peek().key.prefix), origin});
      cursor.advance();
    }
  };
  gather(root.native().view().cursor(), 0);
  gather(root.borrowed(0).view().cursor(), 1);
  gather(root.borrowed(1).view().cursor(), 2);
  std::stable_sort(order.begin(), order.end(), [](auto const &a, auto const &b) {
    auto comparison = everett::compare_bits(a.key.view(), b.key.view());
    return comparison ? comparison < 0 : a.origin < b.origin;
  });
  require(order.size() == root.virtual_size(), "index rank augmented size");
  std::vector<std::uint32_t> origins;
  std::array<std::uint64_t, 2> borrowed{};
  std::array<std::size_t, 2> previous_borrow{order.size(), order.size()};
  bool equal_native = false;
  for (std::size_t i = 0; i != order.size(); ++i) {
    auto const &item = order[i];
    if (i % 15 == 0)
      for (unsigned route = 0; route != 2; ++route) {
        auto common = previous_borrow[route] == order.size()
                          ? 0
                          : everett::compare_common_bits(order[previous_borrow[route]].key.view(),
                                                      item.key.view())
                                .common_bits;
        require(root.cut_lcps(route)[i / 15] == common, "index rank fixture cut LCP semantics");
      }
    bool equal = i && everett::compare_bits(order[i - 1].key.view(), item.key.view()) == 0;
    equal_native = item.origin == 0 || (equal && equal_native);
    if (item.origin) {
      auto route = item.origin - 1;
      require(root.view().false_borrow(route, borrowed[route]++) == equal_native,
              "index rank fixture false-borrow semantics");
      previous_borrow[route] = i;
    }
    origins.push_back(item.origin);
  }
  ranks expected{root.interleave(0), root.interleave(1)};
  check(context, "cpu-index-" + std::to_string(count), origins, expected);
}
} // namespace gpu_index_rank_test

inline void index_rank_adversarial(gpu &context) {
  using namespace gpu_index_rank_test;
  for (std::uint32_t n :
       {0u, 1u, 14u, 15u, 16u, 119u, 120u, 121u, 239u, 240u, 1919u, 1920u, 1921u}) {
    for (unsigned pattern = 0; pattern != 3; ++pattern) {
      std::vector<std::uint32_t> origins(n);
      for (std::uint32_t i = 0; i != n; ++i)
        origins[i] = pattern == 0 ? 0 : pattern == 1 ? 1 : (i * 17 + i / 13) % 3;
      check(context, "tail-" + std::to_string(n) + '-' + std::to_string(pattern), origins,
            reference(origins));
    }
  }
  for (auto n : {0u, 16u, 64u, 2048u})
    real_index(context, n);
  std::uint32_t bad = 3;
  auto input = context.buffer(4, &bad), output = context.buffer(16), status = context.buffer(4);
  std::memset(status.contents, 0, 4);
  auto command = [context.queue commandBuffer];
  context.dispatch(command, "index_rank_classes", 2, input, output, status, 1);
  gpu::finish(command);
  require(*static_cast<std::uint32_t const *>(status.contents) == 1,
          "index rank invalid origin accepted");
}
