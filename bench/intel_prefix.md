Intel paths for bounded 512-bit rank prefixes
===========================================

In the complete bitmap-rank benchmark on Ice Lake, our explicit VPOPCNTDQ path
improves both independent throughput and dependent latency over the portable
body compiled for the same ISA. AVX2 also improves those medians. AVX512BW
improves throughput, with a roughly 2% dependent-latency regression under
Clang; GCC favors the explicit BW path in both patterns.

These are five-trial medians in ns/rank. Each pair compares the portable and
explicit implementations within one compiler job and ISA, on an Intel Xeon
Platinum 8375C (2.90 GHz nominal):

| Ice Lake compiler | ISA | Independent portable → explicit ns | Dependent portable → explicit ns |
| :--- | :--- | ---: | ---: |
| Clang 20.1 | AVX2 | 12.565 → 8.870 | 18.452 → 16.650 |
| Clang 20.1 | AVX512BW | 8.010 → 7.654 | 15.994 → 16.315 |
| Clang 20.1 | AVX512VPOPCNTDQ | 7.011 → 5.892 | 13.891 → 12.677 |
| GCC 15.2 | AVX2 | 15.217 → 9.011 | 19.441 → 17.870 |
| GCC 15.2 | AVX512BW | 15.084 → 8.059 | 20.114 → 18.063 |
| GCC 15.2 | AVX512VPOPCNTDQ | 14.981 → 6.575 | 19.953 → 15.120 |

All seven variants pass the independent full-result oracles. A second Clang
process, using the unchanged source on another Ice Lake assignment, repeats
the same tradeoffs:

| ISA | Independent portable → explicit ns | Dependent portable → explicit ns |
| :--- | ---: | ---: |
| AVX2 | 12.425 → 8.842 | 18.452 → 16.624 |
| AVX512BW | 7.983 → 7.646 | 16.041 → 16.376 |
| AVX512VPOPCNTDQ | 7.000 → 5.947 | 13.901 → 12.681 |

These results support the current compile-time paths for these workloads.
They do not establish a universal Intel speedup: an earlier Broadwell run
favored target-POPCNT scalar for dependent queries, and we did not obtain a
same-ISA complete-rank measurement on Broadwell. Shared-host outliers are
retained and detailed below.

Implementation
--------------

`rank_view::rank` selects Intel SIMD for complete readable 512-bit runs. It
requires eight words before taking a vector path; a shorter final allocation
uses the bounded portable prefix. At an exact run boundary, it returns the
directory answer before reading payload. The 512-bit population builder is
unchanged.

| Target features | Prefix implementation |
| :--- | :--- |
| AVX512F + AVX512VPOPCNTDQ | Masked qword load, boundary-word mask, vector popcount and reduction |
| AVX512F + AVX512BW | Same masked data, byte nibble lookup, SAD and reduction |
| AVX2 | Two unaligned 32-byte loads, qword masks, byte nibble lookup, SAD and reduction |
| ARM NEON / other targets | Existing NEON / portable paths |

VPOPCNTDQ takes precedence over BW, then AVX2. Selection is compile-time, with
no runtime dispatch or new exported compiler flags. Compile each consumer for
the instruction set it can execute.

The vector helpers accept 0–512 bits and read within eight words. For AVX2,
each selected byte contributes at most eight; adding both vectors before SAD
gives at most sixteen per byte and 512 overall. AVX-512 selects full qword
lanes and masks the boundary word. At 512 bits the full mask is `0xff` and
the boundary mask is zero; at zero bits the mask clears the first loaded word.
Independent review found no mask, reduction-width or feature-guard defect.

Measurement and verification
----------------------------

We measure a standalone copy of the public rank arithmetic: the eight-byte
block directory, stored 0/11/22 population lanes, superblock lookup,
run-boundary return and eight-readable-word check. The prefix is forced inline
into each target-specific rank function. In the final control, the portable
body receives the same ISA flags as its explicit counterpart, allowing the
compiler to unroll or vectorize it with the actual `position & 511` bound.

The timer includes position generation, one noinline function-pointer call to
complete rank, and checksum accumulation. Each variant receives a 32,768-query
warmup followed by five trials of 1,048,576 queries in rotating variant order.
The fixture has 253,577 bits: 31,704 bitmap bytes and 1,000 directory bytes,
plus 512 KiB of query seeds. Its short final run exercises the portable tail.
It fits within one superblock; epoch transitions and cold mapped data are
outside this timing experiment.

A separate bit-at-a-time oracle builds the directories and checks every valid
position for allocation lengths 0–1,088, plus out-of-range rejection. It also
checks all 253,577 positions in the timing fixture and every generated query
before timing. Every timed checksum must equal the oracle checksum.

Compiler Explorer jobs can run on different CPUs. We compare variants within
each job; the tables do not compare compilers or CPUs. There is no recorded
CPU affinity, frequency control or exclusive-host guarantee. The five trials
share one process; the Clang repeat is a separate process.

