// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
// KV02 byte FC for the built-in optional-string sort. Input descriptors retain
// the shared eight-word ABI (bit positions, byte key lengths); output offsets,
// EF universes, frame sizes and the fixed value stride are all BYTE counts.
#ifdef TOMBSTONE_MERGE
#error Byte optional-value tags require a byte-specific cleanup implementation
#endif
#define COMPRESSED_INPUT 1
#define PREFIX_CACHE 1
#define BYTE_PROFILE 1
#include "../gpu_merge/kernels.hlsl"

uint byte_input(uint at) {
  uint absolute = parameters[4] + at;
  return (input_data[absolute >> 2] >> ((absolute & 3) * 8)) & 255;
}

bool byte_count(inout uint at, uint end, out uint value) {
  value = 0;
  for (uint i = 0; i < 5; ++i) {
    if (at >= end) return false;
    uint digit = byte_input(at++), payload = digit & 127;
    if (i == 4 && payload > 15) return false;
    value |= payload << (i * 7);
    if (!(digit & 128)) return !i || payload != 0;
  }
  return false;
}

// The shared EF reader selects original mapped offsets directly on the GPU.
// Its packet contains byte units here; no host-side sample expansion is needed.
[numthreads(128, 1, 1)] void byte_parse_input(uint3 tid : SV_DispatchThreadID) {
  uint block = tid.x, count = parameters[0], blocks = parameters[3];
  if (block >= blocks) return;
  uint first = block * 15, at, end;
  if (first >= count || !parse_block_offset(block, at) ||
      !parse_block_offset(block + 1, end) || at >= end || end > parameters[5] ||
      (!block && at) || (block + 1 == blocks && end != parameters[5])) {
    parse_failure(1);
    return;
  }
  uint previous = 0;
  for (uint local = 0; local < min(15u, count - first); ++local) {
    uint control, suffix, value = references[10];
    if (!byte_count(at, end, control) || !byte_count(at, end, suffix) ||
        (!value && !byte_count(at, end, value))) {
      parse_failure(2);
      return;
    }
    if (local && control > previous) { parse_failure(4); return; }
    uint retained = local ? previous - control : control;
    if ((!block && !local && retained) || retained >= (1u << 27) ||
        suffix >= (1u << 27) - retained || (first + local && !suffix) ||
        suffix > end - at) {
      parse_failure(8);
      return;
    }
    uint key = retained + suffix, literal = at;
    at += suffix;
    if (!value || value >= (1u << 27) || value > end - at) {
      parse_failure(16);
      return;
    }
    uint tag = byte_input(at);
    if (tag > 1 || (!tag && value != 1)) { parse_failure(16); return; }
    uint output = (parameters[1] + first + local) * 8;
    output_data[output] = literal << 3;
    output_data[output + 1] = key;
    output_data[output + 2] = at << 3;
    output_data[output + 3] = value << 3;
    output_data[output + 4] = retained << 3;
    output_data[output + 5] = 0;
    output_data[output + 6] = parameters[2];
    output_data[output + 7] = 0;
    at += value;
    previous = key;
    uint ignored;
    // Three canonical u32 LEB128 controls need at most fifteen bytes.
    InterlockedMax(totals[1], key + value + 15, ignored);
  }
  if (at != end) parse_failure(32);
}

// Run after compaction and before sizing. The mismatch word is initially zero.
// This pass detects a common encoded value width without any CPU record walk.
[numthreads(128, 1, 1)] void byte_value_width(uint3 tid : SV_DispatchThreadID) {
  if (tid.x >= parameters[0]) return;
  if (extra[references[tid.x] * 8 + 3] != extra[references[0] * 8 + 3]) {
    uint ignored;
    InterlockedOr(totals[0], 1, ignored);
  }
}

uint byte_count_size(uint value) {
  return value ? firstbithigh(value) / 7 + 1 : 1;
}
uint byte_count_digit(uint value, uint index) {
  value >>= index * 7;
  return (value & 127) | (value >= 128 ? 128 : 0);
}

[numthreads(128, 1, 1)] void byte_merge_sizes(uint3 tid : SV_DispatchThreadID) {
  uint i = tid.x, n = parameters[0];
  if (i >= n) return;
  uint record = references[i], key = extra[record * 8 + 1];
  uint common = i ? cached_lcp_bits(references[i - 1], record) >> 3 : 0;
  uint control = i % 15 ? extra[references[i - 1] * 8 + 1] - common : common;
  uint suffix = key - common, value = extra[record * 8 + 3] >> 3;
  output_data[i] = byte_count_size(control) + byte_count_size(suffix) + suffix + value +
      (totals[0] ? byte_count_size(value) : 0);
  frames[i * 3] = control;
  frames[i * 3 + 1] = common;
  frames[i * 3 + 2] = suffix;
}

uint byte_record_byte(uint ordinal, uint at) {
  uint record = references[ordinal];
  uint control = frames[ordinal * 3], common = frames[ordinal * 3 + 1];
  uint suffix = frames[ordinal * 3 + 2], value = extra[record * 8 + 3] >> 3;
  uint width = byte_count_size(control);
  if (at < width) return byte_count_digit(control, at);
  at -= width;
  width = byte_count_size(suffix);
  if (at < width) return byte_count_digit(suffix, at);
  at -= width;
  // Parameter3 is the output common width, or zero for variable values.
  if (!parameters[3]) {
    width = byte_count_size(value);
    if (at < width) return byte_count_digit(value, at);
    at -= width;
  }
  uint source = extra[record * 8 + 6];
  if (at < suffix)
    return compressed_source_byte(source, (extra[record * 8] >> 3) +
        common - (extra[record * 8 + 4] >> 3) + at);
  return compressed_source_byte(source, (extra[record * 8 + 2] >> 3) + at - suffix);
}

// One invocation owns an output word, including zero padding at the end.
// Parameters: {records, payload_bytes, destination_word, common_value_bytes,
//              source0_payload_delta, source1_payload_delta, unused}.
// totals holds the exclusive record-offset scan, references the compact order.
[numthreads(128, 1, 1)] void byte_merge_emit_words(uint3 tid : SV_DispatchThreadID) {
  uint first = tid.x * 4, extent = parameters[1], n = parameters[0];
  if (first >= extent) return;
  uint lo = 0, hi = n;
  while (lo + 1 < hi) {
    uint mid = lo + (hi - lo) / 2;
    if (totals[mid] <= first) lo = mid;
    else hi = mid;
  }
  uint word = 0;
  for (uint i = 0; i < min(4u, extent - first); ++i) {
    uint at = first + i;
    while (lo + 1 < n && totals[lo + 1] <= at) ++lo;
    word |= byte_record_byte(lo, at - totals[lo]) << (i * 8);
  }
  output_data[parameters[2] + tid.x] = word;
}
