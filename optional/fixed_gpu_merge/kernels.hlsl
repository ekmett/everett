// \file
// \author Edward Kmett <ekmett@gmail.com>
// \brief Complete experimental fixed-key merges, including native EF output.
// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
// All extents are bounded by the host to 32 bits. Wire EF words remain LE u64.
[[vk::binding(0)]] StructuredBuffer<uint> input_data;
[[vk::binding(1)]] RWStructuredBuffer<uint> output_data;
[[vk::binding(2)]] RWStructuredBuffer<uint> status;
[[vk::binding(3)]] StructuredBuffer<uint> parameters;
[[vk::binding(4)]] StructuredBuffer<uint> extra;
[[vk::binding(5)]] StructuredBuffer<uint> references;
[[vk::binding(6)]] StructuredBuffer<uint> frames;
[[vk::binding(7)]] StructuredBuffer<uint> other;
[[vk::binding(8)]] StructuredBuffer<uint> prefix;
#ifndef FV_WORDS
#define FV_WORDS 4
#endif

void failure(uint code) { uint ignored; InterlockedOr(status[0], code, ignored); }

[numthreads(128, 1, 1)] void keep_initialize(uint3 tid : SV_DispatchThreadID) {
  if (tid.x < parameters[0]) output_data[tid.x] = 1;
}

// Due metadata: magic, count, target count, target identity, payload total,
// then three reserved words, then strictly increasing target ordinals.
[numthreads(128, 1, 1)] void cancel_due(uint3 tid : SV_DispatchThreadID) {
  uint i = tid.x;
  if (i >= parameters[0]) return;
  uint target = input_data[8 + i];
  if (target >= parameters[1] || (i && target <= input_data[7 + i])) {
    failure(1); return;
  }
  output_data[target] = 0;
}

uint select_word(uint value, uint ordinal) {
  uint result = 0;
  for (uint width = 16; width; width >>= 1) {
    uint lower = value & ((1u << width) - 1), population = countbits(lower);
    if (ordinal >= population) {
      result += width; ordinal -= population; value >>= width;
    } else value = lower;
  }
  return result;
}

// Input envelope addresses are u32-word offsets, with byte payload extent.
uint select_value(uint ordinal) {
  uint sample = input_data[9] + (ordinal >> 8) * 4;
  uint remaining = ordinal & 255, position = 0;
  uint sparse = input_data[sample + 2];
  if (sparse != 0xffffffffu) {
    position = input_data[input_data[10] + (sparse + remaining) * 2];
  } else {
    uint first = input_data[sample], word = first >> 5, shift = first & 31;
    bool found = false;
    for (uint scanned = 0; scanned < 129 && word < input_data[13] * 2; ++scanned, ++word) {
      uint value = input_data[input_data[8] + word];
      if (!scanned) value &= 0xffffffffu << shift;
      uint population = countbits(value);
      if (remaining < population) {
        position = word * 32 + select_word(value, remaining);
        found = true; break;
      }
      remaining -= population;
    }
    if (!found) { failure(2); return 0; }
  }
  uint width = input_data[11], low = 0;
  if (width) {
    uint bit = ordinal * width, word = input_data[7] + (bit >> 5), shift = bit & 31;
    low = input_data[word] >> shift;
    if (shift + width > 32) low |= input_data[word + 1] << (32 - shift);
    low &= (1u << width) - 1;
  }
  return ((position - ordinal) << width) | low;
}

[numthreads(128, 1, 1)] void decode_values(uint3 tid : SV_DispatchThreadID) {
  uint i = tid.x;
  if (i >= parameters[0] || (parameters[1] && !extra[i])) return;
  uint start = input_data[2] ? select_value(i) : i * 16;
  uint end = input_data[2] ? select_value(i + 1) : start + 16;
  if (end < start || end > input_data[6]) { failure(4); return; }
  output_data[i * 2] = start;
  output_data[i * 2 + 1] = end - start;
}

