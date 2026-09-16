/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Measures fixed-key output with fresh mappings and complete checksums.
 * \license
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
// Reuse the checked implementation verbatim. Its renamed program entry is
// never called; unlike main, an ordinary function needs an explicit return.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#define main fixed_original_main
#include "../fixed_gpu_merge/prototype.mm"
#undef main
#pragma clang diagnostic pop
#include <everett/crc32c.h>
#include "cases.h"

void checksum_fixed(std::byte *bytes, std::size_t size) {
  auto header = reinterpret_cast<u32 *>(bytes);
  header[19] = everett::crc32c(std::span<std::byte const>(bytes + 256, size - 256));
  header[20] = 0;
  header[20] = everett::crc32c(std::span<std::byte const>(bytes, 256));
}
int main(int argc, char **argv) {
  @autoreleasepool { try {
    require(argc == 5, "usage: fixed kernels.metallib scratch case-index trial-count");
    auto index = unsigned(std::stoul(argv[3])), trials = unsigned(std::stoul(argv[4]));
    auto f = matched_fixture::make(index);
    auto expected = matched_fixture::merged(f);
    std::filesystem::path root(argv[2]); std::filesystem::create_directories(root);
    matched_fixture::save(root, f, expected);
    gpu context(argv[1]); std::cerr << "device=" << context.device.name.UTF8String << '\n';
    auto left = encode(f.a, f.variable, 1001), right = encode(f.b, f.variable, 1002);
    checksum_fixed(left.data(), left.size()); checksum_fixed(right.data(), right.size());
    mapping a(root / "input-a.ff", left.size()), b(root / "input-b.ff", right.size()), due(root / "due.bin", f.due.size() * 4);
    std::memcpy(a.data, left.data(), left.size()); std::memcpy(b.data, right.data(), right.size());
    std::memcpy(due.data, f.due.data(), f.due.size() * 4);
    a.clip(left.size()); b.clip(right.size()); due.clip(f.due.size() * 4);
    auto n = a.words()[3] + b.words()[3], value_bytes = a.words()[6] + b.words()[6];
    layout plan(f.variable, n, value_bytes);
    std::vector<std::byte> oracle(plan.capacity());
    auto size = cpu_merge(input_view(a.words()), input_view(b.words()), due.words(), oracle.data());
    oracle.resize(size); checksum_fixed(oracle.data(), size);
    input_view checked(reinterpret_cast<u32 const *>(oracle.data()));
    for (u32 i = 0; i < n; ++i) {
      auto first = checked.offset(i), last = checked.offset(i + 1);
      require(checked.key_at(i) == expected[i].k && last - first == expected[i].value.size() &&
        (last == first || std::memcmp(checked.payload() + first, expected[i].value.data(), last - first) == 0), "fixed logical oracle");
    }
    matched_fixture::header();
    for (int trial = -1; trial < int(trials); ++trial) {
      timing result; double checksum_ms = 0;
      auto begin = clock_type::now();
      @autoreleasepool {
        mapping out(root / "output.fixed", plan.capacity());
        auto ga = context.import(a), gb = context.import(b), gd = context.import(due), go = context.import(out);
        result = gpu_merge(context, ga, gb, gd, go, a.words(), b.words(), due.words());
        auto checksum_start = clock_type::now(); checksum_fixed(out.bytes(), result.bytes); checksum_ms = elapsed(checksum_start);
        out.clip(result.bytes);
      }
      auto complete = elapsed(begin);
      std::ifstream stream(root / "output.fixed", std::ios::binary);
      std::vector<char> actual((std::istreambuf_iterator<char>(stream)), {});
      require(actual.size() == oracle.size() && std::memcmp(actual.data(), oracle.data(), oracle.size()) == 0, "fixed canonical output");
      matched_fixture::row(index, f.variable ? "fv" : "ff", trial, left.size(), right.size(), result.bytes,
        n, value_bytes, complete, result.total_ms, result.gpu_ms, checksum_ms);
    }
    return 0;
  } catch (std::exception const &e) { std::cerr << e.what() << '\n'; return 1; } }
}
