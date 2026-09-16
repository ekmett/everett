// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
#pragma once

// Include after the optional host's gpu/require declarations. This oracle
// extracts individual bits independently of the shader's word formulas.
inline void check_word_emit(gpu &context) {
  std::array<std::uint8_t, 32> source{};
  for (std::size_t i = 0; i != source.size(); ++i)
    source[i] = std::uint8_t((i * 137 + 53) ^ (i >> 1));
  std::vector<std::uint32_t> descriptions, expected;
  auto add = [&](std::uint32_t kind, std::uint32_t value, std::uint32_t offset, std::uint32_t count,
                 std::uint32_t result) {
    descriptions.insert(descriptions.end(), {kind, value, offset, count});
    expected.push_back(result);
  };
  for (std::uint32_t offset = 0; offset != 225; ++offset) {
    for (std::uint32_t count = 1; count <= 32; ++count) {
      std::uint32_t value = 0;
      for (std::uint32_t i = 0; i != count; ++i) {
        auto position = offset + i;
        auto bit = (source[position >> 3] >> (7 - (position & 7))) & 1;
        value |= std::uint32_t(bit) << (31 - i);
      }
      add(0, offset, 0, count, value);
    }
  }
  constexpr std::array<std::uint32_t, 22> values{0,
                                                 1,
                                                 2,
                                                 3,
                                                 14,
                                                 15,
                                                 16,
                                                 31,
                                                 32,
                                                 63,
                                                 64,
                                                 255,
                                                 256,
                                                 65534,
                                                 65535,
                                                 65536,
                                                 (1u << 29) - 1,
                                                 1u << 29,
                                                 (1u << 30) - 1,
                                                 1u << 30,
                                                 (1u << 31) - 1,
                                                 0xfffffffeu};
  for (auto value : values) {
    auto zeros = std::uint32_t(std::bit_width(value + 1) - 1);
    auto size = zeros * 2 + 1;
    for (std::uint32_t offset = 0; offset != size; ++offset) {
      for (std::uint32_t count = 1; count <= std::min(32u, size - offset); ++count) {
        std::uint32_t result = 0;
        for (std::uint32_t i = 0; i != count; ++i) {
          auto position = offset + i;
          auto bit = position < zeros ? 0u : ((value + 1) >> (2 * zeros - position)) & 1;
          result |= bit << (31 - i);
        }
        add(1, value, offset, count, result);
      }
    }
  }
  auto input = context.buffer(source.size(), source.data());
  auto metadata = context.buffer(descriptions.size() * sizeof(std::uint32_t), descriptions.data());
  auto output = context.buffer(expected.size() * sizeof(std::uint32_t));
  auto command = [context.queue commandBuffer];
  context.dispatch(command, "word_emit_probe", std::uint32_t(expected.size()), input, output, nil,
                   std::uint32_t(expected.size()), metadata);
  gpu::finish(command);
  require(std::memcmp(output.contents, expected.data(), expected.size() * sizeof(std::uint32_t)) ==
              0,
          "GPU word/EG extraction differs from independent bit oracle");
  std::cout << "word_emit_probe," << expected.size() << ",passed\n";
}
