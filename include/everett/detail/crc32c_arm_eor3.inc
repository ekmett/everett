/*
 * SPDX-FileCopyrightText: 2023 Peter Cawley
 * SPDX-License-Identifier: MIT OR Zlib
 * Generated from corsix/fast-crc32 13f5289ceb6065d014f9e1e64f161ea7a926c3b7.
 * Parameters: -i neon_eor3 -p crc32c -a v9s3x2_s3.
 * C++ adaptation: tools/generate_crc32c.py; do not edit by hand.
 * The upstream notices are in third_party/fast-crc32/LICENSE*.md
 * and the installed share/everett/third_party/fast-crc32 directory.
 */

namespace everett::crc32c_detail::arm_eor3 {
#define EVERETT_GENERATED_CRC_EXPORT inline

simd_inline uint64x2_t clmul_lo(uint64x2_t a, uint64x2_t b) {
  uint64x2_t r;
  __asm("pmull %0.1q, %1.1d, %2.1d\n" : "=w"(r) : "w"(a), "w"(b));
  return r;
}

simd_inline uint64x2_t clmul_hi(uint64x2_t a, uint64x2_t b) {
  uint64x2_t r;
  __asm("pmull2 %0.1q, %1.2d, %2.2d\n" : "=w"(r) : "w"(a), "w"(b));
  return r;
}

simd_inline uint64x2_t clmul_scalar(uint32_t a, uint32_t b) {
  uint64x2_t r;
  __asm("pmull %0.1q, %1.1d, %2.1d\n" : "=w"(r) : "w"(vmovq_n_u64(a)), "w"(vmovq_n_u64(b)));
  return r;
}

inline uint32_t xnmodp(uint64_t n) /* x^n mod P, in log(n) time */ {
  uint64_t stack = ~(uint64_t)1;
  uint32_t acc, low;
  for (; n > 191; n = (n >> 1) - 16) {
    stack = (stack << 1) + (n & 1);
  }
  stack = ~stack;
  acc = ((uint32_t)0x80000000) >> (n & 31);
  for (n >>= 5; n; --n) {
    acc = __crc32cw(acc, 0);
  }
  while ((low = stack & 1), stack >>= 1) {
    poly8x8_t x = vreinterpret_p8_u64(vmov_n_u64(acc));
    uint64_t y = vgetq_lane_u64(vreinterpretq_u64_p16(vmull_p8(x, x)), 0);
    acc = __crc32cd(0, y << low);
  }
  return acc;
}

simd_inline uint64x2_t crc_shift(uint32_t crc, size_t nbytes) {
  return clmul_scalar(crc, xnmodp(nbytes * 8 - 33));
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
    size_t blk = (len - 0) / 192;
    size_t klen = blk * 16;
    const char* buf2 = buf + klen * 3;
    uint32_t crc1 = 0;
    uint32_t crc2 = 0;
    uint64x2_t vc0;
    uint64x2_t vc1;
    uint64x2_t vc2;
    uint64_t vc;
    /* First vector chunk. */
    uint64x2_t x0 = vld1q_u64((const uint64_t*)buf2), y0;
    uint64x2_t x1 = vld1q_u64((const uint64_t*)(buf2 + 16)), y1;
    uint64x2_t x2 = vld1q_u64((const uint64_t*)(buf2 + 32)), y2;
    uint64x2_t x3 = vld1q_u64((const uint64_t*)(buf2 + 48)), y3;
    uint64x2_t x4 = vld1q_u64((const uint64_t*)(buf2 + 64)), y4;
    uint64x2_t x5 = vld1q_u64((const uint64_t*)(buf2 + 80)), y5;
    uint64x2_t x6 = vld1q_u64((const uint64_t*)(buf2 + 96)), y6;
    uint64x2_t x7 = vld1q_u64((const uint64_t*)(buf2 + 112)), y7;
    uint64x2_t x8 = vld1q_u64((const uint64_t*)(buf2 + 128)), y8;
    uint64x2_t k;
    { static const uint64_t simd_align(16) k_[] = {0x7e908048, 0xc96cfdc0}; k = vld1q_u64(k_); }
    buf2 += 144;
    len -= 192;
    /* Main loop. */
    while (len >= 192) {
      y0 = clmul_lo(x0, k), x0 = clmul_hi(x0, k);
      y1 = clmul_lo(x1, k), x1 = clmul_hi(x1, k);
      y2 = clmul_lo(x2, k), x2 = clmul_hi(x2, k);
      y3 = clmul_lo(x3, k), x3 = clmul_hi(x3, k);
      y4 = clmul_lo(x4, k), x4 = clmul_hi(x4, k);
      y5 = clmul_lo(x5, k), x5 = clmul_hi(x5, k);
      y6 = clmul_lo(x6, k), x6 = clmul_hi(x6, k);
      y7 = clmul_lo(x7, k), x7 = clmul_hi(x7, k);
      y8 = clmul_lo(x8, k), x8 = clmul_hi(x8, k);
      x0 = veor3q_u64(x0, y0, vld1q_u64((const uint64_t*)buf2));
      x1 = veor3q_u64(x1, y1, vld1q_u64((const uint64_t*)(buf2 + 16)));
      x2 = veor3q_u64(x2, y2, vld1q_u64((const uint64_t*)(buf2 + 32)));
      x3 = veor3q_u64(x3, y3, vld1q_u64((const uint64_t*)(buf2 + 48)));
      x4 = veor3q_u64(x4, y4, vld1q_u64((const uint64_t*)(buf2 + 64)));
      x5 = veor3q_u64(x5, y5, vld1q_u64((const uint64_t*)(buf2 + 80)));
      x6 = veor3q_u64(x6, y6, vld1q_u64((const uint64_t*)(buf2 + 96)));
      x7 = veor3q_u64(x7, y7, vld1q_u64((const uint64_t*)(buf2 + 112)));
      x8 = veor3q_u64(x8, y8, vld1q_u64((const uint64_t*)(buf2 + 128)));
      crc0 = __crc32cd(crc0, load_little<uint64_t>(buf));
      crc1 = __crc32cd(crc1, load_little<uint64_t>((buf + klen)));
      crc2 = __crc32cd(crc2, load_little<uint64_t>((buf + klen * 2)));
      crc0 = __crc32cd(crc0, load_little<uint64_t>((buf + 8)));
      crc1 = __crc32cd(crc1, load_little<uint64_t>((buf + klen + 8)));
      crc2 = __crc32cd(crc2, load_little<uint64_t>((buf + klen * 2 + 8)));
      buf += 16;
      buf2 += 144;
      len -= 192;
    }
    /* Reduce x0 ... x8 to just x0. */
    { static const uint64_t simd_align(16) k_[] = {0xf20c0dfe, 0x493c7d27}; k = vld1q_u64(k_); }
    y0 = clmul_lo(x0, k), x0 = clmul_hi(x0, k);
    x0 = veor3q_u64(x0, y0, x1);
    x1 = x2, x2 = x3, x3 = x4, x4 = x5, x5 = x6, x6 = x7, x7 = x8;
    y0 = clmul_lo(x0, k), x0 = clmul_hi(x0, k);
    y2 = clmul_lo(x2, k), x2 = clmul_hi(x2, k);
    y4 = clmul_lo(x4, k), x4 = clmul_hi(x4, k);
    y6 = clmul_lo(x6, k), x6 = clmul_hi(x6, k);
    x0 = veor3q_u64(x0, y0, x1);
    x2 = veor3q_u64(x2, y2, x3);
    x4 = veor3q_u64(x4, y4, x5);
    x6 = veor3q_u64(x6, y6, x7);
    { static const uint64_t simd_align(16) k_[] = {0x3da6d0cb, 0xba4fc28e}; k = vld1q_u64(k_); }
    y0 = clmul_lo(x0, k), x0 = clmul_hi(x0, k);
    y4 = clmul_lo(x4, k), x4 = clmul_hi(x4, k);
    x0 = veor3q_u64(x0, y0, x2);
    x4 = veor3q_u64(x4, y4, x6);
    { static const uint64_t simd_align(16) k_[] = {0x740eef02, 0x9e4addf8}; k = vld1q_u64(k_); }
    y0 = clmul_lo(x0, k), x0 = clmul_hi(x0, k);
    x0 = veor3q_u64(x0, y0, x4);
    /* Final scalar chunk. */
    crc0 = __crc32cd(crc0, load_little<uint64_t>(buf));
    crc1 = __crc32cd(crc1, load_little<uint64_t>((buf + klen)));
    crc2 = __crc32cd(crc2, load_little<uint64_t>((buf + klen * 2)));
    crc0 = __crc32cd(crc0, load_little<uint64_t>((buf + 8)));
    crc1 = __crc32cd(crc1, load_little<uint64_t>((buf + klen + 8)));
    crc2 = __crc32cd(crc2, load_little<uint64_t>((buf + klen * 2 + 8)));
    vc0 = crc_shift(crc0, klen * 2 + blk * 144);
    vc1 = crc_shift(crc1, klen + blk * 144);
    vc2 = crc_shift(crc2, 0 + blk * 144);
    vc = vgetq_lane_u64(veor3q_u64(vc0, vc1, vc2), 0);
    /* Reduce 128 bits to 32 bits, and multiply by x^32. */
    crc0 = __crc32cd(0, vgetq_lane_u64(x0, 0));
    crc0 = __crc32cd(crc0, vc ^ vgetq_lane_u64(x0, 1));
    buf = buf2;
  }
  if (len >= 32) {
    size_t klen = ((len - 8) / 24) * 8;
    uint32_t crc1 = 0;
    uint32_t crc2 = 0;
    uint64x2_t vc0;
    uint64x2_t vc1;
    uint64_t vc;
    /* Main loop. */
    do {
      crc0 = __crc32cd(crc0, load_little<uint64_t>(buf));
      crc1 = __crc32cd(crc1, load_little<uint64_t>((buf + klen)));
      crc2 = __crc32cd(crc2, load_little<uint64_t>((buf + klen * 2)));
      buf += 8;
      len -= 24;
    } while (len >= 32);
    vc0 = crc_shift(crc0, klen * 2 + 8);
    vc1 = crc_shift(crc1, klen + 8);
    vc = vgetq_lane_u64(veorq_u64(vc0, vc1), 0);
    /* Final 8 bytes. */
    buf += klen * 2;
    crc0 = crc2;
    crc0 = __crc32cd(crc0, load_little<uint64_t>(buf) ^ vc), buf += 8;
    len -= 8;
  }
  for (; len >= 8; buf += 8, len -= 8) {
    crc0 = __crc32cd(crc0, load_little<uint64_t>(buf));
  }
  for (; len; --len) {
    crc0 = __crc32cb(crc0, *buf++);
  }
  return ~crc0;
}

#undef EVERETT_GENERATED_CRC_EXPORT
}
