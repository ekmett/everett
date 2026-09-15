/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

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

namespace everett {
  // Reflected Castagnoli CRC32C, with the conventional initial/final complement.
  // Pinned Corsix-generated kernels are selected by the compiler target, without
  // runtime feature probes, allocation, or instructions beyond that target.
  // Their API already complements the initial and final state: pass zero here.
  inline std::uint32_t crc32c(std::span<std::byte const> bytes) noexcept {
    auto data = reinterpret_cast<char const *>(bytes.data());
    auto size = bytes.size();
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32) && (defined(__GNUC__) || defined(__clang__))
    if (size < 128) return crc32c_detail::arm_scalar::crc32_impl(0, data, size);
#if (defined(__ARM_FEATURE_CRYPTO) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__) && defined(__ARM_FEATURE_SHA3)
    // The fused kernel amortizes its fold setup on larger buffers; PMULL has
    // lower setup cost for page-sized inputs. Threshold measured on Apple M2.
    if (size >= 65536) return crc32c_detail::arm_eor3::crc32_impl(0, data, size);
    return crc32c_detail::arm_pmull::crc32_impl(0, data, size);
#elif (defined(__ARM_FEATURE_CRYPTO) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    return crc32c_detail::arm_pmull::crc32_impl(0, data, size);
#else
    return crc32c_detail::arm_scalar::crc32_impl(0, data, size);
#endif
#elif defined(__x86_64__) && defined(__SSE4_2__)
    if (size < 256) return crc32c_detail::x86_scalar::crc32_impl(0, data, size);
#if defined(__PCLMUL__) && defined(__AVX512F__) && defined(__AVX512VL__) && defined(__VPCLMULQDQ__)
    return crc32c_detail::x86_vpclmul::crc32_impl(0, data, size);
#elif defined(__PCLMUL__) && defined(__AVX512F__) && defined(__AVX512VL__)
    return crc32c_detail::x86_avx512::crc32_impl(0, data, size);
#elif defined(__PCLMUL__)
    return crc32c_detail::x86_pclmul::crc32_impl(0, data, size);
#else
    return crc32c_detail::x86_scalar::crc32_impl(0, data, size);
#endif
#else
    return crc32c_detail::portable::crc32_impl(0, data, size);
#endif
  }
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Everett's CRC32C support.
 */
