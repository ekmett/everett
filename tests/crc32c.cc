/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Everett's CRC32C behavior.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/crc32c.h>

#if defined(CRC_AINLINE) || defined(CRC_ALIGN) || defined(CRC_EXPORT) || defined(clmul_lo) || defined(clmul_hi)
#error "generated CRC macros leaked into the consumer"
#endif

#include <algorithm>
#include <array>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
  using bytes = std::span<std::byte const>;
  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }

  // Independent bit-at-a-time oracle: no production tables or fold constants.
  std::uint32_t oracle(bytes input, std::uint32_t initial = 0) {
    auto crc = ~initial;
    for (auto byte : input) {
      crc ^= std::to_integer<std::uint8_t>(byte);
      for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78u : 0u);
    }
    return ~crc;
  }

  void check(bytes input, std::uint32_t initial = 0) {
    auto expected = oracle(input, initial);
    auto data = reinterpret_cast<char const *>(input.data());
    auto size = input.size();
    using namespace everett::crc32c_detail;
    require(portable::crc32_impl(initial, data, size) == expected, "portable CRC mismatch");
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32) && (defined(__GNUC__) || defined(__clang__))
    require(arm_scalar::crc32_impl(initial, data, size) == expected, "Arm scalar CRC mismatch");
#if (defined(__ARM_FEATURE_CRYPTO) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    require(arm_pmull::crc32_impl(initial, data, size) == expected, "Arm PMULL CRC mismatch");
#if defined(__ARM_FEATURE_SHA3)
    require(arm_eor3::crc32_impl(initial, data, size) == expected, "Arm fused CRC mismatch");
#endif
#endif
#elif defined(__x86_64__) && defined(__SSE4_2__)
    require(x86_scalar::crc32_impl(initial, data, size) == expected, "x86 scalar CRC mismatch");
#if defined(__PCLMUL__)
    require(x86_pclmul::crc32_impl(initial, data, size) == expected, "x86 PCLMUL CRC mismatch");
#if defined(__AVX512F__) && defined(__AVX512VL__)
    require(x86_avx512::crc32_impl(initial, data, size) == expected, "x86 AVX512 CRC mismatch");
#if defined(__VPCLMULQDQ__)
    require(x86_vpclmul::crc32_impl(initial, data, size) == expected, "x86 VPCLMUL CRC mismatch");
