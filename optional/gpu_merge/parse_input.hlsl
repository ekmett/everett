// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
// Include after kernels.hlsl's resource declarations. Input0 is a mapped
// sourcefile; output1 is8-u32 descriptors; totals2 is{errors,max_record_bits};
// parameters3 is{count,record_base,source,blocks,payload_byte_delta,extent_bits};
// references5 is12 scalar words: low byte-offset/word-count, high offset/count,
// sample offset/count, sparse offset/count, universe, low-width, fixed-value
// stride, high-bit extent. Offsets address the imported original file mapping.
// One invocation parses one15-record block; no CPU record walk is required.
// File sections may be unaligned within a mapping. All section loads below are
// checked against their declared lengths before reading these little-endian words.
uint parse_file_u32(uint at) {
  uint shift = (at & 3) * 8;
  uint value = input_data[at >> 2] >> shift;
  if (shift)
    value |= input_data[(at >> 2) + 1] << (32 - shift);
  return value;
}
uint2 parse_file_u64(uint at) { return uint2(parse_file_u32(at), parse_file_u32(at + 4)); }
bool parse_small_u64(uint at, out uint value) {
  uint2 words = parse_file_u64(at);
  value = words.x;
  return words.y == 0;
}
uint parse_select_word(uint value, uint ordinal) {
  uint result = 0;
  for (uint width = 16; width; width >>= 1) {
    uint lower = value & ((1u << width) - 1), population = countbits(lower);
    if (ordinal >= population) {
      result += width;
      ordinal -= population;
      value >>= width;
    } else
      value = lower;
  }
  return result;
}
bool parse_ef_select(uint ordinal, out uint value) {
  value = 0;
  if (ordinal > parameters[3] || (ordinal >> 8) >= references[5])
    return false;
  uint sample_at = references[4] + (ordinal >> 8) * 16;
  uint2 sample_first = parse_file_u64(sample_at), sparse = parse_file_u64(sample_at + 8);
  uint remaining = ordinal & 255, position = 0;
  if (any(sparse != uint2(0xffffffffu, 0xffffffffu))) {
    if (sparse.y || sparse.x >= references[7] || remaining >= references[7] - sparse.x)
      return false;
    if (!parse_small_u64(references[6] + (sparse.x + remaining) * 8, position) ||
        position >= references[11])
      return false;
    uint word = position >> 6;
    if (word >= references[3])
      return false;
    uint2 high = parse_file_u64(references[2] + word * 8);
    uint bit = position & 63;
    if (!((bit < 32 ? high.x : high.y) & (1u << (bit & 31))))
      return false;
  } else {
    if (sample_first.y || sample_first.x >= references[11])
      return false;
    uint word = sample_first.x >> 6, shift = sample_first.x & 63;
    bool found = false;
    // Same65-word /4096-bit dense bound as elias_fano_view::select_high.
    for (uint scanned = 0; scanned < 65 && word < references[3]; ++scanned, ++word) {
      uint2 high = parse_file_u64(references[2] + word * 8);
      if (!scanned) {
        if (shift < 32)
          high.x &= 0xffffffffu << shift;
        else {
          high.x = 0;
          high.y &= 0xffffffffu << (shift - 32);
        }
      }
      uint lower = countbits(high.x), population = lower + countbits(high.y);
      if (remaining < population) {
        uint offset = remaining < lower ? parse_select_word(high.x, remaining)
                                        : 32 + parse_select_word(high.y, remaining - lower);
        position = word * 64 + offset;
        if (position >= references[11] || position - sample_first.x >= 4096)
          return false;
        found = true;
        break;
      }
      remaining -= population;
    }
    if (!found)
      return false;
  }
  if (position < ordinal)
    return false;
  uint high_value = position - ordinal, width = references[9], low = 0;
  if (high_value > (references[8] >> width))
    return false;
  if (width) {
    uint bit = ordinal * width, word = bit >> 5, shift = bit & 31;
    if (word >= references[1] * 2)
      return false;
    low = parse_file_u32(references[0] + word * 4) >> shift;
    if (shift + width > 32) {
      if (word + 1 >= references[1] * 2)
        return false;
      low |= parse_file_u32(references[0] + (word + 1) * 4) << (32 - shift);
    }
    low &= (1u << width) - 1;
  }
  value = (high_value << width) | low;
  return value <= references[8];
}
bool parse_block_offset(uint block, out uint at) {
  if (!parse_ef_select(block, at))
    return false;
  uint ordinal = block == parameters[3] ? parameters[0] : block * 15;
  // Host-validated metadata bounds count*stride by the payload extent.
  uint fixed = ordinal * references[10];
  if (at > parameters[5] || fixed > parameters[5] - at)
    return false;
  at += fixed;
  return true;
}
uint parse_source_bit(uint at) {
  uint byte_at = parameters[4] + (at >> 3);
  uint byte_value = (input_data[byte_at >> 2] >> ((byte_at & 3) * 8)) & 255;
  return (byte_value >> (7 - (at & 7))) & 1;
}
bool parse_eg0(inout uint at, uint end, out uint value) {
  value = 0;
  uint zeros = 0;
  while (true) {
    if (at >= end)
      return false;
    uint bit = parse_source_bit(at++);
    if (bit)
      break;
    if (++zeros > 31)
      return false;
  }
  if (zeros > end - at)
    return false;
  uint encoded = 1;
  for (uint i = 0; i < zeros; i++)
    encoded = (encoded << 1) | parse_source_bit(at++);
  value = encoded - 1;
  return true;
}
void parse_failure(uint code) {
  uint ignored;
  InterlockedOr(totals[0], code, ignored);
}
[numthreads(128, 1, 1)] void parse_input(uint3 tid : SV_DispatchThreadID) {
  uint block = tid.x, count = parameters[0], blocks = parameters[3];
  if (block >= blocks)
    return;
  uint first = block * 15;
  if (first >= count) {
    parse_failure(1);
    return;
  }
  uint at, end;
  if (!parse_block_offset(block, at) || !parse_block_offset(block + 1, end)) {
    parse_failure(64);
    return;
  }
  if (at >= end || end > parameters[5] || (!block && at) ||
      (block + 1 == blocks && end != parameters[5])) {
    parse_failure(1);
    return;
  }
  uint previous_key_bits = 0;
  for (uint local = 0; local < min(15u, count - first); local++) {
    uint control;
    if (!parse_eg0(at, end, control)) {
      parse_failure(2);
      return;
    }
    uint retained;
    if (!local) {
      if (!control || control - 1 > (1u << 30) || (!block && control != 1)) {
        parse_failure(4);
        return;
      }
      retained = control - 1; // Absolute retention includes the code0 selector.
    } else {
      if (control > previous_key_bits) {
        parse_failure(4);
        return;
      }
      retained = previous_key_bits - control;
    }
    uint suffix;
    if (!parse_eg0(at, end, suffix) || suffix > end - at || suffix > (1u << 30) - retained) {
      parse_failure(8);
      return;
    }
    uint key_bits = retained + suffix, literal = at;
    if (key_bits & 7) {
      parse_failure(8);
      return;
    }
    at += suffix;
    uint value_at = at;
    if (at >= end) {
      parse_failure(16);
      return;
    }
    if (parse_source_bit(at++)) {
      uint bytes;
      if (!parse_eg0(at, end, bytes) || bytes > ((end - at) >> 3)) {
        parse_failure(16);
        return;
      }
      at += bytes << 3;
    }
    uint value_bits = at - value_at;
    if (value_bits > (1u << 30) || (references[10] && value_bits != references[10])) {
      parse_failure(16);
      return;
    }
    uint output = (parameters[1] + first + local) * 8;
    output_data[output] = literal;
    output_data[output + 1] = key_bits >> 3;
    output_data[output + 2] = value_at;
    output_data[output + 3] = value_bits;
    output_data[output + 4] = retained;
    output_data[output + 5] = 0;
    output_data[output + 6] = parameters[2];
    output_data[output + 7] = 0;
    uint ignored;
    InterlockedMax(totals[1], key_bits + value_bits + 126, ignored);
    previous_key_bits = key_bits;
  }
  if (at != end)
    parse_failure(32);
}

    // Correctness-only entry: synthetic EF sections exercise selection independently
    // of record grammar. The timed merge path invokes only parse_input above.
    [numthreads(128, 1, 1)] void parse_ef_probe(uint3 tid : SV_DispatchThreadID) {
  if (tid.x >= parameters[0])
    return;
  uint value;
  if (!parse_ef_select(tid.x, value)) {
    parse_failure(64);
    return;
  }
  output_data[tid.x] = value;
}
