// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
// Optional bounded GPU construction kernels. uint counts deliberately cap inputs.
[[vk::binding(0)]] StructuredBuffer<uint> input_data;
[[vk::binding(1)]] RWStructuredBuffer<uint> output_data;
[[vk::binding(2)]] RWStructuredBuffer<uint> totals;
[[vk::binding(3)]] StructuredBuffer<uint> parameters;
[[vk::binding(4)]] StructuredBuffer<uint> extra;

groupshared uint scan_values[256];
[numthreads(256, 1, 1)] void scan_blocks(uint3 tid : SV_DispatchThreadID,
                                         uint3 local : SV_GroupThreadID, uint3 group : SV_GroupID) {
  uint n = parameters[0], i = tid.x, x = i < n ? input_data[i] : 0;
  scan_values[local.x] = x;
  GroupMemoryBarrierWithGroupSync();
  for (uint offset = 1; offset < 256; offset *= 2) {
    uint add = local.x >= offset ? scan_values[local.x - offset] : 0;
    GroupMemoryBarrierWithGroupSync();
    scan_values[local.x] += add;
    GroupMemoryBarrierWithGroupSync();
  }
  if (i < n)
    output_data[i] = scan_values[local.x] - x;
  if (local.x == 255)
    totals[group.x] = scan_values[255];
}[numthreads(256, 1, 1)] void scan_add(uint3 tid : SV_DispatchThreadID) {
  if (tid.x < parameters[0])
    output_data[tid.x] += input_data[tid.x / 256];
}
uint source_word(uint at, uint bits) {
  if (at >= (bits + 31) / 32)
    return 0;
  uint x = input_data[at];
  uint remain = bits - at * 32;
  return remain < 32 ? x & ((1u << remain) - 1) : x;
}
[numthreads(128, 1, 1)] void rank_count(uint3 tid : SV_DispatchThreadID) {
  uint block = tid.x, bits = parameters[0];
  if (block >= (bits + 2047) / 2048)
    return;
  uint4 c = 0;
  for (uint j = 0; j < 64; j++)
    c[j / 16] += countbits(source_word(block * 64 + j, bits));
  output_data[block * 2 + 1] = c.x | (c.y << 11) | (c.z << 22);
  totals[block] = c.x + c.y + c.z + c.w;
}[numthreads(128, 1, 1)] void rank_finish(uint3 tid : SV_DispatchThreadID) {
  if (tid.x < parameters[0])
    output_data[tid.x * 2] = input_data[tid.x];
}
uint population15(uint first, uint bits) {
  uint word = first / 32, shift = first % 32;
  uint x = source_word(word, bits) >> shift;
  if (shift > 17)
    x |= source_word(word + 1, bits) << (32 - shift);
  return countbits(x & 32767);
}
[numthreads(128, 1, 1)] void rank15_count(uint3 tid : SV_DispatchThreadID) {
  uint block = tid.x, bits = parameters[0], groups = (bits + 14) / 15;
  if (block >= (groups + 127) / 128)
    return;
  uint total = 0;
  for (uint packed = 0; packed < 16; packed++) {
    uint value = 0;
    for (uint nibble = 0; nibble < 8; nibble++) {
      uint group = block * 128 + packed * 8 + nibble;
      uint c = group < groups ? population15(group * 15, bits) : 0;
      value |= c << (nibble * 4);
      total += c;
    }
    if (block * 16 + packed < ((groups + 15) / 16) * 2)
      output_data[block * 16 + packed] = value;
  }
  totals[block] = total;
}[numthreads(128, 1, 1)] void rank15_finish(uint3 tid : SV_DispatchThreadID) {
  if (tid.x < parameters[0]) {
    output_data[tid.x * 2] = input_data[tid.x];
    output_data[tid.x * 2 + 1] = 0;
  }
}
[numthreads(128, 1, 1)] void probe_copy(uint3 tid : SV_DispatchThreadID) {
  if (tid.x < parameters[0])
    output_data[tid.x] = input_data[tid.x] ^ 0x5a5a5a5a;
}

