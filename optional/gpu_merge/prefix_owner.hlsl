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
#ifdef BYTE_PROFILE
  // Byte FC never splits a byte between owners. One tree lookup supplies
  // the complete byte, without the fragment loop needed by bit prefixes.
  record = compressed_owner(record, byte_offset * 8);
  return compressed_source_byte(extra[record * 8 + 6],
      (extra[record * 8] >> 3) + byte_offset - (extra[record * 8 + 4] >> 3));
#else
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
#endif
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

// Validation is shared by the levelwise and tiled builders. The parser bounds
// key widths before either builder; no padding descriptor is ever read.
uint prefix_retained(uint i) {
  if (i >= parameters[0])
    return 0xffffffffu;
  uint retained = extra[i * 8 + 4];
  bool first = i == 0 || i == parameters[1];
  bool invalid = first ? retained != 0 : retained > extra[(i - 1) * 8 + 1] * 8;
  invalid = invalid || extra[i * 8 + 6] != (i < parameters[1] ? 0u : 1u);
  if (invalid) {
    uint old;
    InterlockedOr(totals[0], 16u, old);
  }
  return retained;
}

[numthreads(128, 1, 1)] void prefix_leaf(uint3 tid : SV_DispatchThreadID) {
  if (tid.x < parameters[6])
    output_data[parameters[6] + tid.x] = prefix_retained(tid.x);
}

// One dispatch per tree level; children are complete before parents are read.
// input_data/output_data both bind the tree; params are {node_count,first_node}.
[numthreads(128, 1, 1)] void prefix_reduce(uint3 tid : SV_DispatchThreadID) {
  if (tid.x >= parameters[0])
    return;
  uint node = parameters[1] + tid.x;
  output_data[node] = min(input_data[node * 2], input_data[node * 2 + 1]);
}

// One workgroup owns a complete binary subtree of up to 256 leaves. Its
// shared heap preserves the original global heap: every interior node is
// written, not just the tile minimum. Different groups write disjoint nodes.
// base/level and tile_span are powers of two, so there is no partial tile.
groupshared uint prefix_tile[512];

void prefix_tile_parents(uint lane, uint first_node, uint tile_span) {
  for (uint width = tile_span / 2; width != 0; width /= 2) {
    first_node /= 2;
    if (lane < width) {
      uint value = min(prefix_tile[2 * (width + lane)], prefix_tile[2 * (width + lane) + 1]);
      prefix_tile[width + lane] = value;
      output_data[first_node + lane] = value;
    }
    GroupMemoryBarrierWithGroupSync();
  }
}

// Same parameters/bindings as prefix_leaf. Dispatch ceil(base/256) groups.
[numthreads(128, 1, 1)] void prefix_leaf_tiles(uint3 lane : SV_GroupThreadID,
                                             uint3 group : SV_GroupID) {
  uint base = parameters[6], tile_span = min(base, 256u);
  uint first = group.x * 256;
  for (uint local = lane.x; local < tile_span; local += 128) {
    uint value = prefix_retained(first + local);
    prefix_tile[tile_span + local] = value;
    output_data[base + first + local] = value;
  }
  GroupMemoryBarrierWithGroupSync();
  prefix_tile_parents(lane.x, base + first, tile_span);
}

// Reads a completed heap level [level,2*level), writing its ancestors through
// at most eight more levels. Dispatch ceil(level/256) groups, then repeat with
// level/256 until the root is complete. Children/parents never alias within a
// dispatch; dispatch ordering establishes the dependency between tile passes.
[numthreads(128, 1, 1)] void prefix_reduce_tiles(uint3 lane : SV_GroupThreadID,
                                               uint3 group : SV_GroupID) {
  uint level = parameters[0], tile_span = min(level, 256u);
  uint first_node = level + group.x * 256;
  for (uint local = lane.x; local < tile_span; local += 128)
    prefix_tile[tile_span + local] = input_data[first_node + local];
  GroupMemoryBarrierWithGroupSync();
  prefix_tile_parents(lane.x, first_node, tile_span);
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
