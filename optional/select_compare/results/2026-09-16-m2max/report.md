Select alternatives on the same offsets
======================================

Measured source `960d5c578dd78438edaed256094f0fd6627a4d52`; arm64, Apple M2 Max.
315 fresh processes; one excluded warmup plus three trials each. Every valid candidate matches the same sequence and query checksums.
Numbers below are medians of three process medians. The raw CSV files, immutable-array byte counts, construction times, and per-process results are retained alongside this report.

Scope
-----

These are offset-access measurements, not complete lookup or merge benchmarks. Library-built native and fractional-index fixtures supply 96 real encoded directories. Six larger cases replay their observed gaps; three synthetic cases probe an upstream boundary. All access data are resident and warmed by qualification/construction. No filesystem work is timed. The production baseline remains unchanged.

The trusted control also bypasses mapped/native view accessors: its difference cannot be assigned entirely to validation. Sux and other directory alternatives trust their high-select structures while checking ordinal and decoded offset. Direct arrays and packed positions expect valid ordinals.

Across the 96 encoded fixture directories
---------------------------------------

Paired geometric-mean speedup over checked production EF; each sequence has equal weight. Parentheses show the minimum and maximum per-sequence speedup. This aggregates differently sized directories, so use the detailed rows for a particular workload.

| Candidate | Random throughput | Dependent latency | Offset binary search |
| --- | ---: | ---: | ---: |
| ef-trusted-control | 1.00× (0.87–1.12) | 1.02× (0.94–1.09) | 1.00× (0.88–1.07) |
| direct64 | 84.80× (33.01–123.67) | 5.04× (2.53–6.83) | 13.82× (6.80–22.36) |
| direct32 | 91.64× (29.79–125.26) | 5.23× (2.51–6.88) | 14.30× (6.63–22.29) |
| packed-absolute | 6.81× (2.90–27.43) | 3.28× (1.62–4.66) | 4.12× (2.30–6.03) |
| ef-high-direct64 | 8.50× (3.01–16.13) | 3.33× (1.56–4.47) | 4.16× (2.38–6.62) |
| ef-sub32 | 1.59× (1.06–2.50) | 1.24× (0.94–1.44) | 1.11× (0.94–1.21) |
| ef-sux-simple1 | 0.94× (0.76–1.46) | 0.93× (0.82–1.03) | 0.93× (0.82–1.03) |
| ef-sux-simple2 | 1.04× (0.81–1.42) | 0.99× (0.85–1.09) | 0.97× (0.84–1.06) |
| ef-sux-half | 1.39× (1.15–1.67) | 1.06× (0.83–1.18) | 0.99× (0.89–1.07) |
| ef-sux-half-fixed | 1.38× (1.15–1.70) | 1.06× (0.81–1.21) | 0.99× (0.83–1.08) |

Representative space/time frontier
----------------------------------

Array deltas are divided by the actual native-record count, or by borrowed-record count for an index. This is an estimated directory-array change per record, not a claimed new persisted file size. Repeated offsets remain valid. $U/n$ below describes the offset universe; it is distinct from high-bit density and from the mean gap $U/(n-1)$ in these zero-based sequences.

`case-2.raw-byte.native-output`: 524288 native records, 34954 offsets.

| Candidate | Array bytes (auxiliary) | Resident bytes | Extra bytes/record | Random ns/select | Dependent ns/select | Build ns/offset |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| ef-current | 34688 (2192) | 36840 | +0.0000 | 21.92 | 35.19 | 1.14 |
| ef-sub32 | 36880 (4384) | 40840 | +0.0042 | 13.79 | 29.40 | 1.83 |
| ef-sux-simple2 | 33224 (728) | 33464 | -0.0028 | 22.02 | 34.93 | 6.93 |
| ef-sux-half-fixed | 33904 (1408) | 34048 | -0.0015 | 16.57 | 31.63 | 4.99 |
| direct32 | 139816 (0) | 139840 | +0.2005 | 0.29 | 7.33 | 0.10 |
| packed-absolute | 91760 (0) | 91792 | +0.1089 | 3.27 | 9.59 | 0.34 |

`case-11.typed-bit.native-output`: 262144 native records, 17478 offsets.

