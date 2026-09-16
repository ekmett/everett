Matched fixed-key and KV03 Metal results
=======================================

The fixed formats have lower complete-path medians in 12 of 12 matched no-cancellation cases. The KV03/fixed median ratio ranges from 1.37 to 4.79×. The result depends on key distribution and value size. The tables distinguish structured keys from hash-like keys and report encoded sizes separately.

This measures two standalone Metal implementations on an Apple M2 Max. It does not establish a transaction, durability, fractional-index, cancellation, or general-purpose sort speedup. Both paths include fresh output mapping/import, CRC32C, clipping and return-time cleanup. Neither performs durable synchronization.

Complete merge latency
----------------------

Times are milliseconds: median of three process medians, followed by their minimum–maximum. Each process has three measured iterations after one excluded warmup. The fixed schedule alternates format order across the three process pairs for every case. All 216 timed rows and 72 warmups are retained.

| Keys | Values | Records/input | Fixed complete ms | KV03 complete ms | KV03 / fixed |
|---|---|---:|---:|---:|---:|
| structured | FF16 | 4,096 | 0.962 [0.873–1.162] | 4.606 [2.850–6.887] | 4.79× |
| structured | FF16 | 65,536 | 2.446 [1.734–3.339] | 5.989 [4.430–7.585] | 2.45× |
| structured | FF16 | 262,144 | 4.476 [3.176–4.845] | 15.463 [12.789–16.068] | 3.45× |
| structured | FV 0–512 | 4,096 | 2.062 [2.028–2.094] | 5.668 [3.645–6.449] | 2.75× |
| structured | FV 0–512 | 65,536 | 6.471 [6.441–7.266] | 12.309 [12.157–12.496] | 1.90× |
| structured | FV 0–512 | 131,072 | 12.836 [12.036–13.041] | 17.623 [17.433–21.403] | 1.37× |
| hash-like | FF16 | 4,096 | 1.438 [1.065–2.109] | 3.219 [2.532–4.419] | 2.24× |
| hash-like | FF16 | 65,536 | 1.498 [1.392–2.409] | 5.756 [4.869–5.825] | 3.84× |
| hash-like | FF16 | 262,144 | 3.471 [3.000–4.262] | 9.406 [9.260–12.360] | 2.71× |
| hash-like | FV 0–512 | 4,096 | 1.559 [1.514–3.998] | 3.661 [3.621–4.468] | 2.35× |
| hash-like | FV 0–512 | 65,536 | 6.371 [6.039–7.231] | 11.903 [11.045–12.993] | 1.87× |
| hash-like | FV 0–512 | 131,072 | 12.837 [12.367–13.758] | 19.136 [15.409–19.237] | 1.49× |

11 of 12 process-median ranges are disjoint in the fixed format’s favor. The other case(s) are: hash-like FV 4,096/input. Looking at all nine individual iterations instead, 8 cases have disjoint ranges; the other case(s) are: structured FV 4,096/input, hash-like FF16 4,096/input, hash-like FV 4,096/input, hash-like FV 131,072/input. These short runs show visible variability; no trial or process was discarded.

Encoded size
------------

All sizes below are exact logical file bytes, including headers, checksums, values and navigation sections. Inputs means the sum of both input files. Physical page rounding and scratch capacity are excluded from these file sizes. The fixed due-list envelope is another 32 input bytes in every case; it contains no cancellation ordinals.

| Keys | Values | Records/input | Fixed inputs | KV03 inputs | Fixed output | KV03 output | Fixed / KV03 output |
|---|---|---:|---:|---:|---:|---:|---:|
| structured | FF16 | 4,096 | 262,656 | 155,856 | 262,400 | 152,888 | 1.716 |
| structured | FF16 | 65,536 | 4,194,816 | 2,483,264 | 4,194,560 | 2,441,152 | 1.718 |
| structured | FF16 | 262,144 | 16,777,728 | 9,930,928 | 16,777,472 | 9,763,608 | 1.718 |
| structured | FV 0–512 | 4,096 | 2,239,088 | 2,128,192 | 2,238,808 | 2,125,240 | 1.053 |
| structured | FV 0–512 | 65,536 | 35,823,960 | 34,047,528 | 35,823,672 | 34,005,736 | 1.053 |
| structured | FV 0–512 | 131,072 | 71,646,648 | 68,093,640 | 71,646,360 | 68,010,440 | 1.053 |
| hash-like | FF16 | 4,096 | 262,656 | 289,080 | 262,400 | 287,712 | 0.912 |
| hash-like | FF16 | 65,536 | 4,194,816 | 4,550,784 | 4,194,560 | 4,534,352 | 0.925 |
| hash-like | FF16 | 262,144 | 16,777,728 | 18,071,312 | 16,777,472 | 18,005,776 | 0.932 |
| hash-like | FV 0–512 | 4,096 | 2,239,088 | 2,261,168 | 2,238,808 | 2,259,808 | 0.991 |
| hash-like | FV 0–512 | 65,536 | 35,823,960 | 36,111,336 | 35,823,672 | 36,094,904 | 0.992 |
| hash-like | FV 0–512 | 131,072 | 71,646,648 | 72,188,968 | 71,646,360 | 72,156,216 | 0.993 |