[numthreads(128, 1, 1)] void compact_a(uint3 tid : SV_DispatchThreadID) {
  uint i = tid.x;
  if (status[0]) return;
  if (i < parameters[0] && input_data[i]) output_data[extra[i]] = i;
}

int compare_key(uint a, uint b) {
  uint ai = input_data[4] + a * 4, bi = extra[4] + b * 4;
  for (uint j = 0; j < 4; ++j) {
    uint x = input_data[ai + j], y = extra[bi + j];
    if (x != y) return x < y ? -1 : 1;
  }
  return 0;
}

uint older_ordinal(uint i) { return parameters[7] ? references[i] : i; }

// One invocation owns a 32-record diagonal partition. references is the
// compacted list of surviving older ordinals, not premerged CPU metadata.
[numthreads(128, 1, 1)] void merge_order(uint3 tid : SV_DispatchThreadID) {
  uint diagonal = tid.x * 32, n = parameters[0], na = parameters[1];
  uint nb = parameters[2], survivors = parameters[3];
  if (diagonal >= n || status[0]) return;
  uint lo = diagonal > nb ? diagonal - nb : 0, hi = min(diagonal, survivors);
  while (lo < hi) {
    uint mid = lo + ((hi - lo) >> 1), b = diagonal - mid;
    if (mid < survivors && b && compare_key(older_ordinal(mid), b - 1) <= 0) lo = mid + 1;
    else hi = mid;
  }
  uint a = lo, b = diagonal - lo;
  for (uint i = diagonal; i < min(diagonal + 32, n); ++i) {
    int order = a < survivors && b < nb ? compare_key(older_ordinal(a), b) : 1;
    if (a < survivors && b < nb && !order) failure(8);
    bool take_a = a < survivors && (b == nb || order < 0);
    output_data[i] = take_a ? older_ordinal(a++) : na + b++;
  }
}

[numthreads(128, 1, 1)] void merge_lengths(uint3 tid : SV_DispatchThreadID) {
  uint i = tid.x;
  if (i >= parameters[0]) return;
  if (status[0]) { output_data[i] = 0; return; }
  uint record = input_data[i], na = parameters[1];
  output_data[i] = record < na ? extra[record * 2 + 1] : references[(record - na) * 2 + 1];
}

[numthreads(128, 1, 1)] void emit_keys(uint3 tid : SV_DispatchThreadID) {
  uint i = tid.x;
  if (i >= parameters[0]) return;
  uint record = references[i], na = parameters[1];
  for (uint j = 0; j < 4; ++j)
    output_data[64 + i * 4 + j] = record < na
        ? input_data[input_data[4] + record * 4 + j]
        : extra[extra[4] + (record - na) * 4 + j];
}

uint source_byte(uint record, uint offset) {
  uint na = parameters[1];
  if (record < na) {
    uint at = input_data[5] + frames[record * 2] + offset;
    return (input_data[at >> 2] >> ((at & 3) * 8)) & 255;
  }
  record -= na;
  uint at = extra[5] + other[record * 2] + offset;
  return (extra[at >> 2] >> ((at & 3) * 8)) & 255;
}

uint source_word(uint record, uint offset) {
  uint na = parameters[1];
  if (record < na) {
    uint at = input_data[5] + frames[record * 2] + offset, shift = (at & 3) * 8;
    uint value = input_data[at >> 2] >> shift;
    if (shift) value |= input_data[(at >> 2) + 1] << (32 - shift);
    return value;
  }
  record -= na;
  uint at = extra[5] + other[record * 2] + offset, shift = (at & 3) * 8;
  uint value = extra[at >> 2] >> shift;
  if (shift) value |= extra[(at >> 2) + 1] << (32 - shift);
  return value;
}

