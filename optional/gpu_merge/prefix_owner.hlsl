// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
// Included after the optional merge driver's concrete resource declarations.
// Descriptor: literal_bit,key_bytes,value_bit,value_bits,retained,parent,source,0.
// All descriptor bit offsets are relative to the source's KV03 payload.

uint compressed_source_byte(uint source, uint byte_offset) {
  uint absolute = byte_offset + parameters[4 + source];
  uint word = source == 0 ? input_data[absolute >> 2] : other_data[absolute >> 2];
  return (word >> ((absolute & 3) * 8)) & 255;
}

uint compressed_source_bit(uint source, uint bit_offset) {
  return (compressed_source_byte(source, bit_offset >> 3) >> (7 - (bit_offset & 7))) & 1;
}

// Rightmost j <= record with retained[j] <= bit_position. The first record
// of each input has retained=0, so a valid query cannot cross source inputs.
uint compressed_owner(uint record, uint bit_position) {
  if (extra[record * 8 + 4] <= bit_position)
    return record;
  uint base = parameters[6];
  uint node = base + record;
  while (node > 1) {
    if ((node & 1) != 0 && prefix_tree[node - 1] <= bit_position) {
      --node;
      while (node < base) {
        uint right = node * 2 + 1;
        node = prefix_tree[right] <= bit_position ? right : right - 1;
      }
      return node - base;
    }
    node >>= 1;
  }
  // Unreachable for descriptors admitted by the parser and prefix_leaf checks.
  return 0;
}

uint compressed_key_byte_scalar(uint record, uint byte_offset) {
  uint first = byte_offset * 8;
  uint end = first + 8;
  uint value = 0;
  while (end > first) {
    record = compressed_owner(record, end - 1);
    uint retained = extra[record * 8 + 4];
    uint begin = max(first, retained);
    uint literal = extra[record * 8];
    uint source = extra[record * 8 + 6];
    for (uint bit = begin; bit < end; ++bit) {
      uint digit = compressed_source_bit(source, literal + bit - retained);
      value |= digit << (7 - (bit - first));
    }
    end = begin;
  }
  return value;
}

// At most eight bits; do not read a second byte unless the fragment crosses
// its source byte boundary. A fragment can begin at any FC bit position.
uint compressed_source_fragment(uint source, uint at, uint count) {
  uint shift = at & 7;
  uint value = compressed_source_byte(source, at >> 3);
  uint mask = (1u << count) - 1;
  if (shift + count <= 8)
    return (value >> (8 - shift - count)) & mask;
  value = (value << 8) | compressed_source_byte(source, (at >> 3) + 1);
  return (value >> (16 - shift - count)) & mask;
}

uint compressed_key_byte(uint record, uint byte_offset) {
  uint first = byte_offset * 8;
  uint end = first + 8;
  uint value = 0;
  while (end > first) {
    record = compressed_owner(record, end - 1);
    uint retained = extra[record * 8 + 4];
    uint begin = max(first, retained);
    uint literal = extra[record * 8];
    uint source = extra[record * 8 + 6];
    uint fragment = compressed_source_fragment(source, literal + begin - retained, end - begin);
    value |= fragment << (8 - (end - first));
    end = begin;
  }
  return value;
}

uint compressed_value_bit(uint record, uint at) {
  return compressed_source_bit(extra[record * 8 + 6], extra[record * 8 + 2] + at);
}

// Every distinct key survives replacement merge, including tombstones.
// Hence the output predecessor is >= this record's source predecessor, and
// output LCP >= its encoded retained prefix. Output suffix bits are local.
uint compressed_suffix_bit(uint record, uint full_key_bit) {
  return compressed_source_bit(extra[record * 8 + 6],
                               extra[record * 8] + full_key_bit - extra[record * 8 + 4]);
}

[numthreads(128, 1, 1)] void prefix_leaf(uint3 tid : SV_DispatchThreadID) {
  uint i = tid.x;
  uint count = parameters[0], base = parameters[6];
  if (i >= base)
    return;
  uint retained = 0xffffffffu;
  if (i < count) {
    retained = extra[i * 8 + 4];
    bool first = i == 0 || i == parameters[1];
    bool invalid = first ? retained != 0 : retained > extra[(i - 1) * 8 + 1] * 8;
    invalid = invalid || extra[i * 8 + 6] != (i < parameters[1] ? 0u : 1u);
    if (invalid) {
      uint old;
      InterlockedOr(totals[0], 16u, old);
    }
  }
  output_data[base + i] = retained;
}

    // One dispatch per tree level; children are complete before parents are read.
    // Here input_data and output_data both bind the tree, while params mean
    // {node_count, first_node}. All writes in a dispatch target distinct parents.
    [numthreads(128, 1, 1)] void prefix_reduce(uint3 tid : SV_DispatchThreadID) {
  if (tid.x >= parameters[0])
    return;
  uint node = parameters[1] + tid.x;
  output_data[node] = min(input_data[node * 2], input_data[node * 2 + 1]);
}

uint compressed_common_bytes(uint a, uint b, uint limit) {
  uint count = min(limit, min(extra[a * 8 + 1], extra[b * 8 + 1]));
  for (uint i = 0; i < count; ++i)
    if (compressed_key_byte(a, i) != compressed_key_byte(b, i))
      return i;
  return count;
}

// Sorted inputs share a prefix exactly when their endpoints share it. This
// one-off comparison permits every subsequent merge comparison to skip those
// proven bytes, without any CPU reconstruction of even the endpoint keys.
[numthreads(1, 1, 1)] void compressed_prefix(uint3 tid : SV_DispatchThreadID) {
  if (tid.x != 0)
    return;
  uint count = parameters[0], older = parameters[1], newer = parameters[2];
  if (count == 0) {
    output_data[0] = 0;
    return;
  }
  uint common = extra[1];
  if (older != 0)
    common = compressed_common_bytes(0, older - 1, common);
  if (newer != 0) {
    common = compressed_common_bytes(0, older, common);
    common = compressed_common_bytes(0, count - 1, common);
  }
  output_data[0] = common;
}