| Candidate | Array bytes (auxiliary) | Resident bytes | Extra bytes/record | Random ns/select | Dependent ns/select | Build ns/offset |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| ef-current | 38272 (1104) | 39464 | +0.0000 | 24.32 | 33.45 | 1.28 |
| ef-sub32 | 39376 (2208) | 41416 | +0.0042 | 15.30 | 26.77 | 2.11 |
| ef-sux-simple2 | 37536 (368) | 37776 | -0.0028 | 22.78 | 32.26 | 7.02 |
| ef-sux-half-fixed | 37896 (728) | 38040 | -0.0014 | 16.34 | 29.34 | 4.96 |
| direct32 | 69912 (0) | 69936 | +0.1207 | 0.21 | 5.77 | 0.09 |
| packed-absolute | 65544 (0) | 65576 | +0.1040 | 4.01 | 9.92 | 0.39 |

`case-2.raw-byte.index-main`: 19807 borrowed records, 1322 offsets.

| Candidate | Array bytes (auxiliary) | Resident bytes | Extra bytes/record | Random ns/select | Dependent ns/select | Build ns/offset |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| ef-current | 1440 (96) | 1720 | +0.0000 | 22.86 | 35.44 | 1.13 |
| ef-sub32 | 1536 (192) | 1752 | +0.0048 | 14.37 | 27.50 | 1.87 |
| ef-sux-simple2 | 1392 (48) | 1632 | -0.0024 | 22.00 | 34.06 | 5.03 |
| ef-sux-half-fixed | 1432 (88) | 1576 | -0.0004 | 14.54 | 30.93 | 3.80 |
| direct32 | 5288 (0) | 5312 | +0.1943 | 0.20 | 5.64 | 0.12 |
| packed-absolute | 2816 (0) | 2848 | +0.0695 | 2.74 | 9.13 | 0.40 |

`case-11.typed-bit.index-main`: 9904 borrowed records, 662 offsets.

| Candidate | Array bytes (auxiliary) | Resident bytes | Extra bytes/record | Random ns/select | Dependent ns/select | Build ns/offset |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| ef-current | 1136 (48) | 1400 | +0.0000 | 22.55 | 33.79 | 1.19 |
| ef-sub32 | 1184 (96) | 1368 | +0.0048 | 11.17 | 24.27 | 2.01 |
| ef-sux-simple2 | 1136 (48) | 1376 | +0.0000 | 22.32 | 34.43 | 5.07 |
| ef-sux-half-fixed | 1136 (48) | 1280 | +0.0000 | 15.80 | 31.40 | 3.91 |
| direct32 | 2648 (0) | 2672 | +0.1527 | 0.20 | 5.56 | 0.14 |
| packed-absolute | 1744 (0) | 1776 | +0.0614 | 3.08 | 9.45 | 0.41 |

The trusted owning-array control is essentially tied with production across the real sequences. Sub32's measured layout benefit therefore remains when compared with that control; it is not explained by removing directory validation. The much larger direct-array throughput gain includes the benefit of independent tiny cached loads, while the dependent-latency column is a better bound for a serialized query chain.

Same-data space detail
----------------------

`case-2.raw-byte.native-output`: 34954 offsets, inclusive universe 1606155, EF low width 5; $U/n=45.951$ and high-bit density $n/H=0.411$.

| Candidate | Payload bytes | Auxiliary bytes | Total array bytes | Bytes/offset | Resident bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| ef-current | 32496 | 2192 | 34688 | 0.992 | 36840 |
| ef-trusted-control | 32496 | 2192 | 34688 | 0.992 | 36840 |
| direct64 | 279632 | 0 | 279632 | 8.000 | 279656 |
| direct32 | 139816 | 0 | 139816 | 4.000 | 139840 |
| packed-absolute | 91760 | 0 | 91760 | 2.625 | 91792 |
| ef-high-direct64 | 32496 | 279632 | 312128 | 8.930 | 312224 |
| ef-sub32 | 32496 | 4384 | 36880 | 1.055 | 40840 |
| ef-sux-simple1 | 32496 | 440 | 32936 | 0.942 | 33176 |
| ef-sux-simple2 | 32496 | 728 | 33224 | 0.951 | 33464 |
| ef-sux-half | 32496 | 1408 | 33904 | 0.970 | 34048 |
| ef-sux-half-fixed | 32496 | 1408 | 33904 | 0.970 | 34048 |

