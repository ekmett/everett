// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
// Optional integration with the attributed, self-contained 512-bit rank helper.
// Temporary construction scratch: collision bits -> surviving record ordinals.
// This neither emits nor replaces the final IX03 rank15 query directories.
// Include after prefix_cache.hlsl and the existing resource declarations.
#include "rank512.hlsl"

#ifndef COLLISION_TILE_RECORDS
#define COLLISION_TILE_RECORDS 32
#endif

// Match/scatter params: {A+B, A, B, unused, payloadA, payloadB, tree_base}.
// Descriptors are older A followed by newer B. Each source is sorted UNIQUE.
uint collision_lower_a(uint record) {
  uint lo = 0, hi = parameters[1];
  while (lo < hi) {
    uint mid = lo + (hi - lo) / 2;
    if (cached_compare_records(mid, record) < 0) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}
uint collision_lower_b(uint record) {
  uint lo = 0, hi = parameters[2], a = parameters[1];
  while (lo < hi) {
    uint mid = lo + (hi - lo) / 2;
    if (cached_compare_records(a + mid, record) < 0) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

// Clear before marking. params[0]=A, output1=rank storage. The 512-bit layout is
// [ceil(A/32) bitmap words][ceil(A/512) three-word blocks][total]. Everett's
// variant uses the same bitmap but a separate 2048-bit directory in totals2.
[numthreads(128, 1, 1)]
void collision_clear(uint3 tid : SV_DispatchThreadID) {
  uint words = rank_words(parameters[0]);
#ifndef COLLISION_RANK2048
  words += rank_blocks(parameters[0]) * 3 + 1;
#endif
  if (tid.x < words) output_data[tid.x] = 0;
}

// output1=zeroed collision bitmap/rank storage; frames6=B's lower_bound(A).
// Distinct B keys can hit each A at most once, but bits in one bitmap word
// still need atomic OR. Tombstones have exactly the same collision rule.
void collision_remember(uint j, uint i) {
  frames[j] = i;
  uint a = parameters[1];
  if (i < a && cached_compare_records(i, a + j) == 0) {
    uint old;
    InterlockedOr(output_data[i >> 5], 1u << (i & 31), old);
  }
}
[numthreads(128, 1, 1)]
void collision_mark(uint3 tid : SV_DispatchThreadID) {
  uint j = tid.x;
  if (j < parameters[2]) collision_remember(j, collision_lower_a(parameters[1] + j));
}

// Optional balanced-input variant: one lower_bound per B tile, followed by
// a monotone A walk. Sparse B should keep the independent-search variant:
// scanning the gaps between its keys can otherwise touch most of large A.
[numthreads(128, 1, 1)]
void collision_mark_tiled(uint3 tid : SV_DispatchThreadID) {
  uint first = tid.x * COLLISION_TILE_RECORDS, a = parameters[1], b = parameters[2];
  if (first >= b) return;
  uint i = collision_lower_a(a + first);
  for (uint j = first; j < min(first + COLLISION_TILE_RECORDS, b); ++j) {
    while (i < a && cached_compare_records(i, a + j) < 0) ++i;
    collision_remember(j, i);
  }
}

// Parallel constructor for the 512-bit rank layout. This adapter
// builds independent relative blocks, then uses the existing global scan.
// It does not call the helper's single-256-thread-group rank_build utility.
// input0 and output1 may alias: input reads bitmap words, output writes only
// the following directory. totals2 gets one population per block. params0=A.
[numthreads(128, 1, 1)]
void collision_rank512_count(uint3 tid : SV_DispatchThreadID) {
  uint block = tid.x, bits = parameters[0], words = rank_words(bits);
  if (block >= rank_blocks(bits)) return;
  uint local = 0, low = 0, high = 0;
  for (uint pair = 0; pair < 8; ++pair) {
    for (uint half = 0; half < 2; ++half) {
      uint word = block * 16 + pair * 2 + half;
      if (word < words) local += countbits(input_data[word]);
    }
    if (pair < 7) {
      uint shift = pair * 9;
      if (shift < 32) {
        low |= local << shift;
        if (shift != 0) high |= local >> (32 - shift);
      } else high |= local << (shift - 32);
    }
  }
  uint at = words + block * 3;
  output_data[at] = 0;
  output_data[at + 1] = low; output_data[at + 2] = high;
  totals[block] = local;
}

// input0=exclusive block scan, totals2=block populations, output1=rank storage.
// Dispatch max(1,ceil(A/512)). No bitmap bits or relative fields are changed.
[numthreads(128, 1, 1)]
void collision_rank512_finish(uint3 tid : SV_DispatchThreadID) {
  uint block = tid.x, bits = parameters[0], blocks = rank_blocks(bits), words = rank_words(bits);
  if (blocks == 0) { if (block == 0) output_data[0] = 0; return; }
  if (block >= blocks) return;
  output_data[words + block * 3] = input_data[block];
  if (block + 1 == blocks) output_data[words + blocks * 3] = input_data[block] + totals[block];
}

// Everett comparison: existing rank_count builds its 2048/512 relative directory.
// Finish its scanned bases and append the bounded experiment's total count.
// params0=A; input0=scan, totals2=block counts, output1=directory+one total.
[numthreads(128, 1, 1)]
void collision_rank2048_finish(uint3 tid : SV_DispatchThreadID) {
  uint block = tid.x, blocks = (parameters[0] + 2047) / 2048;
  if (blocks == 0) { if (block == 0) output_data[0] = 0; return; }
  if (block >= blocks) return;
  output_data[block * 2] = input_data[block];
  if (block + 1 == blocks) output_data[blocks * 2] = input_data[block] + totals[block];
}

// Adapt existing bindings to the byte-addressed 512-bit query protocol.
// The rank query itself is the template in rank512.hlsl.
struct collision_rank512_view {
  uint unused;
  uint Load(uint at) { return frames[at >> 2]; }
  uint3 Load3(uint at) { return uint3(Load(at), Load(at + 4), Load(at + 8)); }
};
uint collision_prefix(uint i) {
  uint bits = parameters[1];
#ifdef COLLISION_RANK2048
  // frames6=bitmap, totals2=Everett directory + final total. The current bounds
  // are below2^24 records, so Everett's absolute rank epoch is always zero.
  if (i >= bits) return totals[((bits + 2047) / 2048) * 2];
  uint block = i / 2048, run = (i / 512) & 3;
  uint result = totals[block * 2], packed = totals[block * 2 + 1];
  for (uint j = 0; j < run; ++j) result += (packed >> (j * 11)) & 1023;
  for (uint word = (i / 512) * 16; word < i / 32; ++word) result += countbits(frames[word]);
  return result + countbits(frames[i / 32] & ((1u << (i & 31)) - 1));
#else
  collision_rank512_view view = (collision_rank512_view)0;
  return rank_prefix(view, bits, i);
#endif
}
bool collision_cancelled(uint i) { return (frames[i >> 5] & (1u << (i & 31))) != 0; }

// Scatter bindings: output1=final ordered record references; references5=B's
// saved lower_bound(A); frames6=rank storage (or bitmap for Everett), totals2=
// Everett directory if selected. Other bindings still supply compressed keys.
[numthreads(128, 1, 1)]
void collision_scatter_b(uint3 tid : SV_DispatchThreadID) {
  uint j = tid.x;
  if (j >= parameters[2]) return;
  uint i = references[j];
  output_data[i + j - collision_prefix(i)] = parameters[1] + j;
}
[numthreads(128, 1, 1)]
void collision_scatter_a(uint3 tid : SV_DispatchThreadID) {
  uint i = tid.x;
  if (i >= parameters[1] || collision_cancelled(i)) return;
  uint j = collision_lower_b(i);
  output_data[i + j - collision_prefix(i)] = i;
}

// One binary search per A tile, then a monotone B cursor. This is useful when
// A is large and B is a small incoming run; it avoids a binary search for each
// surviving A. The independent variant remains available for comparison.
[numthreads(128, 1, 1)]
void collision_scatter_a_tiled(uint3 tid : SV_DispatchThreadID) {
  uint first = tid.x * COLLISION_TILE_RECORDS, a = parameters[1], b = parameters[2];
  if (first >= a) return;
  uint j = collision_lower_b(first);
  for (uint i = first; i < min(first + COLLISION_TILE_RECORDS, a); ++i) {
    while (j < b && cached_compare_records(a + j, i) < 0) ++j;
    if (!collision_cancelled(i)) output_data[i + j - collision_prefix(i)] = i;
  }
}

// Alternative to corrected direct scatter: compact only surviving A, then
// merge two now-disjoint streams using Merge Path. This avoids A binary
// searches and the existing full-size keep-flag/compaction scan.
[numthreads(128, 1, 1)]
void collision_compact_a(uint3 tid : SV_DispatchThreadID) {
  uint i = tid.x;
  if (i < parameters[1] && !collision_cancelled(i))
    output_data[i - collision_prefix(i)] = i;
}

// references5=compacted A references; params3=their count. params1 remains
// original A count, since B descriptor j is still at A+j. No bitmap access.
[numthreads(128, 1, 1)]
void collision_merge_compact(uint3 tid : SV_DispatchThreadID) {
  uint old_a = parameters[1], a_count = parameters[3], b_count = parameters[2];
  uint count = a_count + b_count, diagonal = tid.x * 8;
  if (diagonal >= count) return;
  uint lo = diagonal > b_count ? diagonal - b_count : 0, hi = min(diagonal, a_count);
  while (lo < hi) {
    uint i = lo + (hi - lo) / 2, j = diagonal - i;
    if (i < a_count && j > 0 && cached_compare_records(references[i], old_a + j - 1) <= 0)
      lo = i + 1;
    else hi = i;
  }
  uint i = lo, j = diagonal - lo;
  for (uint k = diagonal; k < min(diagonal + 8, count); ++k) {
    bool left = j == b_count || (i < a_count && cached_compare_records(references[i], old_a + j) <= 0);
    if (left) output_data[k] = references[i++];
    else output_data[k] = old_a + j++;
  }
}

// Correctness-only query probe. references5 contains positions in[0,A];
// output1 receives their exclusive collision ranks. params0=query count.
[numthreads(128, 1, 1)]
void collision_rank_probe(uint3 tid : SV_DispatchThreadID) {
  if (tid.x < parameters[0]) output_data[tid.x] = collision_prefix(references[tid.x]);
}
