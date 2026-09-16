// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
// Exact word-owned FC output, gathering literal/value spans a word at a time.
// Resource and descriptor ABI matches compressed_merge_emit.

uint word_emit_swap(uint value) {
  return ((value & 255u) << 24) | ((value & 65280u) << 8) | ((value >> 8) & 65280u) | (value >> 24);
}

uint word_emit_mask(uint count) { return count == 32 ? 0xffffffffu : 0xffffffffu << (32 - count); }

// Return count valid bits at the high end of the result. The parser has
// validated the source extent, and the host retains its page-rounded mapping.
uint word_emit_source(uint source, uint bit_offset, uint count) {
  uint byte_offset = parameters[4 + source] + (bit_offset >> 3);
  uint word = byte_offset >> 2;
  uint shift = ((byte_offset & 3) << 3) + (bit_offset & 7);
  uint first = source == 0 ? input_data[word] : other_data[word];
  uint result = word_emit_swap(first) << shift;
  if (shift + count > 32) {
    uint second = source == 0 ? input_data[word + 1] : other_data[word + 1];
    result |= word_emit_swap(second) >> (32 - shift);
  }
  return result & word_emit_mask(count);
}

uint word_emit_eg_size(uint value) { return 2 * firstbithigh(value + 1) + 1; }

// An EG0 code is leading zeros followed by value+1. Extracting a window
// therefore needs only a bounded right shift and a mask, even across the
// leading-zero boundary. No 64-bit integer operations are needed.
uint word_emit_eg(uint value, uint size, uint offset, uint count) {
  uint shift = size - offset - count;
  uint bits = shift >= 32 ? 0 : (value + 1) >> shift;
  if (count < 32)
    bits &= (1u << count) - 1;
  return bits << (32 - count);
}

// Read at most capacity bits from one grammar component. The caller advances
// across component and record boundaries; no invocation shares an output word.
uint word_emit_part(uint record, uint ordinal, uint offset, uint capacity, out uint count) {
  uint control = frames[ordinal * 3];
  uint common = frames[ordinal * 3 + 1];
  uint suffix = frames[ordinal * 3 + 2];
  uint control_size = word_emit_eg_size(control);
  if (offset < control_size) {
    count = min(capacity, control_size - offset);
    return word_emit_eg(control, control_size, offset, count);
  }
  offset -= control_size;
  uint suffix_size = word_emit_eg_size(suffix);
  if (offset < suffix_size) {
    count = min(capacity, suffix_size - offset);
    return word_emit_eg(suffix, suffix_size, offset, count);
  }
  offset -= suffix_size;
  uint source = extra[record * 8 + 6];
  if (offset < suffix) {
    count = min(capacity, suffix - offset);
    // All distinct keys survive this replacement merge. The output LCP is
    // at least the source retained prefix, so this suffix is a local span.
    uint source_bit = extra[record * 8] + common - extra[record * 8 + 4] + offset;
    return word_emit_source(source, source_bit, count);
  }
  offset -= suffix;
  count = min(capacity, extra[record * 8 + 3] - offset);
  return word_emit_source(source, extra[record * 8 + 2] + offset, count);
}

[numthreads(128, 1, 1)] void compressed_merge_emit_words(uint3 tid : SV_DispatchThreadID) {
  uint word = tid.x;
  uint bits = parameters[1], records = parameters[0], start = word * 32;
  if (start >= bits)
    return;
  uint lo = 0, hi = records;
  while (lo < hi) {
    uint mid = (lo + hi) >> 1;
    if (totals[mid] <= start)
      lo = mid + 1;
    else
      hi = mid;
  }
  uint ordinal = lo - 1;
  uint value = 0, filled = 0, limit = min(32u, bits - start);
  while (filled < limit) {
    uint position = start + filled;
    if (ordinal + 1 < records && position == totals[ordinal + 1])
      ++ordinal;
    uint next = ordinal + 1 < records ? totals[ordinal + 1] : bits;
    uint capacity = min(limit - filled, next - position);
    uint count;
    uint part =
        word_emit_part(references[ordinal], ordinal, position - totals[ordinal], capacity, count);
    value |= part >> filled;
    filled += count;
  }
  output_data[parameters[2] + word] = word_emit_swap(value);
}

    // Correctness-only extraction probe. Descriptors are {kind,value,offset,count};
    // kind zero selects source bits and kind one selects an EG0 code window.
    [numthreads(128, 1, 1)] void word_emit_probe(uint3 tid : SV_DispatchThreadID) {
  uint i = tid.x;
  if (i >= parameters[0])
    return;
  uint kind = extra[i * 4], value = extra[i * 4 + 1];
  uint offset = extra[i * 4 + 2], count = extra[i * 4 + 3];
  output_data[i] = kind == 0 ? word_emit_source(0, value, count)
                             : word_emit_eg(value, word_emit_eg_size(value), offset, count);
}
