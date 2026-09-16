/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Compares existing class/bitmap rank builders with shared GPU inputs.
 *
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#pragma once

// Include after the host's gpu, mapping and timing helpers. Allocation of the
// scan hierarchy is explicit so the reused-buffer measurements hide no copies.
struct reusable_scan {
  std::uint32_t count;
  id<MTLBuffer> output, totals;
  std::unique_ptr<reusable_scan> parent;

  reusable_scan(gpu &context, std::uint32_t n)
      : count(n), output(context.buffer(std::size_t(n) * 4)),
        totals(context.buffer(std::size_t((n + 255) / 256) * 4)) {
    if (n > 256)
      parent = std::make_unique<reusable_scan>(context, (n + 255) / 256);
  }
  void encode(gpu &context, id<MTLCommandBuffer> command, id<MTLBuffer> input) const {
    context.dispatch(command, "scan_blocks", count, input, output, totals, count);
    if (parent) {
      parent->encode(context, command, totals);
      context.dispatch(command, "scan_add", count, parent->output, output, nil, count);
    }
  }
};

// Same validated scalar class-packing loop as rank15_index::build, writing
// already allocated sections. The owning builder is measured separately.
inline void rank15_reused(std::span<std::uint8_t const> source, std::uint32_t bits,
                          std::span<std::uint64_t> packed, std::span<std::uint64_t> checkpoints) {
  std::fill(packed.begin(), packed.end(), 0);
  std::uint64_t total = 0;
  for (std::size_t i = 0; i < source.size(); ++i) {
    auto limit = i + 1 == source.size() && bits % 15 ? bits % 15 : 15;
    require(source[i] <= limit, "rank15 shared class population");
    if (!(i & 127))
      checkpoints[i >> 7] = total;
    packed[i >> 4] |= std::uint64_t(source[i]) << (4 * (i & 15));
    total += source[i];
  }
}

// This bounded experiment has at most 2^28 bits, so only the first rank
// epoch is needed. It uses the production NEON/popcount512 helper and the
// production tail algorithm, without rank_index::build's owning input copy.
inline void rank2048_reused(std::span<std::uint64_t const> source, std::uint32_t bits,
                            std::span<everett::rank_block> output) {
  std::uint32_t total = 0;
  for (std::size_t block = 0; block < output.size(); ++block) {
    unsigned counts[4]{};
    auto first = block * 32;
    if (std::uint64_t(block + 1) * 2048 <= bits) {
      for (unsigned i = 0; i < 4; ++i)
        counts[i] = everett::rank_detail::popcount512(source.data() + first + i * 8);
    } else {
      for (auto word = first; word < source.size(); ++word)
        counts[(word - first) >> 3] += unsigned(std::popcount(source[word]));
    }
    output[block] = {total, counts[0] | (counts[1] << 11) | (counts[2] << 22)};
    total += counts[0] + counts[1] + counts[2] + counts[3];
  }
}

