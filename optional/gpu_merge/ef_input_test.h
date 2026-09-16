/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Cross-checks GPU Elias-Fano selection against synthetic CPU fixtures.
 *
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#pragma once

#include <everett/elias_fano.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

// Include after the optional host's gpu definition. This helper belongs only to
// correctness mode; no generated fixtures or CPU selects enter merge timings.
inline void ef_input_test(gpu & context) {
  std::vector<std::vector<std::uint64_t>> fixtures{{0}, {0, 0, 0, 1, 1},
    {(1u << 30) + 127}, {(1u << 29) + 31, (1u << 30) + 63, (3u << 29) + 127}};
  for (auto [count, stride] : {std::pair{257u, 2u}, std::pair{259u, 33u}, std::pair{513u, 131072u}}) {
    std::vector<std::uint64_t> values;
    for (unsigned i = 0; i != count; ++i) values.push_back(std::uint64_t(i + 1) * stride + i % stride);
    fixtures.push_back(std::move(values));
  }
  std::vector<std::uint64_t> sparse(8192);
  for (std::size_t i = 128; i != sparse.size(); ++i) sparse[i] = (1u << 20) + i;
  fixtures.push_back(std::move(sparse));
  bool saw_sparse = false, saw_width0 = false, saw_width1 = false, saw_width30 = false;
  for (auto const & expected : fixtures) {
    auto ef = everett::elias_fano::build(expected);
    saw_sparse |= !ef.sparse.empty();
    saw_width0 |= ef.low_width == 0; saw_width1 |= ef.low_width == 1; saw_width30 |= ef.low_width == 30;
    // Start at a deliberately unaligned byte address, as portable file bodies
    // need not align their uint64 words with the imported VM page.
    std::vector<std::byte> bytes(3);
    std::array<std::uint32_t, 12> metadata{};
    auto word = [&](std::uint64_t value) {
      for (unsigned b = 0; b != 8; ++b) bytes.push_back(std::byte(value >> (8 * b)));
    };
    auto words = [&](unsigned part, auto const & values) {
      metadata[2 * part] = static_cast<std::uint32_t>(bytes.size());
      metadata[2 * part + 1] = static_cast<std::uint32_t>(values.size());
      for (auto value : values) word(value);
    };
    words(0, ef.low); words(1, ef.high);
    metadata[4] = static_cast<std::uint32_t>(bytes.size());
    metadata[5] = static_cast<std::uint32_t>(ef.samples.size());
    for (auto sample : ef.samples) { word(sample.first); word(sample.sparse); }
    words(3, ef.sparse);
    metadata[8] = static_cast<std::uint32_t>(ef.universe);
    metadata[9] = ef.low_width;
    metadata[11] = static_cast<std::uint32_t>((ef.universe >> ef.low_width) + ef.entry_count);
    bytes.resize((bytes.size() + 3) & ~std::size_t(3));
    auto input = context.buffer(bytes.size(), bytes.data());
    auto packet = context.buffer(sizeof(metadata), metadata.data());
    auto output = context.buffer(expected.size() * 4);
    auto status = context.buffer(4);
    auto run = [&](bool valid) {
      *static_cast<std::uint32_t *>(status.contents) = 0;
      auto command = [context.queue commandBuffer];
      auto count = static_cast<std::uint32_t>(expected.size());
      context.dispatch(command, "parse_ef_probe", count, input, output, status, count,
        nil, packet, nil, 0, 0, count - 1);
      gpu::finish(command);
      auto failed = *static_cast<std::uint32_t const *>(status.contents);
      require(valid ? failed == 0 : failed != 0, "GPU EF fixture acceptance");
      if (valid) {
        auto result = static_cast<std::uint32_t const *>(output.contents);
        for (std::size_t i = 0; i != expected.size(); ++i)
          require(result[i] == expected[i] && result[i] == ef.view().select(i), "GPU EF select mismatch");
      }
    };
    run(true);
    // A sample claiming a position above uint32, or a sparse index outside its
    // section, must be rejected before it can direct any out-of-range load.
    auto bad = bytes;
    if (ef.sparse.empty()) bad[metadata[4] + 4] = std::byte{1};
    else {
      auto at = metadata[4] + 8;
      for (unsigned b = 0; b != 8; ++b) bad[at + b] = std::byte{0};
      auto outside = std::uint64_t(ef.sparse.size());
      for (unsigned b = 0; b != 8; ++b) bad[at + b] = std::byte(outside >> (8 * b));
    }
    std::memcpy(input.contents, bad.data(), bad.size());
    run(false);
  }
  require(saw_sparse && saw_width0 && saw_width1 && saw_width30, "GPU EF fixture coverage");
}
