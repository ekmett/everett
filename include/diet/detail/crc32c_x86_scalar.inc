/*
 * SPDX-FileCopyrightText: 2023 Peter Cawley
 * SPDX-License-Identifier: MIT OR Zlib
 * Generated from corsix/fast-crc32 13f5289ceb6065d014f9e1e64f161ea7a926c3b7.
 * Parameters: -i sse -p crc32c -a s1.
 * C++ adaptation: tools/generate_crc32c.py; do not edit by hand.
 * The upstream notices are in third_party/fast-crc32/LICENSE*.md
 * and the installed share/diet/third_party/fast-crc32 directory.
 */

namespace diet::crc32c_detail::x86_scalar {
#if defined(_MSC_VER)
#define DIET_GENERATED_CRC_AINLINE __forceinline
#define DIET_GENERATED_CRC_ALIGN(n) __declspec(align(n))
#else
#define DIET_GENERATED_CRC_AINLINE inline __attribute__((always_inline))
#define DIET_GENERATED_CRC_ALIGN(n) __attribute__((aligned(n)))
#endif
#define DIET_GENERATED_CRC_EXPORT inline

DIET_GENERATED_CRC_EXPORT uint32_t crc32_impl(uint32_t crc0, const char* buf, size_t len) {
  crc0 = ~crc0;
  for (; len && ((uintptr_t)buf & 7); --len) {
    crc0 = _mm_crc32_u8(crc0, *buf++);
  }
  for (; len >= 8; buf += 8, len -= 8) {
    crc0 = _mm_crc32_u64(crc0, load_little<uint64_t>(buf));
  }
  for (; len; --len) {
    crc0 = _mm_crc32_u8(crc0, *buf++);
  }
  return ~crc0;
}

#undef DIET_GENERATED_CRC_AINLINE
#undef DIET_GENERATED_CRC_ALIGN
#undef DIET_GENERATED_CRC_EXPORT
}