All outliers remain in the CSV files. In the final same-ISA dataset, Clang's
dependent AVX2 has two roughly 490 ns trials; its median is the middle of
three ordinary and two very slow trials. GCC's independent portable BW has
a 493 ns trial, and its separate target-POPCNT baseline has a 473 ns dependent
trial. The Clang repeat contains a 488.656 ns portable-BW dependent trial.
Other variation and the full ranges are available in the raw data; the cause
of these stalls was not measured.

The production checkpoint `1b1446c` passes the focused rank suite under
ASan/UBSan on native ARM and translated scalar x86-64, including partial tails
and inaccessible guard pages. Direct helper oracles cover every prefix from
0 through 512 at eight uint64 alignments for zero, all-one and random words.
Native Compiler Explorer runs execute and validate AVX2, AVX512BW and
AVX512VPOPCNTDQ. Rosetta advertises neither AVX2 nor AVX-512, so local runtime
validation covers only scalar x86-64.

All seven compile profiles pass strict Apple Clang 21 checks:

* Native ARM, scalar x86-64 and AVX2 compile and link with ASan/UBSan.
* AVX512F alone, F+VPOPCNTDQ, F+BW, and F+BW+VPOPCNTDQ compile the complete
  rank test translation unit. DQ and VL are disabled; the VPOPCNTDQ-only
  profile also disables BW, and the BW-only profile disables VPOPCNTDQ.
* Optimized assembly probes cover each x86 profile. Including the capability
  probe, the driver records fourteen successful compiler calls.

VPOPCNTDQ assembly contains a mask-zeroed `vmovdqu64`, boundary masking and
`vpopcntq`; Clang narrows the qword counts and reduces them with SAD. AVX2 and
BW use nibble shuffles and SAD. The disabled-feature compile matrix confirms
that DQ/VL are unnecessary. Compiler vectorization may turn the portable tail
into bounded masked loads.

The artifact manifests record the checked and committed header hashes; their
only difference is the two-line helper-precondition comment. The `rank_bounds`
adapter passes its oracle after independently building the old packed directory.
The [stored-spacer M2 benchmark](rank_spacers.md) measures an earlier revision
without these Intel paths.

Appendix: prefix-only measurements
----------------------------------

These experiments measure the prefix helper before composition with rank.
Their bitmap is 32 KiB, with 512 KiB of query seeds. Each query selects one of
512 runs and 0–512 prefix bits. The timer includes query generation, a noinline
function-pointer call, AVX2 call/return transition handling, and checksum
accumulation. Warmup, trial count and query count match the complete-rank test.

The oracle checks all 0–512 prefixes at eight uint64 offsets across 66
zero/all-one/random patterns, then checks every generated independent and
dependent query. All supported variants pass every oracle and timed checksum.
The extra call and 0–512 range differ from the inlined complete-rank path,
which returns at zero and calls the prefix with 1–511 bits.

The initial noinline scalar loop does not receive an upper-bound assumption.
Both jobs ran on Broadwell Xeon E5-2686 v4 (2.30 GHz nominal), with POPCNT and
AVX2 available and AVX-512 skipped. Values are median ns/prefix, followed by
min–max:

| Compiler | Pattern | Scalar POPCNT | AVX2 |
| :--- | :--- | ---: | ---: |
| Clang 20.1 | Independent | 17.551 (17.130–17.697) | 9.122 (8.109–9.562) |
| Clang 20.1 | Dependent | 19.748 (19.204–20.148) | 19.778 (19.419–20.511) |
| GCC 15.2 | Independent | 19.973 (17.798–20.345) | 12.443 (12.036–13.717) |
| GCC 15.2 | Dependent | 21.116 (20.559–21.898) | 22.163 (21.127–24.068) |

The bounded source adds `scalar_bounded` with a `bits <= 512` assumption.
Clang again ran on Broadwell; GCC ran on Ice Lake with both AVX-512 variants
available. The original scalar remains in every trial and in the raw results.

| Host / compiler | Pattern | Variant | Median ns | Min–max ns |
| :--- | :--- | :--- | ---: | ---: |
| Broadwell / Clang 20.1 | Independent | Scalar bounded | 15.481 | 15.069–15.799 |
| Broadwell / Clang 20.1 | Independent | AVX2 | 8.021 | 7.254–8.612 |
| Broadwell / Clang 20.1 | Dependent | Scalar bounded | 17.361 | 16.781–245.148 |
| Broadwell / Clang 20.1 | Dependent | AVX2 | 19.726 | 19.395–20.681 |
| Ice Lake / GCC 15.2 | Independent | Scalar bounded | 16.103 | 15.761–16.742 |
| Ice Lake / GCC 15.2 | Independent | AVX2 | 11.008 | 10.610–11.565 |
| Ice Lake / GCC 15.2 | Independent | AVX512BW | 9.963 | 9.805–10.049 |
| Ice Lake / GCC 15.2 | Independent | AVX512VPOPCNTDQ | 8.188 | 8.074–9.352 |
| Ice Lake / GCC 15.2 | Dependent | Scalar bounded | 16.935 | 16.673–17.322 |
| Ice Lake / GCC 15.2 | Dependent | AVX2 | 16.773 | 16.599–290.925 |
| Ice Lake / GCC 15.2 | Dependent | AVX512BW | 17.461 | 17.011–18.203 |
| Ice Lake / GCC 15.2 | Dependent | AVX512VPOPCNTDQ | 13.795 | 13.480–14.227 |