`case-11.typed-bit.index-main`: 662 offsets, inclusive universe 1408540, EF low width 11; $U/n=2127.704$ and high-bit density $n/H=0.491$.

| Candidate | Payload bytes | Auxiliary bytes | Total array bytes | Bytes/offset | Resident bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| ef-current | 1088 | 48 | 1136 | 1.716 | 1400 |
| ef-trusted-control | 1088 | 48 | 1136 | 1.716 | 1400 |
| direct64 | 5296 | 0 | 5296 | 8.000 | 5320 |
| direct32 | 2648 | 0 | 2648 | 4.000 | 2672 |
| packed-absolute | 1744 | 0 | 1744 | 2.634 | 1776 |
| ef-high-direct64 | 1088 | 5296 | 6384 | 9.644 | 6480 |
| ef-sub32 | 1088 | 96 | 1184 | 1.789 | 1368 |
| ef-sux-simple1 | 1088 | 32 | 1120 | 1.692 | 1360 |
| ef-sux-simple2 | 1088 | 48 | 1136 | 1.716 | 1376 |
| ef-sux-half | 1088 | 48 | 1136 | 1.716 | 1280 |
| ef-sux-half-fixed | 1088 | 48 | 1136 | 1.716 | 1280 |

Totals count array contents and padding, excluding constant file metadata. Resident bytes additionally count retained capacity and C++ objects, excluding allocator headers. Sux array bytes are not a persisted file-format size. High bits are counted once; auxiliary percentages must not be confused with total representation percentages.

Cache-scale replay
------------------

These are derived synthetic sequences. Random-throughput nanoseconds per selected offset:

| Sequence | Current | Trusted | Sub32 | Simple2 | Half fixed | Direct32 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| replay-case-11.typed-bit.index-main-22 | 32.44 | 31.97 | 23.97 | 25.39 | 21.30 | ineligible |
| replay-case-11.typed-bit.native-output-22 | 35.76 | 34.63 | 26.54 | 27.17 | 24.28 | ineligible |
| replay-case-2.raw-byte.native-output-20 | 27.67 | 26.93 | 18.02 | 22.61 | 18.90 | 1.09 |
| replay-case-2.raw-byte.native-output-22 | 31.25 | 30.20 | 20.80 | 25.18 | 21.14 | 3.22 |
| replay-case-5.typed-byte.native-output-22 | 36.17 | 34.50 | 25.08 | 28.79 | 24.54 | ineligible |
| replay-case-8.raw-bit.native-output-22 | 35.11 | 34.78 | 23.70 | 28.41 | 23.81 | ineligible |

Construction and scanning
-------------------------

`summary.json` reports whole construction and alternative index-only construction separately, in nanoseconds. The whole builder includes low/high arrays; the isolated alternative builder does not build unused production samples. Batch destruction is outside construction timing. Production EF has no isolated-directory builder entry point, represented by -1 rather than an invented comparable number. The sub32 constructor currently scans all high ones; a builder given original offsets could construct its subinventory directly. Its measured build cost is this implementation, not an intrinsic lower bound.

| Example | Current build ns/offset | Simple2 build ns/offset | Simple2 index ns/offset | Current forward ns/offset | Current indexed scan ns/offset |
| --- | ---: | ---: | ---: | ---: | ---: |
| case-2.raw-byte.native-output | 1.14 | 6.93 | 5.49 | 3.95 | 11.61 |
| case-11.typed-bit.index-main | 1.19 | 5.07 | 4.06 | 3.87 | 11.87 |
| replay-case-2.raw-byte.native-output-22 | 1.11 | 6.26 | 5.23 | 4.03 | 11.66 |

Correctness boundary and limitations
------------------------------------

The unmodified pinned Sux Half generated 3 explicit failed-verification rows (one per failing sequence/process). Those rows have no performance claim. See the README for the exact 65536-span reproducer and separately labeled correction. The full SimpleSelect candidates remain unmodified.

Results are from one Apple Silicon host. Neither Pasta nor SPIDER was measured. The experiment establishes no deployment lookup/merge crossover and no x86/PDEP speedup. The source, binary, external source, generated patch, sequence, and raw-output hashes are in the manifest files.