// Each variable-mode invocation owns four complete output words. The one
// initial upper_bound skips empty values; most subsequent words copy directly
// from a single value. Boundary fragments use bytes without racing neighbors.
[numthreads(128, 1, 1)] void emit_values(uint3 tid : SV_DispatchThreadID) {
  uint at = tid.x * 4, n = parameters[0], bytes = parameters[4];
  if (at >= bytes) return;
  if (!parameters[7]) {
    uint record = references[at >> 4], na = parameters[1], word = (at & 15) >> 2;
    output_data[(parameters[6] >> 2) + tid.x] = record < na
        ? input_data[(input_data[5] >> 2) + record * 4 + word]
        : extra[(extra[5] >> 2) + (record - na) * 4 + word];
    return;
  }
  at = tid.x * (4 * FV_WORDS);
  if (at >= bytes) return;
  uint lo = 0, hi = n;
  while (lo < hi) {
    uint mid = lo + ((hi - lo) >> 1);
    if (prefix[mid] <= at) lo = mid + 1; else hi = mid;
  }
  uint record = lo - 1;
  for (uint word = 0; word < FV_WORDS && at < bytes; ++word, at += 4) {
    while (record + 1 < n && prefix[record + 1] <= at) ++record;
    uint end = record + 1 == n ? bytes : prefix[record + 1], result = 0;
    if (end - at >= 4) result = source_word(references[record], at - prefix[record]);
    else for (uint j = 0; j < min(4u, bytes - at); ++j) {
      while (record + 1 < n && prefix[record + 1] <= at + j) ++record;
      result |= source_byte(references[record], at + j - prefix[record]) << (j * 8);
    }
    output_data[(parameters[6] + at) >> 2] = result;
  }
}

uint ef_value(uint index) { return index + 1 == parameters[0] ? parameters[4] : input_data[index]; }
uint ef_position(uint index) { return (ef_value(index) >> parameters[5]) + index; }

[numthreads(128, 1, 1)] void ef_sparse_count(uint3 tid : SV_DispatchThreadID) {
  uint first = tid.x * 256, count = parameters[0];
  if (first >= count) return;
  uint end = min(first + 256, count);
  output_data[tid.x] = ef_position(end - 1) - ef_position(first) >= 4096 ? end - first : 0;
}

[numthreads(128, 1, 1)] void ef_low(uint3 tid : SV_DispatchThreadID) {
  uint word = tid.x, count = parameters[0], width = parameters[5];
  if (!width || word >= ((count * width + 63) >> 6) * 2) return;
  uint bit = word * 32, index = bit / width, offset = bit % width, result = 0, written = 0;
  while (written < 32 && index < count) {
    uint take = min(32 - written, width - offset);
    result |= ((ef_value(index) >> offset) & ((1u << take) - 1)) << written;
    written += take; ++index; offset = 0;
  }
  output_data[parameters[6] + word] = result;
}

[numthreads(128, 1, 1)] void ef_high(uint3 tid : SV_DispatchThreadID) {
  uint word = tid.x, count = parameters[0], bits = (parameters[4] >> parameters[5]) + count;
  if (word >= ((bits + 63) >> 6) * 2) return;
  uint first = word * 32, lo = 0, hi = count;
  while (lo < hi) {
    uint mid = lo + ((hi - lo) >> 1);
    if (ef_position(mid) < first) lo = mid + 1; else hi = mid;
  }
  uint value = 0;
  for (uint i = lo; i < count; ++i) {
    uint position = ef_position(i);
    if (position - first >= 32) break;
    value |= 1u << (position - first);
  }
  output_data[parameters[6] + word] = value;
}

[numthreads(128, 1, 1)] void ef_samples(uint3 tid : SV_DispatchThreadID) {
  uint group = tid.x;
  if (group * 256 >= parameters[0]) return;
  uint at = parameters[6] + group * 4;
  output_data[at] = ef_position(group * 256);
  output_data[at + 1] = 0;
  output_data[at + 2] = extra[group] ? references[group] : 0xffffffffu;
  output_data[at + 3] = extra[group] ? 0 : 0xffffffffu;
}

[numthreads(128, 1, 1)] void ef_sparse(uint3 tid : SV_DispatchThreadID) {
  uint i = tid.x;
  if (i >= parameters[0] || !extra[i >> 8]) return;
  uint at = parameters[6] + (references[i >> 8] + (i & 255)) * 2;
  output_data[at] = ef_position(i);
  output_data[at + 1] = 0;
}
