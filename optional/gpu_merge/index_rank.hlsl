// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
// Isolated rank pass for a completed stable three-way augmented index order.
// input_data[i] is 0=native, 1=main borrow, 2=secondary borrow. Equal-key
// occurrences, including false borrows, MUST remain in this stream.
// params[0] is the virtual occurrence count, bounded below 2^24 by the host.

uint index_rank_words(uint count) {
  uint groups = (count + 14) / 15;
  return ((groups + 15) / 16) * 2;
}

// Each invocation owns one u32 half of each route's packed class section.
// Output layout: [main class words][secondary class words], including the
// canonical final u64 padding. Each source occurrence is read once overall.
// totals[0] is a zero-initialized error flag, checked before using the result.
[numthreads(128, 1, 1)] void index_rank_classes(uint3 tid : SV_DispatchThreadID) {
  uint word = tid.x, count = parameters[0], words = index_rank_words(count);
  if (word >= words)
    return;
  uint main = 0, secondary = 0;
  bool invalid = false;
  for (uint nibble = 0; nibble < 8; ++nibble) {
    uint first = (word * 8 + nibble) * 15;
    uint main_count = 0, secondary_count = 0;
    for (uint at = first; at < min(first + 15, count); ++at) {
      uint origin = input_data[at];
      main_count += origin == 1 ? 1 : 0;
      secondary_count += origin == 2 ? 1 : 0;
      invalid = invalid || origin > 2;
    }
    main |= main_count << (4 * nibble);
    secondary |= secondary_count << (4 * nibble);
  }
  output_data[word] = main;
  output_data[words + word] = secondary;
  if (invalid) {
    uint old;
    InterlockedOr(totals[0], 1u, old);
  }
}

uint index_rank_nibble_sum(uint value) {
  value = (value & 0x0f0f0f0fu) + ((value >> 4) & 0x0f0f0f0fu);
  value = (value & 0x00ff00ffu) + ((value >> 8) & 0x00ff00ffu);
  return (value & 65535) + (value >> 16);
}

// input_data binds the completed packed class sections. output_data and
// totals receive independent main/secondary totals for each 128-class block.
// Exclusive-scan each result, then rank15_finish writes LE64 checkpoints.
[numthreads(128, 1, 1)] void index_rank_blocks(uint3 tid : SV_DispatchThreadID) {
  uint block = tid.x, count = parameters[0], groups = (count + 14) / 15;
  if (block >= (groups + 127) / 128)
    return;
  uint words = index_rank_words(count), main = 0, secondary = 0;
  for (uint word = block * 16; word < min(block * 16 + 16, words); ++word) {
    main += index_rank_nibble_sum(input_data[word]);
    secondary += index_rank_nibble_sum(input_data[words + word]);
  }
  output_data[block] = main;
  totals[block] = secondary;
}
