// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
// Included after prefix_owner.hlsl and the concrete resource declarations.
// Cache eight raw key bytes AFTER the GPU-proven common byte prefix. Words 5
// and 7 of each eight-u32 descriptor hold their big-endian numeric values.
// Short keys are padded with zero bytes; the descriptor still carries length.

// Dispatch after compressed_prefix, before any cached comparison. Bind the
// descriptors to output_data and extra. Reads use only the other descriptor
// fields, so each invocation can fill its own two unused words in place.
[numthreads(128, 1, 1)] void compressed_cache(uint3 tid : SV_DispatchThreadID) {
  uint record = tid.x;
  if (record >= parameters[0])
    return;
  uint prefix = prefix_data[0], length = extra[record * 8 + 1];
  uint first = 0, second = 0;
  for (uint i = 0; i < 4; ++i) {
    uint at = prefix + i;
    uint value = at < length ? compressed_key_byte(record, at) : 0;
    first = (first << 8) | value;
  }
  for (uint i = 4; i < 8; ++i) {
    uint at = prefix + i;
    uint value = at < length ? compressed_key_byte(record, at) : 0;
    second = (second << 8) | value;
  }
  output_data[record * 8 + 5] = first;
  output_data[record * 8 + 7] = second;
}

// The caller still supplies a valid byte position in this record's key.
uint cached_key_byte(uint record, uint at) {
  uint prefix = prefix_data[0];
  if (at >= prefix && at - prefix < 8) {
    uint relative = at - prefix;
    uint word = extra[record * 8 + (relative < 4 ? 5 : 7)];
    return (word >> ((3 - (relative & 3)) * 8)) & 255;
  }
  return compressed_key_byte(record, at);
}

int cached_compare_records(uint a, uint b) {
  if (a == b)
    return 0;
  // Each individual source is strictly sorted and has no duplicate keys.
  // Combined descriptor ordinals preserve the order within each source.
  if (extra[a * 8 + 6] == extra[b * 8 + 6])
    return a < b ? -1 : 1;

  uint x = extra[a * 8 + 5], y = extra[b * 8 + 5];
  if (x != y)
    return x < y ? -1 : 1;
  x = extra[a * 8 + 7];
  y = extra[b * 8 + 7];
  if (x != y)
    return x < y ? -1 : 1;

  uint na = extra[a * 8 + 1], nb = extra[b * 8 + 1];
  uint count = min(na, nb), prefix = prefix_data[0];
  // Zero padding preserves unsigned-byte lexicographic order: a difference
  // after a shorter key ends can only place that shorter key first. If the
  // padded words tie, length resolves a prefix tie inside the cached window.
  for (uint at = prefix + 8; at < count; ++at) {
    x = compressed_key_byte(a, at);
    y = compressed_key_byte(b, at);
    if (x != y)
      return x < y ? -1 : 1;
  }
  return na == nb ? 0 : (na < nb ? -1 : 1);
}

// Equal leading bits in the two cached words, including their zero padding.
uint cached_equal_bits(uint a, uint b) {
  uint difference = extra[a * 8 + 5] ^ extra[b * 8 + 5];
  if (difference != 0)
    return 31 - firstbithigh(difference);
  difference = extra[a * 8 + 7] ^ extra[b * 8 + 7];
  if (difference != 0)
    return 63 - firstbithigh(difference);
  return 64;
}

uint cached_lcp_bits(uint a, uint b) {
  uint count = min(extra[a * 8 + 1], extra[b * 8 + 1]);
  if (a == b)
    return count * 8;
  uint prefix = prefix_data[0], matched = cached_equal_bits(a, b);
  // Unlike comparison, LCP must not count padded bits beyond the shorter key.
  uint available = (count - prefix) * 8;
  if (matched < 64 || available <= 64)
    return prefix * 8 + min(matched, available);
  for (uint at = prefix + 8; at < count; ++at) {
    uint difference = compressed_key_byte(a, at) ^ compressed_key_byte(b, at);
    if (difference != 0)
      return at * 8 + 7 - firstbithigh(difference);
  }
  return count * 8;
}
