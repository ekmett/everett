/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Benchmarks Diet's CRC32C backends.
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/crc32c.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
  using bytes = std::span<std::byte const>;
  using function = std::uint32_t (*)(bytes);
  [[gnu::noinline]] std::uint32_t bitwise(bytes input) {
    std::uint32_t crc = ~std::uint32_t{0};
    for (auto byte : input) {
      crc ^= std::to_integer<std::uint32_t>(byte);
      for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0x82f63b78u);
    }
    return ~crc;
  }
  [[gnu::noinline]] std::uint32_t public_crc(bytes input) { return diet::crc32c(input); }
  [[gnu::noinline]] std::uint32_t portable(bytes input) {
    return diet::crc32c_detail::portable::crc32_impl(
      0, reinterpret_cast<char const *>(input.data()), input.size());
  }
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
  [[gnu::noinline]] std::uint32_t scalar(bytes input) {
    return diet::crc32c_detail::arm_scalar::crc32_impl(
      0, reinterpret_cast<char const *>(input.data()), input.size());
  }
#if defined(__ARM_FEATURE_CRYPTO) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  [[gnu::noinline]] std::uint32_t pmull(bytes input) {
    return diet::crc32c_detail::arm_pmull::crc32_impl(
      0, reinterpret_cast<char const *>(input.data()), input.size());
  }
#if defined(__ARM_FEATURE_SHA3)
  [[gnu::noinline]] std::uint32_t fused(bytes input) {
    return diet::crc32c_detail::arm_eor3::crc32_impl(
      0, reinterpret_cast<char const *>(input.data()), input.size());
  }
#endif
#endif
#endif
  struct variant { char const * name; function run; };
  constexpr variant variants[] = {
    {"bitwise", bitwise}, {"portable", portable},
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
    {"arm_scalar", scalar},
#if defined(__ARM_FEATURE_CRYPTO) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    {"arm_pmull", pmull},
#if defined(__ARM_FEATURE_SHA3)
    {"arm_fused", fused},
#endif
#endif
#endif
    {"public", public_crc}
  };
  std::uint32_t observed = 0;
  double measure(function run, bytes input, std::size_t length, std::size_t reps, bool rotating) {
    auto stride = (length + 63) & ~std::size_t(63);
    auto slots = rotating ? input.size() / stride : 1;
    std::size_t slot = 0;
    std::uint32_t checksum = 0;
    auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < reps; ++i) {
      auto result = run(input.subspan(slot * stride, length));
      // A memory clobber keeps repeated identical inputs from being hoisted.
      __asm__ volatile("" : "+r"(result) : : "memory");
      checksum ^= result;
      if (++slot == slots) slot = 0;
    }
    auto end = std::chrono::steady_clock::now();
    observed ^= checksum;
    return std::chrono::duration<double, std::nano>(end - start).count() / double(reps);
  }
}

int main(int argc, char ** argv) {
  std::string_view selected = argc > 1 ? argv[1] : "";
  std::vector<std::byte> data(128 * 1024 * 1024);
  std::uint64_t random = 0x123456789abcdef;
  for (auto & byte : data) {
    random ^= random << 13; random ^= random >> 7; random ^= random << 17;
    byte = std::byte(random);
  }
  std::cout << "mode,bytes,variant,repetitions,trials,median_ns,GB_per_s\n";
  for (bool rotating : {false, true})
    for (std::size_t length : {96u, 256u, 512u, 4096u, 1048576u, 8388608u})
      for (auto const & variant : variants) {
        if (!selected.empty() && variant.name != selected) continue;
        if (rotating && (length == 256 || length == 512)) continue;
        auto trial = measure(variant.run, data, length, length < 4096 ? 10000 : 4, rotating);
        auto reps = std::clamp<std::size_t>(std::size_t(15000000.0 / trial), 1, 10000000);
        if (rotating) {
          auto slots = data.size() / ((length + 63) & ~std::size_t(63));
          reps = ((reps + slots - 1) / slots) * slots;
        }
        std::array<double, 7> samples{};
        for (auto & sample : samples) sample = measure(variant.run, data, length, reps, rotating);
        std::sort(samples.begin(), samples.end());
        auto median = samples[samples.size() / 2];
        std::cout << (rotating ? "rotating128MiB" : "warm") << ',' << length << ',' << variant.name << ','
                  << reps << ',' << samples.size() << ',' << std::fixed << std::setprecision(3)
                  << median << ',' << double(length) / median << '\n';
      }
  std::cerr << "observed=" << observed << '\n';
  return 0;
}
