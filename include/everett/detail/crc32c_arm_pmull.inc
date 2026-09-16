/*
 * SPDX-FileCopyrightText: 2023 Peter Cawley
 * SPDX-License-Identifier: MIT OR Zlib
 * Generated from corsix/fast-crc32 13f5289ceb6065d014f9e1e64f161ea7a926c3b7.
 * Parameters: -i neon -p crc32c -a v12_v1.
 * C++ adaptation: tools/generate_crc32c.py; do not edit by hand.
 * The upstream notices are in third_party/fast-crc32/LICENSE*.md
 * and the installed share/everett/third_party/fast-crc32 directory.
 */

namespace everett::crc32c_detail::arm_pmull {
#if defined(_MSC_VER)
#define EVERETT_GENERATED_CRC_AINLINE __forceinline
#define EVERETT_GENERATED_CRC_ALIGN(n) __declspec(align(n))
#else
#define EVERETT_GENERATED_CRC_AINLINE inline __attribute__((always_inline))
#define EVERETT_GENERATED_CRC_ALIGN(n) __attribute__((aligned(n)))
#endif
#define EVERETT_GENERATED_CRC_EXPORT inline

EVERETT_GENERATED_CRC_AINLINE uint64x2_t clmul_lo_e(uint64x2_t a, uint64x2_t b, uint64x2_t c) {
  uint64x2_t r;
  __asm("pmull %0.1q, %2.1d, %3.1d\neor %0.16b, %0.16b, %1.16b\n" : "=w"(r), "+w"(c) : "w"(a), "w"(b));
  return r;
}

EVERETT_GENERATED_CRC_AINLINE uint64x2_t clmul_hi_e(uint64x2_t a, uint64x2_t b, uint64x2_t c) {
  uint64x2_t r;
  __asm("pmull2 %0.1q, %2.2d, %3.2d\neor %0.16b, %0.16b, %1.16b\n" : "=w"(r), "+w"(c) : "w"(a), "w"(b));
  return r;
}

EVERETT_GENERATED_CRC_EXPORT uint32_t crc32_impl(uint32_t crc0, const char* buf, size_t len) {
  crc0 = ~crc0;
  for (; len && ((uintptr_t)buf & 7); --len) {
    crc0 = __crc32cb(crc0, *buf++);
  }
  if (((uintptr_t)buf & 8) && len >= 8) {
    crc0 = __crc32cd(crc0, load_little<uint64_t>(buf));
    buf += 8;
    len -= 8;
  }
  if (len >= 192) {
    /* First vector chunk. */
    uint64x2_t x0 = vld1q_u64((const uint64_t*)buf), y0;
    uint64x2_t x1 = vld1q_u64((const uint64_t*)(buf + 16)), y1;
    uint64x2_t x2 = vld1q_u64((const uint64_t*)(buf + 32)), y2;
    uint64x2_t x3 = vld1q_u64((const uint64_t*)(buf + 48)), y3;
    uint64x2_t x4 = vld1q_u64((const uint64_t*)(buf + 64)), y4;
    uint64x2_t x5 = vld1q_u64((const uint64_t*)(buf + 80)), y5;
    uint64x2_t x6 = vld1q_u64((const uint64_t*)(buf + 96)), y6;
    uint64x2_t x7 = vld1q_u64((const uint64_t*)(buf + 112)), y7;
    uint64x2_t x8 = vld1q_u64((const uint64_t*)(buf + 128)), y8;
    uint64x2_t x9 = vld1q_u64((const uint64_t*)(buf + 144)), y9;
    uint64x2_t x10 = vld1q_u64((const uint64_t*)(buf + 160)), y10;
    uint64x2_t x11 = vld1q_u64((const uint64_t*)(buf + 176)), y11;
    uint64x2_t k;
    { static const uint64_t EVERETT_GENERATED_CRC_ALIGN(16) k_[] = {0xa87ab8a8, 0xab7aff2a}; k = vld1q_u64(k_); }
    x0 = veorq_u64(uint64x2_t{crc0, 0}, x0);
    buf += 192;
    len -= 192;
    /* Main loop. */
    while (len >= 192) {
      y0 = clmul_lo_e(x0, k, vld1q_u64((const uint64_t*)buf)), x0 = clmul_hi_e(x0, k, y0);
      y1 = clmul_lo_e(x1, k, vld1q_u64((const uint64_t*)(buf + 16))), x1 = clmul_hi_e(x1, k, y1);
      y2 = clmul_lo_e(x2, k, vld1q_u64((const uint64_t*)(buf + 32))), x2 = clmul_hi_e(x2, k, y2);
      y3 = clmul_lo_e(x3, k, vld1q_u64((const uint64_t*)(buf + 48))), x3 = clmul_hi_e(x3, k, y3);
      y4 = clmul_lo_e(x4, k, vld1q_u64((const uint64_t*)(buf + 64))), x4 = clmul_hi_e(x4, k, y4);
      y5 = clmul_lo_e(x5, k, vld1q_u64((const uint64_t*)(buf + 80))), x5 = clmul_hi_e(x5, k, y5);
      y6 = clmul_lo_e(x6, k, vld1q_u64((const uint64_t*)(buf + 96))), x6 = clmul_hi_e(x6, k, y6);
      y7 = clmul_lo_e(x7, k, vld1q_u64((const uint64_t*)(buf + 112))), x7 = clmul_hi_e(x7, k, y7);
      y8 = clmul_lo_e(x8, k, vld1q_u64((const uint64_t*)(buf + 128))), x8 = clmul_hi_e(x8, k, y8);
      y9 = clmul_lo_e(x9, k, vld1q_u64((const uint64_t*)(buf + 144))), x9 = clmul_hi_e(x9, k, y9);
      y10 = clmul_lo_e(x10, k, vld1q_u64((const uint64_t*)(buf + 160))), x10 = clmul_hi_e(x10, k, y10);
      y11 = clmul_lo_e(x11, k, vld1q_u64((const uint64_t*)(buf + 176))), x11 = clmul_hi_e(x11, k, y11);
      buf += 192;
      len -= 192;
    }
    /* Reduce x0 ... x11 to just x0. */
    { static const uint64_t EVERETT_GENERATED_CRC_ALIGN(16) k_[] = {0xf20c0dfe, 0x493c7d27}; k = vld1q_u64(k_); }
    y0 = clmul_lo_e(x0, k, x1), x0 = clmul_hi_e(x0, k, y0);
    y2 = clmul_lo_e(x2, k, x3), x2 = clmul_hi_e(x2, k, y2);
    y4 = clmul_lo_e(x4, k, x5), x4 = clmul_hi_e(x4, k, y4);
    y6 = clmul_lo_e(x6, k, x7), x6 = clmul_hi_e(x6, k, y6);
    y8 = clmul_lo_e(x8, k, x9), x8 = clmul_hi_e(x8, k, y8);
    y10 = clmul_lo_e(x10, k, x11), x10 = clmul_hi_e(x10, k, y10);
    { static const uint64_t EVERETT_GENERATED_CRC_ALIGN(16) k_[] = {0x3da6d0cb, 0xba4fc28e}; k = vld1q_u64(k_); }
    y0 = clmul_lo_e(x0, k, x2), x0 = clmul_hi_e(x0, k, y0);
    y4 = clmul_lo_e(x4, k, x6), x4 = clmul_hi_e(x4, k, y4);
    y8 = clmul_lo_e(x8, k, x10), x8 = clmul_hi_e(x8, k, y8);
    { static const uint64_t EVERETT_GENERATED_CRC_ALIGN(16) k_[] = {0x740eef02, 0x9e4addf8}; k = vld1q_u64(k_); }
    y0 = clmul_lo_e(x0, k, x4), x0 = clmul_hi_e(x0, k, y0);
    x4 = x8;
    y0 = clmul_lo_e(x0, k, x4), x0 = clmul_hi_e(x0, k, y0);
    /* Reduce 128 bits to 32 bits, and multiply by x^32. */
    crc0 = __crc32cd(0, vgetq_lane_u64(x0, 0));
    crc0 = __crc32cd(crc0, vgetq_lane_u64(x0, 1));
  }
  if (len >= 16) {
    /* First vector chunk. */
    uint64x2_t x0 = vld1q_u64((const uint64_t*)buf), y0;
    uint64x2_t k;
    { static const uint64_t EVERETT_GENERATED_CRC_ALIGN(16) k_[] = {0xf20c0dfe, 0x493c7d27}; k = vld1q_u64(k_); }
    x0 = veorq_u64(uint64x2_t{crc0, 0}, x0);
    buf += 16;
    len -= 16;
    /* Main loop. */
    while (len >= 16) {
      y0 = clmul_lo_e(x0, k, vld1q_u64((const uint64_t*)buf)), x0 = clmul_hi_e(x0, k, y0);
      buf += 16;
      len -= 16;
    }
    /* Reduce 128 bits to 32 bits, and multiply by x^32. */
    crc0 = __crc32cd(0, vgetq_lane_u64(x0, 0));
    crc0 = __crc32cd(crc0, vgetq_lane_u64(x0, 1));
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
