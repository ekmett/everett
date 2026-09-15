// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

#include <everett/cola_sections.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace everett;

template <class P> void run(unsigned prefix, unsigned rounds, std::filesystem::path const & dump) {
  using node = cola_index<P>;
  auto records = [prefix](unsigned count, unsigned stride, unsigned offset) {
    std::vector<profile_record> result;
    result.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
      auto n = i * stride + offset;
      std::string text(prefix, 'x');
      for (unsigned shift : {24u, 16u, 8u, 0u}) text.push_back(char(n >> shift));
      auto key = bit_string::from_bytes(text);
      if constexpr (P::unit == profile_unit::bit) {
        key.bytes.push_back(std::byte((n & 7) << 5));
        key.bit_size += 3;
      }
      result.push_back({std::move(key), bit_string::from_bytes("value")});
    }
    return result;
  };
  auto leaf = std::make_shared<node const>(node::build(records(4096, 5, 0)));
  auto main = std::make_shared<node const>(node::build(records(4096, 3, 1), leaf, leaf->native_owner()));
  auto secondary = std::make_shared<profile_array<P> const>(profile_array<P>::build(records(4096, 3, 0)));
  auto native = std::make_shared<profile_array<P> const>(profile_array<P>::build(records(2048, 6, 2)));
  object_id native_id("00000000000000000000000000000001"), side_id("00000000000000000000000000000002");
  blob_identity main_id{object_id("00000000000000000000000000000003"), object_id("00000000000000000000000000000004")};
  std::uint32_t expected = 0;
  for (unsigned round = 0; round <= rounds; ++round) {
    auto begin = std::chrono::steady_clock::now();
    cola_index_builder<P> builder(native, main, secondary);
    while (!builder.done()) builder.step(4096);
    auto result = builder.finish();
    auto end = std::chrono::steady_clock::now();
    auto encoded = encode_cola_sections(result, native_id, main_id, side_id).materialize();
    auto checksum = crc32c(encoded);
    if (!round) {
      expected = checksum;
      if (!dump.empty()) {
        auto name = std::string(P::unit == profile_unit::byte ? "byte" : "bit") + "-" +
          std::to_string(P::group_size) + "-" + std::to_string(prefix) + ".index";
        std::ofstream output(dump / name, std::ios::binary);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output.write(reinterpret_cast<char const *>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        output.close();
      }
    }
    else {
      if (checksum != expected) throw std::runtime_error("COLA benchmark output changed");
      auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
      std::cout << (P::unit == profile_unit::byte ? "byte" : "bit") << ',' << P::group_size << ','
        << prefix << ',' << round << ',' << result.virtual_size() << ',' << elapsed << ','
        << double(elapsed) / double(result.virtual_size()) << ',' << encoded.size() << ',' << checksum << '\n';
    }
  }
}

int main(int argc, char ** argv) {
  unsigned rounds = argc > 1 ? unsigned(std::stoul(argv[1])) : 3;
  std::filesystem::path dump = argc > 2 ? argv[2] : "";
  if (!dump.empty()) std::filesystem::create_directories(dump);
  for (unsigned prefix : {0u, 4096u}) {
    run<storage_policy<profile_unit::byte, variable_values, 3>>(prefix, rounds, dump);
    run<storage_policy<profile_unit::byte, variable_values, 15>>(prefix, rounds, dump);
    run<storage_policy<profile_unit::bit, variable_values, 3, golomb<3>>>(prefix, rounds, dump);
    run<storage_policy<profile_unit::bit, variable_values, 15>>(prefix, rounds, dump);
  }
}
