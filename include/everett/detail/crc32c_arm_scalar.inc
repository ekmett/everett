/*
 * SPDX-FileCopyrightText: 2023 Peter Cawley
 * SPDX-License-Identifier: MIT OR Zlib
 * Generated from corsix/fast-crc32 13f5289ceb6065d014f9e1e64f161ea7a926c3b7.
 * Parameters: -i neon -p crc32c -a s1.
 * C++ adaptation: tools/generate_crc32c.py; do not edit by hand.
 * The upstream notices are in third_party/fast-crc32/LICENSE*.md
 * and the installed share/everett/third_party/fast-crc32 directory.
 */

namespace everett::crc32c_detail::arm_scalar {
#if defined(_MSC_VER)
#define EVERETT_GENERATED_CRC_AINLINE __forceinline
#define EVERETT_GENERATED_CRC_ALIGN(n) __declspec(align(n))
#else
#define EVERETT_GENERATED_CRC_AINLINE inline __attribute__((always_inline))
#define EVERETT_GENERATED_CRC_ALIGN(n) __attribute__((aligned(n)))
#endif
#define EVERETT_GENERATED_CRC_EXPORT inline

EVERETT_GENERATED_CRC_EXPORT uint32_t crc32_impl(uint32_t crc0, const char* buf, size_t len) {
  crc0 = ~crc0;
  for (; len && ((uintptr_t)buf & 7); --len) {
    crc0 = __crc32cb(crc0, *buf++);
  }
  for (; len >= 8; buf += 8, len -= 8) {
    crc0 = __crc32cd(crc0, load_little<uint64_t>(buf));
  }
  for (; len; --len) {
    crc0 = __crc32cb(crc0, *buf++);
  }
  return ~crc0;
}

#undef EVERETT_GENERATED_CRC_AINLINE
#undef EVERETT_GENERATED_CRC_ALIGN
#undef EVERETT_GENERATED_CRC_EXPORT
}
