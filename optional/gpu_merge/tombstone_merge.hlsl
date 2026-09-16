// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
// Three words per ORIGINAL record: physical key donor, cancellation redirect,
// retained key-bit limit (selector excluded). Values always use the winner.
// These descriptors are metadata, not reconstructed keys. Input identities are
// encoded by the disjoint record ranges and each descriptor's source field.
[[vk::binding(10)]] StructuredBuffer<uint> tombstone_data;

bool tombstone_record(uint record) { return compressed_value_bit(record, 0) == 0; }

[numthreads(128, 1, 1)] void tombstone_initialize(uint3 tid : SV_DispatchThreadID) {
  uint record = tid.x;
  if (record >= parameters[0]) return;
  frames[record * 3] = record;
  frames[record * 3 + 1] = record;
  frames[record * 3 + 2] = tombstone_record(record) ? extra[record * 8 + 4] : 0xffffffffu;
}

// Newer keys are unique, so each thread owns one newer descriptor and at most
// one older redirect. This pass follows initialization in the same command.
[numthreads(128, 1, 1)] void tombstone_redirect(uint3 tid : SV_DispatchThreadID) {
  uint ordinal = tid.x, older = parameters[1];
  if (ordinal >= parameters[2]) return;
  uint record = older + ordinal;
  if (!tombstone_record(record)) return;
  uint lo = 0, hi = older;
  while (lo < hi) {
    uint mid = (lo + hi) >> 1;
    if (compare_records(mid, record) < 0) lo = mid + 1;
    else hi = mid;
  }
  if (lo == older || compare_records(lo, record) != 0) return;
  uint previous = extra[lo * 8 + 4], incoming = extra[record * 8 + 4];
  frames[record * 3] = incoming <= previous ? record : lo;
  frames[record * 3 + 2] = min(previous, incoming);
  frames[lo * 3 + 1] = record;
}
