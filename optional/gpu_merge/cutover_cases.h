/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Measures header-based GPU merge cutover on calibration and held-out cases.
 *
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#pragma once

#include <array>
#include <iomanip>
#include <span>
#include <string>
#include <vector>

// Include after the optional host's gpu_merge and native/policy aliases. The
// host enables its final checked GPU variant and warms every pipeline first.
namespace everett_gpu_cutover {
  struct case_spec {
    bool held_out;
    std::uint32_t records, ratio, prefix, overlap, value_bytes;
    bool fixed_values;
  };

  inline std::vector<case_spec> cases() {
    std::vector<case_spec> result;
    for (auto n : {256u, 1024u, 4096u, 16384u, 65536u, 262144u})
      result.push_back({false, n, 1, 32, 50, 8, false});
    for (auto n : {4096u, 65536u, 262144u}) {
      for (auto ratio : {4u, 16u}) result.push_back({false, n, ratio, 32, 50, 8, false});
      for (auto prefix : {0u, 256u}) result.push_back({false, n, 1, prefix, 50, 8, false});
      for (auto overlap : {0u, 100u}) result.push_back({false, n, 1, 32, overlap, 8, false});
      result.push_back({false, n, 1, 32, 50, 64, false});
      // Bound the final parser's max-record-bits * total-records guard.
      result.push_back({false, n, 1, 32, 50, n == 262144 ? 256u : 512u, false});
      result.push_back({false, n, 1, 32, 50, 8, true});
    }
    result.push_back({false, 65536, 16, 256, 0, 512, true});
    for (auto n : {2048u, 8192u, 32768u, 131072u}) {
      result.push_back({true, n, 1, 64, 25, 32, false});
      result.push_back({true, n, 3, 128, 75, 128, true});
      result.push_back({true, n, 8, 0, 0, 512, false});
      result.push_back({true, n, 16, 256, 100, 8, true});
    }
    return result;
  }

  inline std::shared_ptr<native const> input(std::filesystem::path const & path,
      case_spec const & spec, unsigned side) {
    everett::sort_profile_writer<policy> writer;
    auto count = side ? spec.records / spec.ratio : spec.records;
    for (std::uint32_t i = 0; i != count; ++i) {
      auto id = std::uint64_t(i) * 4 + (side && i % 100 >= spec.overlap ? 1 : 0);
      std::string key(spec.prefix, 'p');
      if (spec.prefix > 4) key[spec.prefix / 2] = '\0';
      for (int byte = 7; byte >= 0; --byte) key.push_back(char(id >> (byte * 8)));
      std::optional<std::string> value;
      if (spec.fixed_values || (i + side) % 11)
        value = std::string(spec.value_bytes + (spec.fixed_values ? 0 : i % 13), char('a' + side));
      writer.append<sort_type>(key, value);
    }
    auto array = writer.finish();
    auto wire = everett::encoded_sort_sections<policy>::from(array).materialize();
    write_bytes(path, wire);
    auto result = std::make_shared<native const>(native::open(everett::file<policy>::open(path)));
    result->scan();
    return result;
  }