// Merge resources: byte arena=0, output=1, scratch/counts=2, parameters=3,
// four-u32 descriptors=4, record references=5, frame metadata=6.
[[vk::binding(5)]] StructuredBuffer<uint> references;
[[vk::binding(6)]] RWStructuredBuffer<uint> frames;
[[vk::binding(7)]] StructuredBuffer<uint> other_data;
[[vk::binding(8)]] StructuredBuffer<uint> prefix_data;
[[vk::binding(9)]] StructuredBuffer<uint> prefix_tree;
#include "ef_output.hlsl"
#include "index_rank.hlsl"
#include "parse_input.hlsl"
#ifdef COMPRESSED_INPUT
#include "prefix_owner.hlsl"
#ifdef PREFIX_CACHE
#include "prefix_cache.hlsl"
#include "collision_rank.hlsl"
#endif
#define RECORD_WORDS 8
uint shared_prefix_bytes() { return prefix_data[0]; }
#else
#define RECORD_WORDS 4
uint shared_prefix_bytes() { return parameters[3]; }
#endif
uint arena_byte(uint at) { return (input_data[at / 4] >> ((at % 4) * 8)) & 255; }
uint key_byte(uint record, uint at) {
#ifdef COMPRESSED_INPUT
#ifdef PREFIX_CACHE
  return cached_key_byte(record, at);
#else
  return compressed_key_byte(record, at);
#endif
#else
  return arena_byte(extra[record * 4] + at);
#endif
}
int compare_records(uint a, uint b) {
#ifdef PREFIX_CACHE
  return cached_compare_records(a, b);
#else
  uint na = extra[a * RECORD_WORDS + 1], nb = extra[b * RECORD_WORDS + 1];
  for (uint i = shared_prefix_bytes(); i < min(na, nb); i++) {
    uint x = key_byte(a, i), y = key_byte(b, i);
    if (x != y)
      return x < y ? -1 : 1;
  }
  return na == nb ? 0 : (na < nb ? -1 : 1);
#endif
}
uint lcp_bits(uint a, uint b) {
#ifdef PREFIX_CACHE
  return cached_lcp_bits(a, b);
#else
  uint n = min(extra[a * RECORD_WORDS + 1], extra[b * RECORD_WORDS + 1]);
  for (uint i = shared_prefix_bytes(); i < n; i++) {
    uint x = key_byte(a, i) ^ key_byte(b, i);
    if (x)
      return i * 8 + 7 - firstbithigh(x);
  }
  return n * 8;
#endif
}
[numthreads(128, 1, 1)] void merge_order(uint3 tid : SV_DispatchThreadID) {
  uint na = parameters[1], nb = parameters[2], count = na + nb, diagonal = tid.x * 8;
  if (diagonal >= count)
    return;
  uint lo = diagonal > nb ? diagonal - nb : 0, hi = min(diagonal, na);
  while (lo < hi) {
    uint i = (lo + hi) / 2, j = diagonal - i;
    if (i < na && j > 0 && compare_records(i, na + j - 1) <= 0)
      lo = i + 1;
    else
      hi = i;
  }
  uint a = lo, b = diagonal - lo;
  for (uint k = diagonal; k < min(diagonal + 8, count); k++) {
    bool left = b == nb || (a < na && compare_records(a, na + b) <= 0);
    output_data[k] = left ? a++ : na + b++;
  }
}[numthreads(128, 1, 1)] void merge_keep(uint3 tid : SV_DispatchThreadID) {
  uint i = tid.x, n = parameters[0];
  if (i >= n)
    return;
  output_data[i] = (i + 1 == n || compare_records(references[i], references[i + 1]) != 0) ? 1 : 0;
}
[numthreads(128, 1, 1)] void merge_compact(uint3 tid : SV_DispatchThreadID) {
  uint i = tid.x, n = parameters[0];
  if (i >= n)
    return;
  if (input_data[i])
    output_data[extra[i]] = references[i];
  if (i + 1 == n)
    totals[0] = extra[i] + input_data[i];
} uint eg_size(uint n) {
  return 2 * firstbithigh(n + 1) + 1;
}
[numthreads(128, 1, 1)] void merge_sizes(uint3 tid : SV_DispatchThreadID) {
  uint i = tid.x, n = parameters[0];
  if (i >= n)
    return;
  uint record = references[i], bits = extra[record * RECORD_WORDS + 1] * 8;
  uint common = i ? lcp_bits(references[i - 1], record) : 0;
  uint control =
      (i % 15 == 0) ? common + 1 : extra[references[i - 1] * RECORD_WORDS + 1] * 8 - common;
  uint suffix = bits - common, value = extra[record * RECORD_WORDS + 3];
  output_data[i] = eg_size(control) + eg_size(suffix) + suffix + value;
  frames[i * 3] = control;
  frames[i * 3 + 1] = common;
  frames[i * 3 + 2] = suffix;
  if (value != extra[references[0] * RECORD_WORDS + 3]) {
    uint old;
    InterlockedOr(totals[0], 1, old);
  }
} uint eg_bit(uint value, uint position) {
  uint v = value + 1, z = firstbithigh(v);
  if (position < z)
    return 0;
  return (v >> (2 * z - position)) & 1;
}
uint record_bit(uint record, uint ordinal, uint at) {
  uint control = frames[ordinal * 3], common = frames[ordinal * 3 + 1],
       suffix = frames[ordinal * 3 + 2];
  uint n = eg_size(control);
  if (at < n)
    return eg_bit(control, at);
  at -= n;
  n = eg_size(suffix);
  if (at < n)
    return eg_bit(suffix, at);
  at -= n;
  if (at < suffix) {
    uint k = common + at;
#ifdef COMPRESSED_INPUT
    return compressed_suffix_bit(record, k);
#else
    return (key_byte(record, k / 8) >> (7 - k % 8)) & 1;
#endif
  }
  at -= suffix;
#ifdef COMPRESSED_INPUT
  return compressed_value_bit(record, at);
#else
  return (arena_byte(extra[record * RECORD_WORDS + 2] + at / 8) >> (7 - at % 8)) & 1;
#endif
}
[numthreads(128, 1, 1)] void merge_emit(uint3 tid : SV_DispatchThreadID) {
  uint word = tid.x, bits = parameters[1], n = parameters[0], start = word * 32;
  if (start >= bits)
    return;
  uint lo = 0, hi = n;
  while (lo < hi) {
    uint mid = (lo + hi) / 2;
    if (totals[mid] <= start)
      lo = mid + 1;
    else
      hi = mid;
  }
  uint ordinal = lo - 1, value = 0;
  for (uint k = 0; k < 32 && start + k < bits; k++) {
    if (ordinal + 1 < n && start + k >= totals[ordinal + 1])
      ++ordinal;
    value |= record_bit(references[ordinal], ordinal, start + k - totals[ordinal]) << (31 - k);
  }
  // Native uint buffer is little endian; Everett payload bits are MSB-first bytes.
  output_data[parameters[2] + word] =
      ((value & 255) << 24) | ((value & 65280) << 8) | ((value >> 8) & 65280) | (value >> 24);
}[numthreads(128, 1, 1)] void rank15_classes(uint3 tid : SV_DispatchThreadID) {
  uint block = tid.x, groups = parameters[0];
  if (block >= (groups + 127) / 128)
    return;
  uint total = 0;
  for (uint packed = 0; packed < 16; packed++) {
    uint value = 0;
    for (uint nibble = 0; nibble < 8; nibble++) {
      uint group = block * 128 + packed * 8 + nibble;
      uint c = group < groups ? arena_byte(group) : 0;
      value |= c << (nibble * 4);
      total += c;
    }
    if (block * 16 + packed < ((groups + 15) / 16) * 2)
      output_data[block * 16 + packed] = value;
  }
  totals[block] = total;
}
#ifdef COMPRESSED_INPUT
// Correctness-only probe; absent from timed merges. Every reconstructed byte
// participates, including inherited bytes the merge comparisons may skip.
[numthreads(128, 1, 1)] void compressed_probe(uint3 tid : SV_DispatchThreadID) {
  uint record = tid.x;
  if (record >= parameters[0])
    return;
  uint hash = 2166136261;
  for (uint i = 0; i < extra[record * 8 + 1]; i++)
    hash = (hash ^ compressed_key_byte(record, i)) * 16777619;
  output_data[record] = hash;
}
#endif

#ifdef COMPRESSED_INPUT
#include "emit_word.hlsl"
#endif
