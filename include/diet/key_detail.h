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
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace diet::key_detail {
  // All loads remain within count bytes; neither alignment nor padding is assumed.
  inline std::size_t common_bytes(void const * lhs, void const * rhs, std::size_t count) noexcept {
    auto a = static_cast<unsigned char const *>(lhs);
    auto b = static_cast<unsigned char const *>(rhs);
    std::size_t at = 0;
#if defined(__aarch64__) && defined(__ARM_NEON)
    for (; count - at >= 16; at += 16)
      if (vmaxvq_u8(veorq_u8(vld1q_u8(a + at), vld1q_u8(b + at)))) break;
#elif defined(__SSE2__)
    for (; count - at >= 16; at += 16) {
      auto x = _mm_loadu_si128(reinterpret_cast<__m128i const *>(a + at));
      auto y = _mm_loadu_si128(reinterpret_cast<__m128i const *>(b + at));
      auto different = unsigned(_mm_movemask_epi8(_mm_cmpeq_epi8(x, y))) ^ 65535u;
      if (different) return at + std::countr_zero(different);
    }
#endif
    for (; count - at >= 8; at += 8) {
      std::uint64_t x, y;
      std::memcpy(&x, a + at, 8); std::memcpy(&y, b + at, 8);
      if (auto different = x ^ y) {
        if constexpr (std::endian::native == std::endian::little) return at + (std::countr_zero(different) >> 3);
        else if constexpr (std::endian::native == std::endian::big) return at + (std::countl_zero(different) >> 3);
        else break;
      }
    }
    while (at != count && a[at] == b[at]) ++at;
    return at;
  }

  inline std::uint64_t reverse_bytes(std::uint64_t x) noexcept {
    x = ((x & 0x00ff00ff00ff00ffull) << 8) | ((x >> 8) & 0x00ff00ff00ff00ffull);
    x = ((x & 0x0000ffff0000ffffull) << 16) | ((x >> 16) & 0x0000ffff0000ffffull);
    return (x << 32) | (x >> 32);
  }
  inline std::uint64_t load_big(void const * source) noexcept {
    if constexpr (std::endian::native == std::endian::little || std::endian::native == std::endian::big) {
      std::uint64_t value; std::memcpy(&value, source, 8);
      if constexpr (std::endian::native == std::endian::little) return reverse_bytes(value);
      else return value;
    } else {
      auto bytes = static_cast<unsigned char const *>(source);
      std::uint64_t value = 0;
      for (unsigned i = 0; i != 8; ++i) value = (value << 8) | bytes[i];
      return value;
    }
  }
  inline void store_big(void * target, std::uint64_t value) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
      value = reverse_bytes(value); std::memcpy(target, &value, 8);
    } else if constexpr (std::endian::native == std::endian::big) std::memcpy(target, &value, 8);
    else {
      auto bytes = static_cast<unsigned char *>(target);
      for (unsigned i = 0; i != 8; ++i) bytes[i] = static_cast<unsigned char>(value >> (56 - (i << 3)));
    }
  }
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Provides bounded byte comparison shared by the key codecs.
 */
