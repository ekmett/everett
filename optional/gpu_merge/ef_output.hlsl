// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
// Canonical OUTPUT Elias--Fano from GPU-exclusive record bit offsets.
// Bindings inherited from kernels.hlsl:
//   input_data[0]: u32 offset per surviving record (no EOF entry required).
//   output_data[1]: sparse counts, or final mapped u32 output words.
//   extra[4]: u32 sparse counts per 256-entry EF sample group.
//   references[5]: exclusive scan of those sparse counts.
// Params[3]: {records, payload_bits, common_value_bits_or_0, low_width,
//             destination_u32_offset}. Width and all extents are checked by
// the host using constant-sized layout metadata before dispatch.
// All mathematical values/positions fit u32, but wire words remain LE u64.

uint ef_output_count() { return (parameters[0] + 14) / 15 + 1; }

uint ef_output_residual(uint index) {
  uint ordinal = min(index * 15, parameters[0]);
  uint physical = ordinal == parameters[0] ? parameters[1] : input_data[ordinal];
  return physical - ordinal * parameters[2];
}

uint ef_output_position(uint index) { return (ef_output_residual(index) >> parameters[3]) + index; }

uint ef_output_mask(uint width) { return width == 32 ? 0xffffffffu : (1u << width) - 1; }

// The caller scans these counts with its existing GPU exclusive scan, and
// reads only last_start+last_count to determine the sparse section extent.
[numthreads(128, 1, 1)] void ef_output_sparse_count(uint3 tid : SV_DispatchThreadID) {
  uint group = tid.x, count = ef_output_count();
  if (group >= (count + 255) / 256)
    return;
  uint first = group * 256, end = min(first + 256, count);
  uint span = ef_output_position(end - 1) - ef_output_position(first);
  output_data[group] = span >= 4096 ? end - first : 0;
}

    // Each invocation owns one u32 half of a canonical u64 word, including the
    // otherwise-unused high half and all final section padding bits.
    [numthreads(128, 1, 1)] void ef_output_low(uint3 tid : SV_DispatchThreadID) {
  uint word = tid.x, count = ef_output_count(), width = parameters[3];
  if (width == 0 || word >= ((count * width + 63) / 64) * 2)
    return;
  uint bit = word * 32, index = bit / width, offset = bit % width;
  uint result = 0, written = 0;
  while (written < 32 && index < count) {
    uint take = min(32 - written, width - offset);
    uint fragment = (ef_output_residual(index) >> offset) & ef_output_mask(take);
    result |= fragment << written;
    written += take;
    ++index;
    offset = 0;
  }
  output_data[parameters[4] + word] = result;
}

[numthreads(128, 1, 1)] void ef_output_high(uint3 tid : SV_DispatchThreadID) {
  uint word = tid.x, count = ef_output_count();
  uint high_bits = (ef_output_residual(count - 1) >> parameters[3]) + count;
  if (word >= ((high_bits + 63) / 64) * 2)
    return;
  uint first_bit = word * 32, lo = 0, hi = count;
  while (lo < hi) {
    uint mid = lo + (hi - lo) / 2;
    if (ef_output_position(mid) < first_bit)
      lo = mid + 1;
    else
      hi = mid;
  }
  uint result = 0;
  for (uint index = lo; index < count; ++index) {
    uint position = ef_output_position(index);
    if (position - first_bit >= 32)
      break;
    result |= 1u << (position - first_bit);
  }
  output_data[parameters[4] + word] = result;
}

    [numthreads(128, 1, 1)] void ef_output_samples(uint3 tid : SV_DispatchThreadID) {
  uint group = tid.x;
  if (group >= (ef_output_count() + 255) / 256)
    return;
  uint at = parameters[4] + group * 4;
  bool sparse = extra[group] != 0;
  output_data[at] = ef_output_position(group * 256);
  output_data[at + 1] = 0;
  output_data[at + 2] = sparse ? references[group] : 0xffffffffu;
  output_data[at + 3] = sparse ? 0 : 0xffffffffu;
}

[numthreads(128, 1, 1)] void ef_output_sparse(uint3 tid : SV_DispatchThreadID) {
  uint index = tid.x;
  if (index >= ef_output_count())
    return;
  uint group = index >> 8;
  if (extra[group] == 0)
    return;
  uint at = parameters[4] + (references[group] + (index & 255)) * 2;
  output_data[at] = ef_output_position(index);
  output_data[at + 1] = 0;
}
