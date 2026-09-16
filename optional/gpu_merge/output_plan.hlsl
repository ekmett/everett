// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
// Fuse constant output metadata and EF sparse counts after the size scan.
// input0=record offsets, output1=sparse counts, extra4=record lengths,
// references5=compacted descriptors, other7=descriptors (4 or8 u32 each).
// totals2 and frames6 alias one status packet: word0 is the completed value
// width mismatch flag; group0 writes words1..4={bits,common,U,low_width}.
// params0=nonzero surviving records, params1=descriptor stride. The earlier
// checked max-record-bits * input-count bound is <2^31, so sums/products fit.
// Host checks the returned plan before allocating the exact output mapping.
[numthreads(128, 1, 1)] void ef_output_sparse_plan(uint3 tid : SV_DispatchThreadID) {
  uint records = parameters[0];
  if (records == 0)
    return;
  uint count = (records + 14) / 15 + 1;
  uint group = tid.x;
  if (group >= (count + 255) / 256)
    return;
  uint bits = input_data[records - 1] + extra[records - 1];
  uint common = frames[0] != 0 ? 0 : other_data[references[0] * parameters[1] + 3];
#ifdef BYTE_PROFILE
  // Shared input descriptors retain bit lengths; the byte output scan and
  // residual universe count bytes.
  common >>= 3;
#endif
  uint universe = bits - records * common;
  uint quotient = universe / count;
  uint width = quotient != 0 ? firstbithigh(quotient) : 0;
  if (group == 0) {
    totals[1] = bits;
    totals[2] = common;
    totals[3] = universe;
    totals[4] = width;
  }
  uint first = group * 256, end = min(first + 256, count);
  uint begin_ordinal = min(first * 15, records), end_ordinal = min((end - 1) * 15, records);
  uint begin_physical = begin_ordinal == records ? bits : input_data[begin_ordinal];
  uint begin_residual = begin_physical - begin_ordinal * common;
  uint end_physical = end_ordinal == records ? bits : input_data[end_ordinal];
  uint end_residual = end_physical - end_ordinal * common;
  uint span = (end_residual >> width) - (begin_residual >> width) + end - first - 1;
  output_data[group] = span >= 4096 ? end - first : 0;
}