With structured FF16 keys, fixed output is 71.63–71.84% larger. With hash-like FF16 keys it is 6.82–8.80% smaller. The much larger FV payload reduces the size difference: fixed output is 5.34–5.35% larger for structured keys and 0.71–0.93% smaller for hash-like keys.

The same logical keys and present values are encoded by both formats. KV03 includes its ordinary single-sort selector, optional-string value framing and front coding. The experimental fixed formats use fixed-width keys with implicit or EF-addressed value lengths. This is a comparison of those actual formats, not identical wire grammars.

Internal diagnostics
--------------------

These are also medians of process medians, in milliseconds. They are **not matching timing boundaries**: fixed internal starts with imported inputs/output and excludes CRC; KV03 internal includes fresh output mapping and CRC but stops before final return cleanup. Only the complete table above compares matching boundaries. Fixed CRC is separately timed; KV03 CRC remains part of its total.

| Case | Fixed internal | KV03 internal | Fixed CRC | Fixed device | KV03 device |
|---:|---:|---:|---:|---:|---:|
| 0 | 0.845 | 4.572 | 0.014 | 0.123 | 1.599 |
| 1 | 1.930 | 5.893 | 0.217 | 0.232 | 2.256 |
| 2 | 2.945 | 15.375 | 0.909 | 0.594 | 10.203 |
| 3 | 1.680 | 5.612 | 0.129 | 0.510 | 1.365 |
| 4 | 3.252 | 12.118 | 2.416 | 1.253 | 4.952 |
| 5 | 6.152 | 17.302 | 5.057 | 2.329 | 5.952 |
| 6 | 1.268 | 3.186 | 0.014 | 0.089 | 1.006 |
| 7 | 1.015 | 5.688 | 0.212 | 0.145 | 1.936 |
| 8 | 1.763 | 9.299 | 0.926 | 0.442 | 3.531 |
| 9 | 1.171 | 3.603 | 0.127 | 0.325 | 1.338 |
| 10 | 3.107 | 11.746 | 2.520 | 1.125 | 3.187 |
| 11 | 6.552 | 18.841 | 5.165 | 1.729 | 5.093 |

Validation and scope
--------------------

- All 72 processes passed complete canonical CPU-output equality after every warmup and timed iteration. The corresponding CPU encoders were also checked against the independently merged logical fixture records.
- Canonical logical input and output SHA-256 values agree across both formats and all processes for every case. All physical output hashes are stable across processes of a format; 216 retained physical headers have independently checked counts, file extents and header CRCs.
- Eight separate serial Metal correctness CTests passed before collection. All 126 frozen source hashes and 4 artifact hashes remained unchanged through the collection.
- KV03 uses the calibrated optimized path: compressed mapped inputs, GPU EF/frame parsing, prefix-owner tree, eight-byte prefix cache, packed-word output and GPU output EF. Levelwise tree and separate output plan remain the defaults after earlier mixed experiments. Fixed uses its existing no-cancellation path and Merge Path 32. No claim is made that either implementation is an optimized limit.
- Fixture generation, sorting hash-like keys, input encoding/mapping, pipeline warmup and output verification are outside timing. There is no CPU input-key reconstruction or per-record setup inside the merge timer.
- No cancellation or tombstone case is included. Such a comparison requires matching logical updates while disclosing KV tombstone versus fixed due-ordinal physical counts.
- FV stops at 131,072 records/input because the existing KV maximum-record-bound guard rejects the next larger proposed case. The guard was preserved; this is not an observed actual-file-size limit.

Provenance
----------

Measured host source: `473099253e91012a1f03321dee5812d057a8b96b`. Collection date: 2026-09-16. Device: Apple M2 Max; backend: Metal; macOS 26.6.2 (25G83). Host wrappers use C++20 Objective-C++ with `-O3 -DNDEBUG`, ARC and strict warnings. The existing, independently qualified Metal libraries were reused after exact current HLSL-source hash checks; no shader was modified for this comparison.

See [the frozen manifest](results/manifest.json), [shader provenance](results/shader-provenance.json), [raw rows](results/results.csv), [all summaries](results/summary.json), [physical/logical checks](results/checks.json), and [schedule/host metadata](results/metadata.json). [The method and reproduction guide](README.md) defines the complete boundary and fixture mapping. Original resident-path measurements and earlier KV calibration remain unchanged.
