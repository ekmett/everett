/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks fused GPU output planning against the independent CPU EF builder.
 *
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#pragma once

#include "../host_backend.h"
#include <everett/elias_fano.h>

// Include after gpu. Synthetic monotone offsets exercise EF boundaries even
// when zero record lengths would not occur in a native optional-string file.
namespace gpu_output_plan_test {
inline void check(gpu &context, std::string const &name,
                  std::vector<std::uint64_t> const &residuals,
                  std::uint32_t common = 0, bool partial = false,
                  std::uint32_t explicit_records = 0) {
  require(residuals.size() >= 2 && residuals.front() == 0, "output plan fixture origin");
  auto entries = static_cast<std::uint32_t>(residuals.size());
  auto records = explicit_records ? explicit_records : (entries - 1) * 15 - (partial ? 7 : 0);
  require((records + 14) / 15 + 1 == entries, "output plan fixture count");
  auto bits64 = residuals.back() + std::uint64_t(records) * common;
  require(bits64 < (1ull << 31), "output plan fixture extent");
  auto bits = static_cast<std::uint32_t>(bits64);
  std::vector<std::uint32_t> offsets(records), lengths(records), references(records, 2);
  for (std::uint32_t i = 0; i != records; ++i)
    offsets[i] = static_cast<std::uint32_t>(residuals[i / 15] + std::uint64_t(i) * common);
  for (std::uint32_t i = 0; i != records; ++i)
    lengths[i] = (i + 1 == records ? bits : offsets[i + 1]) - offsets[i];
  auto cpu = everett::elias_fano::build<everett_experiment::architecture>(residuals);
  auto input = context.buffer(offsets.size() * 4, offsets.data());
  auto sizes = context.buffer(lengths.size() * 4, lengths.data());
  auto selected = context.buffer(references.size() * 4, references.data());
  auto groups = (entries + 255) / 256;
  constexpr std::uint32_t guard = 0xa5a5a5a5;
  for (auto stride : {4u, 8u}) {
    // Different descriptor values plus a nonzero first compacted reference
    // make reading descriptor zero, or the wrong descriptor stride, observable.
    std::vector<std::uint32_t> descriptors(stride * 3 + 2, 19);
    descriptors[stride * 2 + 3] = common ? common : 7;
    auto descriptor = context.buffer(descriptors.size() * 4, descriptors.data());
    auto output = context.buffer((groups + 3) * 4);
    std::memset(output.contents, 0xa5, (groups + 3) * 4);
    std::uint32_t packet[]{common ? 0u : 1u, guard, guard, guard, guard, guard, guard, guard};
    auto status = context.buffer(sizeof(packet), packet);
    auto command = [context.queue commandBuffer];
    context.dispatch(command, "ef_output_sparse_plan", groups, input, output, status,
                     records, sizes, selected, status, stride, 0, 0, 0, 0, 0, descriptor);
    auto starts = context.scan(command, output, groups);
    gpu::finish(command);
    auto plan = static_cast<std::uint32_t const *>(status.contents);
    require(plan[0] == packet[0] && plan[1] == bits && plan[2] == common &&
                plan[3] == residuals.back() && plan[4] == cpu.low_width,
            "GPU output plan scalar metadata");
    for (unsigned i = 5; i != 8; ++i)
      require(plan[i] == guard, "GPU output plan packet guard");
    auto counts = static_cast<std::uint32_t const *>(output.contents);
    auto scanned = static_cast<std::uint32_t const *>(starts.contents);
    for (std::uint32_t i = 0; i != groups; ++i) {
      auto sparse = cpu.samples[i].sparse != std::numeric_limits<std::uint64_t>::max();
      require(counts[i] == (sparse ? std::min(256u, entries - i * 256) : 0),
              "GPU output plan sparse classification");
      if (sparse)
        require(scanned[i] == cpu.samples[i].sparse, "GPU output plan sparse start");
    }
    require(std::uint64_t(scanned[groups - 1]) + counts[groups - 1] == cpu.sparse.size(),
            "GPU output plan sparse total");
    for (auto i = groups; i != groups + 3; ++i)
      require(counts[i] == guard, "GPU output plan count guard");
    require(std::memcmp(input.contents, offsets.data(), offsets.size() * 4) == 0 &&
                std::memcmp(sizes.contents, lengths.data(), lengths.size() * 4) == 0 &&
                std::memcmp(selected.contents, references.data(), references.size() * 4) == 0 &&
                std::memcmp(descriptor.contents, descriptors.data(), descriptors.size() * 4) == 0,
            "GPU output plan changed an input");
  }
  std::cout << "output-plan," << name << ",passed," << records << ',' << entries << '\n';
}
}

inline void output_plan_adversarial(gpu &context) {
  using gpu_output_plan_test::check;
  check(context, "single", {0, 17}, 1, false, 1);
  check(context, "one-full-block", {0, 0}, 1);
  check(context, "partial-eof", {0, 13, 33}, 9, true);
  check(context, "repeated", std::vector<std::uint64_t>(513, 0));
  check(context, "fixed-stride", std::vector<std::uint64_t>(258, 0), 13, true);
  for (unsigned width : {1u, 5u, 17u, 29u}) {
    auto count = width == 29 ? 3u : 67u;
    auto universe = std::uint64_t(count) * (std::uint64_t{1} << width) + 1;
    std::vector<std::uint64_t> values(count);
    for (unsigned i = 0; i != count; ++i)
      values[i] = universe * i / (count - 1);
    check(context, "low-width-" + std::to_string(width), values, 3, true);
  }
  for (std::uint64_t jump : {3840u, 3841u, 65536u}) {
    std::vector<std::uint64_t> values(4097, jump);
    std::fill(values.begin(), values.begin() + 127, 0);
    check(context, "sparse-threshold-" + std::to_string(jump), values);
  }
}