The 245.148 ns and 290.925 ns outliers remain in the ranges and raw data.
AVX2 improves independent throughput against bounded scalar, while dependent
latency is slower on Broadwell and close on Ice Lake. VPOPCNTDQ improves both
patterns in this Ice Lake run. These helper results motivate the complete-rank
and same-ISA controls presented first.

Appendix: complete rank against target-POPCNT scalar
---------------------------------------------------

This source uses the complete-rank fixture and oracles described above, with
a scalar baseline restricted to POPCNT. Its median ns/rank results were:

| Host / compiler | Variant | Independent | Dependent |
| :--- | :--- | ---: | ---: |
| Broadwell / Clang 20.1 | Scalar POPCNT | 16.416 | 18.232 |
| Broadwell / Clang 20.1 | AVX2 | 12.391 | 21.993 |
| Ice Lake / GCC 15.2 | Scalar POPCNT | 15.251 | 15.999 |
| Ice Lake / GCC 15.2 | AVX2 | 8.989 | 16.934 |
| Ice Lake / GCC 15.2 | AVX512BW | 8.094 | 16.234 |
| Ice Lake / GCC 15.2 | AVX512VPOPCNTDQ | 6.474 | 12.802 |

All supported variants pass the full-result oracles. Dependent AVX2 outliers
of 116.084 ns on Broadwell and 446.954 ns on Ice Lake remain in the raw data.
Because this scalar baseline has different compiler ISA flags from the SIMD
paths, the final same-ISA comparison is the relevant control for a consumer
compiled with those features.

Evidence
--------

* [Compiler commands, header/test hashes, feature probe and runtime output](results/intel_prefix_checks.json).
* [Driver](results/intel_prefix_check.py.txt), [capability probe](results/intel_prefix_caps.cc.txt)
  and [assembly probe](results/intel_prefix_assembly.cc.txt), preserved as text artifacts.
* [AVX2 assembly](results/intel_prefix_avx2.s.txt), [AVX512BW assembly](results/intel_prefix_avx512bw.s.txt)
  and [VPOPCNTDQ assembly](results/intel_prefix_vpopcnt.s.txt).
* [Artifact hashes and tested/committed header comparison](results/intel_prefix_artifacts.json).

Native execution records retain the submitted source, request and response
with names normalized; the CSV files preserve every timing row without discarding outliers.
The request URLs are `https://godbolt.org/api/compiler/clang2010/compile` and
`https://godbolt.org/api/compiler/g152/compile`, with user arguments
`-O3 -std=c++20 -mpopcnt` and execution arguments `5 1048576`.

* Initial prefix: [source](results/intel_prefix_ce_initial.cc.txt),
  [CSV](results/intel_prefix_ce_initial.csv),
  [Clang request](results/intel_prefix_ce_initial_clang_request.json) /
  [response](results/intel_prefix_ce_initial_clang_result.json),
  [GCC request](results/intel_prefix_ce_initial_gcc_request.json) /
  [response](results/intel_prefix_ce_initial_gcc_result.json).
* Bounded prefix: [source](results/intel_prefix_ce_bounded.cc.txt),
  [CSV](results/intel_prefix_ce_bounded.csv),
  [Clang request](results/intel_prefix_ce_bounded_clang_request.json) /
  [response](results/intel_prefix_ce_bounded_clang_result.json),
  [GCC request](results/intel_prefix_ce_bounded_gcc_request.json) /
  [response](results/intel_prefix_ce_bounded_gcc_result.json).
* Complete rank: [source](results/intel_rank_ce_initial.cc.txt),
  [CSV](results/intel_rank_ce_initial.csv),
  [Clang request](results/intel_rank_ce_initial_clang_request.json) /
  [response](results/intel_rank_ce_initial_clang_result.json),
  [GCC request](results/intel_rank_ce_initial_gcc_request.json) /
  [response](results/intel_rank_ce_initial_gcc_result.json).
* Same-ISA complete rank: [source](results/intel_rank_ce_same_isa.cc.txt),
  [CSV](results/intel_rank_ce_same_isa.csv),
  [Clang request](results/intel_rank_ce_same_isa_clang_request.json) /
  [response](results/intel_rank_ce_same_isa_clang_result.json),
  [GCC request](results/intel_rank_ce_same_isa_gcc_request.json) /
  [response](results/intel_rank_ce_same_isa_gcc_result.json).
* Unchanged same-ISA Clang repeat: [source](results/intel_rank_ce_repeat.cc.txt),
  [CSV](results/intel_rank_ce_repeat.csv),
  [request](results/intel_rank_ce_repeat_clang_request.json) /
  [response](results/intel_rank_ce_repeat_clang_result.json).
* [Native evidence hashes and local standalone checks](results/intel_prefix_ce_artifacts.json).