inline void rank_shared_case(gpu &context, std::filesystem::path const &directory,
                             std::uint32_t bits, unsigned trials) {
  require(bits > 0 && bits <= (1u << 28), "rank shared fixture extent");
  std::mt19937_64 random(bits + 67891);
  auto word_count = (std::uint64_t(bits) + 63) / 64;
  auto groups = (bits + 14) / 15;
  mapping bitmap(directory / "rank-shared-bitmap.bin", word_count * 8);
  mapping class_input(directory / "rank-shared-classes.bin", groups);
  auto words = std::span(static_cast<std::uint64_t *>(bitmap.data), word_count);
  for (auto &word : words)
    word = random();
  if (bits % 64)
    words.back() &= (std::uint64_t{1} << (bits % 64)) - 1;
  auto classes = std::span(static_cast<std::uint8_t *>(class_input.data), groups);
  for (std::size_t i = 0; i < classes.size(); ++i) {
    auto limit = i + 1 == classes.size() && bits % 15 ? bits % 15 : 15;
    classes[i] = std::uint8_t(random() % (limit + 1));
  }
  auto expected = everett::rank_index::build(words, bits);
  auto expected15 = everett::rank15_index::build(classes, bits);
  for (unsigned mode = 0; mode < 2; ++mode) {
    auto &source = mode ? class_input : bitmap;
    auto input = [context.device newBufferWithBytesNoCopy:source.data
                                                   length:source.length
                                                  options:MTLResourceStorageModeShared
                                              deallocator:nil];
    require(input != nil, "rank shared input import");
    auto blocks = mode ? (groups + 127) / 128 : (bits + 2047) / 2048;
    auto output = context.buffer(mode ? expected15.classes.size() * 8 : expected.blocks.size() * 8);
    auto checkpoints = context.buffer(std::size_t(blocks) * 8);
    auto totals = context.buffer(std::size_t(blocks) * 4);
    reusable_scan scan(context, blocks);
    std::vector<everett::rank_block> cpu_blocks(expected.blocks.size());
    std::vector<std::uint64_t> cpu_classes(expected15.classes.size());
    std::vector<std::uint64_t> cpu_checkpoints(expected15.checkpoints.size());
    for (unsigned trial = 0; trial < trials; ++trial) {
      double own_ms = 0, reused_ms = 0, gpu_ms = 0, command_ms = 0;
      auto cpu_run = [&] {
        auto begin = clock_type::now();
        if (mode) {
          auto built = everett::rank15_index::build(classes, bits);
          own_ms = elapsed(begin);
          require(built.classes == expected15.classes &&
                      built.checkpoints == expected15.checkpoints,
                  "rank15 owning shared-input oracle");
        } else {
          auto built = everett::rank_index::build(words, bits);
          own_ms = elapsed(begin);
          require(std::memcmp(built.blocks.data(), expected.blocks.data(),
                              built.blocks.size() * 8) == 0,
                  "rank2048 owning shared-input oracle");
        }
        begin = clock_type::now();
        if (mode)
          rank15_reused(classes, bits, cpu_classes, cpu_checkpoints);
        else
          rank2048_reused(words, bits, cpu_blocks);
        reused_ms = elapsed(begin);
      };
      auto gpu_run = [&] {
        auto begin = clock_type::now();
        auto command = [context.queue commandBuffer];
        context.dispatch(command, mode ? "rank15_classes" : "rank_count", blocks, input, output,
                         totals, mode ? groups : bits);
        scan.encode(context, command, totals);
        context.dispatch(command, mode ? "rank15_finish" : "rank_finish", blocks, scan.output,
                         mode ? checkpoints : output, nil, blocks);
        gpu::finish(command);
        gpu_ms = elapsed(begin);
        command_ms = (command.GPUEndTime - command.GPUStartTime) * 1000;
      };
      if (trial & 1) {
        gpu_run();
        cpu_run();
      } else {
        cpu_run();
        gpu_run();
      }
      if (mode) {
        require(cpu_classes == expected15.classes && cpu_checkpoints == expected15.checkpoints,
                "rank15 reused CPU oracle");
        require(std::memcmp(output.contents, expected15.classes.data(),
                            expected15.classes.size() * 8) == 0 &&
                    std::memcmp(checkpoints.contents, expected15.checkpoints.data(),
                                expected15.checkpoints.size() * 8) == 0,
                "rank15 reused GPU oracle");
      } else {
        require(
            std::memcmp(cpu_blocks.data(), expected.blocks.data(), cpu_blocks.size() * 8) == 0 &&
                std::memcmp(output.contents, expected.blocks.data(), expected.blocks.size() * 8) ==
                    0,
            "rank2048 reused output oracle");
      }
      std::cout << "rank-shared," << (mode ? "class15" : "bitmap2048") << ',' << bits << ','
                << trial << ',' << own_ms << ',' << reused_ms << ',' << gpu_ms << ',' << command_ms
                << '\n';
    }
  }
}
