// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
// Author: Edward Kmett
// Packed 512-bit rank directory and shader query/build helpers.
// Rank/select design informed by Edward Kmett's 2015 VR work; the associated
// notice is retained in rank512_notice.txt. This copy uses Everett's dual license.
#pragma once

// Packed 512-bit blocks: one absolute rank and seven 9-bit relative pair ranks.
// All offsets are bytes.
uint rank_words(uint bits) { return (bits + 31) / 32; }
uint rank_blocks(uint bits) { return (bits + 511) / 512; }
uint rank_total_offset(uint bits) { return rank_words(bits) * 4 + rank_blocks(bits) * 12; }

groupshared uint rank_wave_sums[256];
// All 256 lanes of exactly one thread group must call this utility together.
uint rank_group_prefix(uint value, uint tid, out uint total) {
  uint width = WaveGetLaneCount(), wave = tid / width;
  uint prefix = WavePrefixSum(value), sum = WaveActiveSum(value);
  if (WaveIsFirstLane())
    rank_wave_sums[wave] = sum;
  GroupMemoryBarrierWithGroupSync();
  total = 0;
  for (uint i = 0; i < (256 + width - 1) / width; ++i) {
    uint n = rank_wave_sums[i];
    if (i < wave)
      prefix += n;
    total += n;
  }
  GroupMemoryBarrierWithGroupSync();
  return prefix;
}

// Exactly one 256-thread group; tid is its group-local lane 0..255.
// The collision experiment instead uses parallel count/scan/finish kernels.
void rank_build(RWByteAddressBuffer index, uint bits, uint tid) {
  uint words = rank_words(bits), blocks = rank_blocks(bits), offset = 0;
  if (tid == 0 && bits % 32) {
    uint last = (words - 1) * 4;
    index.Store(last, index.Load(last) & ((1u << (bits % 32)) - 1));
  }
  DeviceMemoryBarrierWithGroupSync();
  for (uint batch = 0; batch < blocks; batch += 256) {
    uint block = batch + tid, local = 0;
    uint2 packed = 0;
    if (block < blocks) {
      for (uint pair = 0; pair < 8; ++pair) {
        if (pair) {
          uint shift = (pair - 1) * 9;
          if (shift < 32) {
            packed.x |= local << shift;
            if (shift) packed.y |= local >> (32 - shift);
          } else {
            packed.y |= local << (shift - 32);
          }
        }
        for (uint j = 0; j < 2; ++j) {
          uint word = block * 16 + pair * 2 + j;
          if (word < words)
            local += countbits(index.Load(word * 4));
        }
      }
    }
    uint total;
    uint prefix = rank_group_prefix(local, tid, total);
    if (block < blocks)
      index.Store3(words * 4 + block * 12,
                   uint3(offset + prefix, packed.x, packed.y));
    offset += total;
  }
  if (tid == 0)
    index.Store(rank_total_offset(bits), offset);
  DeviceMemoryBarrierWithGroupSync();
}

template <typename Buffer> uint rank_prefix(Buffer index, uint bits, uint i) {
  if (i >= bits)
    return index.Load(rank_total_offset(bits));
  uint3 block = index.Load3(rank_words(bits) * 4 + (i / 512) * 12);
  uint pair = (i / 64) % 8;
  // Same seven packed 9-bit ranks, with defined 32-bit shifts on every backend.
  uint relative = 0;
  if (pair) {
    uint shift = (pair - 1) * 9;
    if (shift < 32) {
      relative = block.y >> shift;
      if (shift) relative |= block.z << (32 - shift);
    } else {
      relative = block.z >> (shift - 32);
    }
    relative &= 511;
  }
  uint prefix = block.x + relative;
  if (i & 32)
    prefix += countbits(index.Load((i / 64) * 8));
  return prefix + countbits(index.Load((i / 32) * 4) & ((1u << (i % 32)) - 1));
}

void rank_export_word(RWByteAddressBuffer index, uint bits, uint ids_offset, uint word) {
  uint pending = index.Load(word * 4), output = rank_prefix(index, bits, word * 32);
  while (pending) {
    index.Store(ids_offset + output * 4, word * 32 + firstbitlow(pending));
    ++output;
    pending &= pending - 1;
  }
}
