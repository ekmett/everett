// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
// Function body compiled separately for each admitted execution profile.
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
