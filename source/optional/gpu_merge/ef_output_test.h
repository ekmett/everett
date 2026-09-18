/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks GPU output Elias-Fano sections against the CPU wire encoder.
 *
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#include "../host_backend.h"
#pragma once

// Included after prototype.mm's gpu helper. Synthetic offset sequences test
// EF independently; repeated offsets need not represent actual native records.
namespace gpu_ef_output_test {
inline void check(gpu & context, std::string const & name,
                  std::vector<std::uint64_t> const & residuals,
                  std::uint32_t common = 0, bool partial = false) {
  require(!residuals.empty() && residuals.front() == 0, "output EF fixture origin");
  auto entries = static_cast<std::uint32_t>(residuals.size());
  auto records = (entries - 1) * 15 - (partial && entries > 1 ? 7 : 0);
  auto bits64 = residuals.back() + std::uint64_t(records) * common;
  require(bits64 < (1ull << 31), "output EF fixture extent");
  auto bits = static_cast<std::uint32_t>(bits64);
  std::vector<std::uint32_t> offsets(records);
  for (std::uint32_t i = 0; i != records; ++i)
    offsets[i] = static_cast<std::uint32_t>(residuals[i / 15] + std::uint64_t(i) * common);
  auto cpu = everett::elias_fano::build<everett_experiment::architecture>(residuals);
  auto input = context.buffer(offsets.size() * 4, offsets.data());
  auto groups = (entries + 255) / 256;
  auto counts = context.buffer(groups * 4);
  auto command = [context.queue commandBuffer];
  context.dispatch(command, "ef_output_sparse_count", groups, input, counts, nil,
    records, nil, nil, nil, bits, common, cpu.low_width);
  auto starts = context.scan(command, counts, groups);
  gpu::finish(command);
  auto count_data = static_cast<std::uint32_t const *>(counts.contents);
  auto start_data = static_cast<std::uint32_t const *>(starts.contents);
  require(std::uint64_t(start_data[groups - 1]) + count_data[groups - 1] == cpu.sparse.size(),
    "output EF sparse extent");
  for (std::uint32_t g = 0; g != groups; ++g) {
    auto n = std::min(256u, entries - g * 256);
    auto sparse = cpu.samples[g].sparse != std::numeric_limits<std::uint64_t>::max();
    require(count_data[g] == (sparse ? n : 0), "output EF sparse classification");
    if (sparse) require(start_data[g] == cpu.samples[g].sparse, "output EF sparse start");
  }

  std::array<std::vector<std::byte>, 4> expected;
  everett::sort_profile_file_detail::append_words(expected[0], cpu.low);
  everett::sort_profile_file_detail::append_words(expected[1], cpu.high);
  expected[2].resize(cpu.samples.size() * 16);
  for (std::size_t i = 0; i != cpu.samples.size(); ++i) {
    everett::file_detail::put(expected[2], i * 16, 8, cpu.samples[i].first);
    everett::file_detail::put(expected[2], i * 16 + 8, 8, cpu.samples[i].sparse);
  }
  everett::sort_profile_file_detail::append_words(expected[3], cpu.sparse);
  std::array<std::uint32_t, 4> at{};
  std::uint32_t words = 2; // Guards test nonzero destination bases.
  for (unsigned i = 0; i != 4; ++i) {
    at[i] = words;
    words += static_cast<std::uint32_t>(expected[i].size() / 4) + 2;
  }
  auto output = context.buffer(std::size_t(words) * 4);
  std::memset(output.contents, 0xa5, std::size_t(words) * 4);
  command = [context.queue commandBuffer];
  auto emit = [&](char const * kernel, std::uint32_t threads, unsigned section) {
    context.dispatch(command, kernel, threads, input, output, nil, records,
      counts, starts, nil, bits, common, cpu.low_width, at[section]);
  };
  emit("ef_output_low", static_cast<std::uint32_t>(cpu.low.size() * 2), 0);
  emit("ef_output_high", static_cast<std::uint32_t>(cpu.high.size() * 2), 1);
  emit("ef_output_samples", groups, 2);
  emit("ef_output_sparse", entries, 3);
  gpu::finish(command);
  auto bytes = static_cast<std::byte const *>(output.contents);
  auto verify_guard = [&](std::uint32_t first, std::uint32_t last) {
    for (auto i = first; i != last; ++i)
      require(bytes[i] == std::byte{0xa5}, "output EF guard overwritten");
  };
  verify_guard(0, 8);
  for (unsigned i = 0; i != 4; ++i) {
    require(expected[i].empty() || std::memcmp(bytes + at[i] * 4,
      expected[i].data(), expected[i].size()) == 0, "output EF wire bytes");
    auto end = at[i] * 4 + static_cast<std::uint32_t>(expected[i].size());
    verify_guard(end, end + 8);
  }
  std::cout << "ef-output," << name << ",passed," << entries << ','
    << cpu.low_width << ',' << cpu.sparse.size() << '\n';
}
}

inline void ef_output_adversarial(gpu & context) {
  using gpu_ef_output_test::check;
  check(context, "empty-records", {0});
  check(context, "repeated-low-zero", std::vector<std::uint64_t>(513, 0));
  check(context, "constant-value-stride", std::vector<std::uint64_t>(258, 0), 13, true);
  for (unsigned width : {1u, 5u, 17u, 29u}) {
    auto count = width == 29 ? 3u : 67u;
    auto universe = std::uint64_t(count) * (std::uint64_t{1} << width) + 1;
    std::vector<std::uint64_t> values(count);
    for (unsigned i = 0; i != count; ++i) values[i] = universe * i / (count - 1);
    check(context, "packed-width-" + std::to_string(width), values, 3, true);
  }
  for (std::uint64_t jump : {3840u, 3841u, 65536u}) {
    std::vector<std::uint64_t> values(4097, jump);
    std::fill(values.begin(), values.begin() + 127, 0);
    check(context, "sparse-threshold-" + std::to_string(jump), values);
  }
}