  // Call one case per process, after an untimed warm-up, to permit an external
  // driver to shuffle calibration and held-out cases without changing fixtures.
  inline void run(gpu & context, std::filesystem::path const & directory,
      unsigned case_index, unsigned trials) {
    auto grid = cases();
    require(case_index < grid.size() && trials, "cutover case index/trials");
    auto const & spec = grid[case_index];
    auto a = input(directory / "left.kv", spec, 0), b = input(directory / "right.kv", spec, 1);
    auto a_bytes = std::filesystem::file_size(directory / "left.kv");
    auto b_bytes = std::filesystem::file_size(directory / "right.kv");
    auto av = a->view(), bv = b->view();
    std::cout << "cutover_device," << context.device.name.UTF8String
      << "|registryID=" << std::dec << context.device.registryID << '\n';
    std::cout << "kind,case,split,n,ratio,prefix,overlap,value_bytes,fixed_values,trial,cpu_first,"
      "a_records,b_records,a_payload_bits,b_payload_bits,a_file_bytes,b_file_bytes,"
      "a_common_value_bits,b_common_value_bits,a_terminal_key_bits,b_terminal_key_bits,"
      "cpu_merge_ms,cpu_all_ms,gpu_all_ms,gpu_command_ms,parse_ms,prepare_ms,order_ms,size_ms,emit_ms,assembly_ms,"
      "survivors,output_bits,output_bytes,output_crc32c\n";
    auto prior_precision = std::cout.precision();
    std::cout << std::setprecision(12);
    // One verified warm-up of each implementation is excluded from timings.
    for (unsigned pass = 0; pass != trials + 1; ++pass) {
      auto trial = pass ? pass - 1 : 0;
      std::vector<std::byte> encoded;
      double cpu_merge_ms = 0, cpu_all_ms = 0;
      std::uint64_t cpu_count = 0;
      auto cpu_run = [&] {
        auto begin = clock_type::now();
        everett::sort_profile_merge_builder<policy, native> cpu(a, b);
        cpu.step(std::numeric_limits<std::uint64_t>::max());
        auto merged = cpu.finish();
        cpu_merge_ms = elapsed(begin); cpu_count = merged.size();
        encoded = everett::encoded_sort_sections<policy>::from(merged).materialize();
        mapping output(directory / "cpu.kv", encoded.size());
        std::memcpy(output.data, encoded.data(), encoded.size());
        require(ftruncate(output.fd, off_t(encoded.size())) == 0, "cutover CPU extent");
        cpu_all_ms = elapsed(begin);
      };
      merge_result result;
      bool cpu_first = ((case_index + trial) & 1) == 0;
      if (cpu_first) { cpu_run(); result = gpu_merge(context, *a, *b, directory / "gpu.kv"); }
      else { result = gpu_merge(context, *a, *b, directory / "gpu.kv"); cpu_run(); }
      auto opened = native::open(everett::file<policy>::open(result.path));
      opened.scan();
      require(opened.size() == cpu_count, "cutover survivor count");
      std::ifstream stream(result.path, std::ios::binary);
      std::vector<char> actual((std::istreambuf_iterator<char>(stream)), {});
      require(actual.size() == encoded.size() &&
          std::memcmp(actual.data(), encoded.data(), encoded.size()) == 0, "cutover canonical output bytes");
      if (!pass) continue;
      auto common = [](auto const & view) -> std::int64_t {
        return view.metadata().common_value_width ? std::int64_t(*view.metadata().common_value_width) : -1;
      };
      std::cout << "cutover," << case_index << ',' << (spec.held_out ? "heldout" : "train")
        << ',' << spec.records << ',' << spec.ratio << ',' << spec.prefix << ',' << spec.overlap
        << ',' << spec.value_bytes << ',' << spec.fixed_values << ',' << trial << ',' << cpu_first
        << ',' << av.size() << ',' << bv.size() << ',' << av.data().size() << ',' << bv.data().size()
        << ',' << a_bytes << ',' << b_bytes << ',' << common(av) << ',' << common(bv)
        << ',' << av.metadata().terminal_key_units << ',' << bv.metadata().terminal_key_units
        << ',' << cpu_merge_ms << ',' << cpu_all_ms << ',' << result.total << ',' << result.gpu_ms
        << ',' << result.decode << ',' << result.prepare << ',' << result.order << ',' << result.size
        << ',' << result.emit << ',' << result.assembly << ',' << result.count << ',' << result.bits
        << ',' << result.bytes << ',' << everett::crc32c(encoded) << '\n';
    }
    std::cout.precision(prior_precision);
  }
}

inline void run_cutover(gpu & context, std::filesystem::path const & directory,
    unsigned case_index, unsigned trials) {
  everett_gpu_cutover::run(context, directory, case_index, trials);
}
