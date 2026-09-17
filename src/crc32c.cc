/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Compiles the pinned CRC32C kernels behind Everett's module interface.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/crc32c.h>
#include <everett/detail/crc32c_impl.h>

namespace everett {
  // Combine finalized CRCs of A and B into CRC(A || B) without rereading either
  // input. second_bytes is B's physical byte length. The CRC of empty B is 0.
  // Lengths span all 64 bits; no bytes-to-bits multiplication can overflow.
  std::uint32_t crc32c_combine(std::uint32_t first, std::uint32_t second,
                                    std::uint64_t second_bytes) noexcept {
    return crc32c_detail::shift(first, second_bytes) ^ second;
  }

  // Reflected Castagnoli CRC32C, with the conventional initial/final complement.
  // Pinned Corsix-generated kernels are selected by the compiler target, without
  // runtime feature probes, allocation, or instructions beyond that target.
  // previous_crc is the finalized result of the preceding chunk, or zero for
  // a new stream. Empty chunks preserve it; callers do not invert the state.
  std::uint32_t crc32c(std::span<std::byte const> bytes,
                              std::uint32_t previous_crc) noexcept {
    auto data = reinterpret_cast<char const *>(bytes.data());
    auto size = bytes.size();
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32) && (defined(__GNUC__) || defined(__clang__))
    if (size < 128) return crc32c_detail::arm_scalar::crc32_impl(previous_crc, data, size);
#if (defined(__ARM_FEATURE_CRYPTO) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__) && defined(__ARM_FEATURE_SHA3)
    // The fused kernel amortizes its fold setup on larger buffers; PMULL has
    // lower setup cost for page-sized inputs. Threshold measured on Apple M2.
    if (size >= 65536) return crc32c_detail::arm_eor3::crc32_impl(previous_crc, data, size);
    return crc32c_detail::arm_pmull::crc32_impl(previous_crc, data, size);
#elif (defined(__ARM_FEATURE_CRYPTO) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    return crc32c_detail::arm_pmull::crc32_impl(previous_crc, data, size);
#else
    return crc32c_detail::arm_scalar::crc32_impl(previous_crc, data, size);
#endif
#elif defined(__x86_64__) && defined(__SSE4_2__)
    if (size < 256) return crc32c_detail::x86_scalar::crc32_impl(previous_crc, data, size);
#if defined(__PCLMUL__) && defined(__AVX512F__) && defined(__AVX512VL__) && defined(__VPCLMULQDQ__)
    return crc32c_detail::x86_vpclmul::crc32_impl(previous_crc, data, size);
#elif defined(__PCLMUL__) && defined(__AVX512F__) && defined(__AVX512VL__)
    return crc32c_detail::x86_avx512::crc32_impl(previous_crc, data, size);
#elif defined(__PCLMUL__)
    return crc32c_detail::x86_pclmul::crc32_impl(previous_crc, data, size);
#else
    return crc32c_detail::x86_scalar::crc32_impl(previous_crc, data, size);
#endif
#else
    return crc32c_detail::portable::crc32_impl(previous_crc, data, size);
#endif
  }
}
