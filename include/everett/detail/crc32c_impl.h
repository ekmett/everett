// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
#pragma once

// Pinned third-party generated kernels are private compiled implementation inputs.
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <simd/attributes.h>

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32) && (defined(__GNUC__) || defined(__clang__))
#include <arm_acle.h>
#if (defined(__ARM_FEATURE_CRYPTO) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#include <arm_neon.h>
#endif
#elif defined(__x86_64__) && defined(__SSE4_2__)
#include <nmmintrin.h>
#if defined(__PCLMUL__)
#include <immintrin.h>
#endif
#endif

namespace everett::crc32c_detail {
  // Reflected polynomial representation: bit31 is the multiplicative unit,
  // and one rightward step multiplies by x modulo the Castagnoli polynomial.
  constexpr std::uint32_t multiply_polynomial(std::uint32_t a, std::uint32_t b) noexcept {
    std::uint32_t result = 0;
    for (std::uint32_t bit = 1u << 31; bit; bit >>= 1) {
      if (a & bit) result ^= b;
      b = (b >> 1) ^ (0x82f63b78u & (0u - (b & 1)));
    }
    return result;
  }
  inline constexpr auto byte_powers = [] {
    std::array<std::uint32_t, 64> result{};
    result[0] = 1u << 23; // x^8
    for (unsigned i = 1; i != result.size(); ++i)
      result[i] = multiply_polynomial(result[i - 1], result[i - 1]);
    return result;
  }();
  inline std::uint32_t shift(std::uint32_t crc, std::uint64_t bytes) noexcept {
    for (unsigned i = 0; bytes; ++i, bytes >>= 1)
      if (bytes & 1) crc = multiply_polynomial(byte_powers[i], crc);
    return crc;
  }

  // Generated C kernels load scalar words through this C++ alias-safe bridge.
  // Exactly sizeof(T) bytes are read, including unaligned and mmap-tail inputs.
  template <class T> inline T load_little(void const * bytes) noexcept {
    T value;
    std::memcpy(&value, bytes, sizeof(T));
    if constexpr (std::endian::native == std::endian::big) {
      T reversed = 0;
      for (unsigned i = 0; i < sizeof(T); ++i) {
        reversed = T((reversed << 8) | (value & 255u));
        value >>= 8;
      }
      return reversed;
    } else return value;
  }
}

#include <everett/detail/crc32c_portable.inc>

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32) && (defined(__GNUC__) || defined(__clang__))
#include <everett/detail/crc32c_arm_scalar.inc>
#if (defined(__ARM_FEATURE_CRYPTO) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#include <everett/detail/crc32c_arm_pmull.inc>
#if defined(__ARM_FEATURE_SHA3)
#include <everett/detail/crc32c_arm_eor3.inc>
#endif
#endif
#elif defined(__x86_64__) && defined(__SSE4_2__)
#include <everett/detail/crc32c_x86_scalar.inc>
#if defined(__PCLMUL__)
#include <everett/detail/crc32c_x86_pclmul.inc>
#if defined(__AVX512F__) && defined(__AVX512VL__)
#include <everett/detail/crc32c_x86_avx512.inc>
#if defined(__VPCLMULQDQ__)
#include <everett/detail/crc32c_x86_vpclmul.inc>
#endif
#endif
#endif
#endif