#endif
#endif
#endif
#endif
    require(everett::crc32c(input, initial) == expected, "seeded public CRC mismatch");
    if (initial == 0) require(everett::crc32c(input) == expected, "default public CRC mismatch");
  }

  void known_vectors() {
    require(everett::crc32c({}) == 0, "empty CRC mismatch");
    auto text = std::string_view("123456789");
    require(everett::crc32c(std::as_bytes(std::span(text))) == 0xe3069283u, "standard CRC vector mismatch");
    std::array<std::byte, 32> input{};
    require(everett::crc32c(input) == 0x8a9136aau, "zero CRC vector mismatch");
    input.fill(std::byte{0xff});
    require(everett::crc32c(input) == 0x62a8ab43u, "ones CRC vector mismatch");
    for (unsigned i = 0; i < input.size(); ++i) input[i] = std::byte(i);
    require(everett::crc32c(input) == 0x46dd794eu, "increasing CRC vector mismatch");
    std::reverse(input.begin(), input.end());
    require(everett::crc32c(input) == 0x113fdb5cu, "decreasing CRC vector mismatch");
  }

  // Independent linear-map oracle: square the one-byte state transition as a
  // matrix over GF(2), rather than multiplying reflected polynomials.
  std::uint32_t shift_oracle(std::uint32_t value, std::uint64_t count) {
    using matrix = std::array<std::uint32_t, 32>;
    auto apply = [](matrix const & map, std::uint32_t input) {
      std::uint32_t result = 0;
      for (unsigned i = 0; i != 32; ++i)
        if (input & (std::uint32_t{1} << i)) result ^= map[i];
      return result;
    };
    matrix map{};
    for (unsigned i = 0; i != 32; ++i) {
      auto column = std::uint32_t{1} << i;
      for (unsigned bit = 0; bit != 8; ++bit)
        column = (column >> 1) ^ ((column & 1) ? 0x82f63b78u : 0);
      map[i] = column;
    }
    while (count) {
      if (count & 1) value = apply(map, value);
      matrix squared{};
      for (unsigned i = 0; i != 32; ++i) squared[i] = apply(map, map[i]);
      map = squared;
      count >>= 1;
    }
    return value;
  }

  void combinations() {
    std::mt19937_64 random(0xc04b1e);
    std::vector<std::byte> storage(8193);
    for (auto & byte : storage) byte = std::byte(random());
    auto input = bytes(storage);
    for (std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{127}, std::size_t{513}, input.size()}) {
      auto whole = input.first(size);
      auto expected = oracle(whole);
      for (std::size_t split = 0; split <= size; split += size > 513 ? 31 : 1)
        require(everett::crc32c_combine(oracle(whole.first(split)), oracle(whole.subspan(split)), size - split) == expected,
                "combined CRC differs from independent concatenation");
      require(everett::crc32c_combine(expected, 0, 0) == expected, "empty suffix CRC combine");
    }
    // Prefix replacement is what permits finalizing a section directory
    // without reading the already-streamed FC and navigation payload again.
    for (std::size_t prefix : {std::size_t{0}, std::size_t{96}, std::size_t{128}, std::size_t{256}, input.size()}) {
      auto replacement = storage;
      for (std::size_t i = 0; i != prefix; ++i) replacement[i] ^= std::byte{0xa7};
      auto difference = oracle(input.first(prefix)) ^ oracle(bytes(replacement).first(prefix));
      auto adjusted = oracle(input) ^ everett::crc32c_combine(difference, 0, input.size() - prefix);
      require(adjusted == oracle(replacement), "CRC prefix adjustment rereads suffix incorrectly");
    }
    for (unsigned bit = 0; bit != 64; ++bit) {
      auto count = std::uint64_t{1} << bit;
      for (auto length : {count - 1, count, count | (count - 1)}) {
        auto first = static_cast<std::uint32_t>(random());
        auto second = static_cast<std::uint32_t>(random());
        require(everett::crc32c_combine(first, second, length) == (shift_oracle(first, length) ^ second),
                "combined CRC lost high length bits");
      }
    }
  }

  void boundaries() {
    std::mt19937_64 random(0xc32c);
    std::vector<std::byte> input(1024 * 1024 + 96);
    for (auto & byte : input) byte = std::byte(random());
    constexpr std::array<std::uint32_t, 4> seeds{0, 0xffffffffu, 0x12345678u, 0x80000000u};
    for (std::size_t offset = 0; offset < 32; ++offset)
      for (std::size_t size = 0; size <= 320; ++size)
        for (auto seed : seeds) check(bytes(input).subspan(offset, size), seed);
    // Vector/scalar loop thresholds, multiple blocks, merge tails, and headers.
    constexpr std::array<std::size_t, 19> sizes{
      384, 512, 768, 1024, 1536, 2048, 3072, 4096, 8192, 12288,
      16384, 32768, 65536, 95760, 131072, 262144, 524288, 1048576, 96
    };
    for (auto center : sizes)
      for (std::size_t offset = 0; offset < 16; ++offset)
        for (int delta = -1; delta <= 1; ++delta)
          check(bytes(input).subspan(offset, std::size_t(std::ptrdiff_t(center) + delta)));
    // The generated incremental convention must agree with concatenation.
    for (std::size_t split = 0; split < 2048; split += 17) {
      auto first = everett::crc32c(bytes(input).first(split));
      check(bytes(input).subspan(split, 2048 - split), first);
      auto second = everett::crc32c(bytes(input).subspan(split, 2048 - split), first);
      require(second == everett::crc32c(bytes(input).first(2048)), "incremental CRC mismatch");
    }
  }

  void incremental_streams() {
    std::mt19937_64 random(0x73adc032);
    std::vector<std::byte> storage(196608 + 31);
    for (auto & byte : storage) byte = std::byte(random());
    constexpr std::array<std::uint32_t, 4> seeds{0, 0xffffffffu, 0x12345678u, 0x80000000u};
    constexpr std::array<std::size_t, 12> chunks{0, 1, 7, 96, 127, 128, 255, 256, 4096, 65535, 65536, 65537};
    for (std::size_t offset : {0u, 1u, 7u, 15u, 31u}) {
      auto input = bytes(storage).subspan(offset, 196608);
      for (auto seed : seeds) {
        auto expected = oracle(input, seed);
        require(everett::crc32c(input, seed) == expected, "large seeded public CRC mismatch");
        std::size_t at = 0;
        auto incremental = seed;
        for (auto size : chunks) {
          size = std::min(size, input.size() - at);
          incremental = everett::crc32c(input.subspan(at, size), incremental);
          at += size;
          require(everett::crc32c({}, incremental) == incremental, "empty chunk changed CRC state");
        }
        incremental = everett::crc32c(input.subspan(at), incremental);
        require(incremental == expected, "mixed-backend streaming CRC mismatch");
      }
    }
    // Every two-part split, including both empty endpoints, uses the public
    // finalized-state API and an independently seeded concatenation oracle.
    auto small = bytes(storage).subspan(3, 513);
    for (auto seed : seeds) {
      auto expected = oracle(small, seed);
      require(everett::crc32c({}, seed) == seed, "empty seeded CRC mismatch");
      for (std::size_t split = 0; split <= small.size(); ++split) {
        auto first = everett::crc32c(small.first(split), seed);
        auto second = everett::crc32c(small.subspan(split), first);
        require(second == expected, "all-split seeded CRC mismatch");
      }
    }
  }

#if defined(__unix__) || defined(__APPLE__)
  struct guarded_buffer {
    std::byte * address = nullptr;
    std::size_t page = 0;
    std::size_t extent = 0;
    guarded_buffer() {
      auto result = ::sysconf(_SC_PAGESIZE);
      require(result > 0, "page size unavailable");
      page = static_cast<std::size_t>(result);
      extent = 6 * page;
      auto mapping = ::mmap(nullptr, extent, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
      require(mapping != MAP_FAILED, "guard mapping failed");
      address = static_cast<std::byte *>(mapping);
      if (::mprotect(address + page, 4 * page, PROT_READ | PROT_WRITE) != 0) {
        ::munmap(address, extent);
        throw std::runtime_error("guard protection failed");
      }
      for (std::size_t i = 0; i < 4 * page; ++i) address[page + i] = std::byte(i * 137u);
    }
    guarded_buffer(guarded_buffer const &) = delete;
    ~guarded_buffer() { ::munmap(address, extent); }
    bytes body() const { return {address + page, 4 * page}; }
  };

  void protected_tails() {
    guarded_buffer buffer;
    auto body = buffer.body();
    // Direct calls to every available backend expose any hidden prefix/tail load.
    for (std::size_t size = 0; size < 768; ++size) {
      check(body.first(size));
      check(body.last(size), 0x91ba713du);
    }
    for (std::size_t size = 768; size <= body.size(); size += 193) check(body.last(size));
    check(body);
  }
#endif
}

int main() {
  known_vectors();
  combinations();
  boundaries();
  incremental_streams();
#if defined(__unix__) || defined(__APPLE__)
  protected_tails();
#endif
  std::cout << "CRC32C vectors, generated backends, alignments and bounded tails passed\n";
}
