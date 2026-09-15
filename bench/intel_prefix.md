Intel paths for bounded 512-bit rank prefixes
===========================================

`rank_view::rank` now has compile-time Intel SIMD paths for complete readable
512-bit runs. The allocation check still requires eight words before selecting
a vector path. A shorter final allocation uses the bounded portable prefix;
an exact run boundary returns its directory answer before reading payload.
The 512-bit population builder is unchanged.

| Target features | Prefix implementation |
| :--- | :--- |
| AVX512F + AVX512VPOPCNTDQ | Masked qword load, boundary-word mask, vector popcount and reduction |
| AVX512F + AVX512BW | Same masked data, byte nibble lookup, SAD and reduction |
| AVX2 | Two unaligned 32-byte loads, qword masks, byte nibble lookup, SAD and reduction |
| ARM NEON / other targets | Existing NEON / portable paths |

VPOPCNTDQ takes precedence over BW, then AVX2. There is no runtime feature
dispatch and no new exported compiler flag. Compile each consumer for the
instruction set it can execute.

The vector helpers accept 0–512 prefix bits and require eight readable words.
For AVX2, each selected byte contributes at most eight; adding both vectors
before SAD gives at most sixteen per byte and 512 overall. AVX-512 selects full
qword lanes plus the boundary lane, then masks the boundary's remaining bits.
At 512 bits the full mask is `0xff` and the boundary mask is zero. At zero bits
the boundary mask clears the first loaded word. No vector reads beyond the
eight-word extent. Independent source review found no mask, reduction-width or
feature-guard defect.

Validation and limits
---------------------

The production checkpoint is `1b1446c`. The focused rank suite passes under
ASan/UBSan on native ARM and translated scalar x86-64. It includes independent
prefix oracles, all bit positions, partial tails and inaccessible guard pages.
The new direct-helper assertions cover every prefix from 0 through 512 at eight
uint64 alignments for zero, all-one and random words.

All seven compile profiles pass strict Apple Clang 21 checks:

* Native ARM, scalar x86-64 and AVX2 compile and link with ASan/UBSan.
* AVX512F alone, F+VPOPCNTDQ, F+BW, and F+BW+VPOPCNTDQ compile the complete rank
  test translation unit. DQ and VL are explicitly disabled; the VPOPCNTDQ-only
  profile also disables BW, and the BW-only profile disables VPOPCNTDQ.
* Separate optimized assembly probes cover each x86 profile. Together with
  the capability probe, the driver records fourteen successful compiler calls.

The local Rosetta capability probe advertises neither AVX2 nor AVX-512. Only
the scalar x86-64 executable runs under translation; the local AVX2 and AVX-512
test executables compile but do not run. Separate native AVX2 and AVX-512 prefix experiments
on Compiler Explorer are recorded below.
The [stored-spacer M2 benchmark](rank_spacers.md) measures an earlier revision
without these Intel paths.

The VPOPCNTDQ assembly contains a mask-zeroed `vmovdqu64`, boundary masking and
`vpopcntq`; Clang narrows the per-qword counts and reduces them with SAD. The
AVX2 and BW paths contain nibble shuffles and SAD reductions. Compilation with
DQ/VL disabled confirms those extra feature sets are not needed. Compiler
vectorization may also turn the bounded scalar tail loop into masked vector
loads; its source extent remains bounded by the number of contributing words.

The checked header and committed header differ only by the two-line comment
stating the prefix helper precondition. The artifact manifest records both
hashes and verifies that removing those comment lines reproduces the tested
header exactly. The existing `rank_bounds` benchmark adapter also passes its
bounded oracle run after changing it to build the old bitmap directory from
the original words; it no longer shares incompatible packed run bytes.

Native Intel prefix experiment
------------------------------

The first two Compiler Explorer jobs executed the standalone public prefix
helpers and synthetic harness on an Intel Xeon E5-2686 v4 at its reported
2.30 GHz nominal frequency.
The capability probe reported POPCNT and AVX2; unsupported AVX-512 variants were
skipped. This public-source-only execution was explicitly authorized. No
repository checkout, application data or private workload was transferred.

This measures the prefix helper, **not complete bitmap rank**. The input bitmap
is 32 KiB, with 512 KiB of generated query seeds. Every query chooses one of 512
runs and a prefix from 0 through 512. The timer includes query generation, a
noinline function-pointer call and checksum accumulation. Per-function target
attributes allow the AVX2 implementation and scalar POPCNT implementation in one
generic x86-64 executable; AVX2 call/return transition handling is part of this
measurement. An inlined rank caller can generate different code.

Before timing, a separate bit-at-a-time oracle checks every prefix from 0 through
512 at eight uint64 offsets, across 66 zero/all-one/random patterns. A separate
oracle table then checks every generated independent and dependent query. Each
timed checksum must equal the oracle checksum. Each variant is warmed with
32,768 queries; five trials of 1,048,576 queries rotate variant order. The
shared remote host has no recorded CPU affinity, frequency control or exclusive
resource lease. The five trials share one process per compiler and are not five
independent host sessions.

The initial source used the same scalar bounded-read loop as the portable
helper, without telling its noinline body that the prefix is at most 512.
Results in ns/prefix (median, with min–max in parentheses):

| Compiler | Pattern | Scalar POPCNT | AVX2 |
| :--- | :--- | ---: | ---: |
| Clang 20.1 | Independent | 17.551 (17.130–17.697) | 9.122 (8.109–9.562) |
| Clang 20.1 | Dependent | 19.748 (19.204–20.148) | 19.778 (19.419–20.511) |
| GCC 15.2 | Independent | 19.973 (17.798–20.345) | 12.443 (12.036–13.717) |
| GCC 15.2 | Dependent | 21.116 (20.559–21.898) | 22.163 (21.127–24.068) |

Both compilers pass all oracles and timed checksums. The independent AVX2
throughput gain does not establish an equivalent dependent-latency gain. The
missing scalar range assumption also warrants a separate comparison before
using these numbers to choose an implementation for an inlined rank caller.


The follow-up source adds `scalar_bounded`, whose noinline body receives
`bits <= 512` through an unreachable out-of-range branch. All existing oracles
also cover this baseline. The Clang job again ran on Broadwell; the GCC job
landed on an Ice Lake Xeon Platinum 8375C and advertised AVX512BW and
AVX512VPOPCNTDQ. **The following rows compare variants within each job; they
are not a comparison between compilers or CPUs.**

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

All supported variants pass every oracle and timed checksum, providing native
execution evidence for AVX2, AVX512BW and AVX512VPOPCNTDQ. Both large shared-host
outliers remain in the raw data and ranges; their cause was not measured.
The original unbounded scalar remains in every follow-up trial and in the raw
results, although the table focuses on the more relevant bounded comparison.

AVX2's independent throughput gain persists against the bounded scalar, while
its dependent result is slower on Broadwell and close on Ice Lake. VPOPCNTDQ
improves both patterns on this Ice Lake run. A real rank query masks its prefix
to 0–511 and returns at zero before calling the helper; the complete caller is
therefore still a separate measurement from these 0–512 noinline helper tests.


Complete bitmap rank
--------------------

The next standalone harness copies the public rank query arithmetic, with the
same eight-byte block directory, stored 0/11/22 population lanes, superblock
lookup, run-boundary return and eight-readable-word check. Each prefix helper
is forced inline into its target-specific rank function. The timer calls the
complete rank function through the same kind of noinline function pointer used
by the existing bitmap-rank harness. It includes position generation and
checksum accumulation. This removes the extra prefix call and exposes the
actual `position & 511` range to the scalar compiler.

A bit-at-a-time oracle builds the directories independently and checks every
valid position for allocation lengths 0 through 1,088, including out-of-range
rejection. It then checks all 253,577 positions in the timing fixture and every
generated query before timing. The fixture has 31,704 bitmap bytes and 1,000
directory bytes, plus 512 KiB of query seeds. It contains a short final run;
that run follows the bounded portable path. It fits within one superblock,
so this experiment does not exercise epoch transitions or cold mapped data.

The first complete-rank source still compares target-POPCNT scalar against the
explicit SIMD targets. Its median ns/rank results were:

| Host / compiler | Variant | Independent | Dependent |
| :--- | :--- | ---: | ---: |
| Broadwell / Clang 20.1 | Scalar POPCNT | 16.416 | 18.232 |
| Broadwell / Clang 20.1 | AVX2 | 12.391 | 21.993 |
| Ice Lake / GCC 15.2 | Scalar POPCNT | 15.251 | 15.999 |
| Ice Lake / GCC 15.2 | AVX2 | 8.989 | 16.934 |
| Ice Lake / GCC 15.2 | AVX512BW | 8.094 | 16.234 |
| Ice Lake / GCC 15.2 | AVX512VPOPCNTDQ | 6.474 | 12.802 |

All supported rank variants pass the full-result oracles. The raw results
retain dependent AVX2 outliers of 116.084 ns on Broadwell and 446.954 ns on
Ice Lake. This first comparison still differs in the compiler ISA available to
the scalar body; compiling the portable body for the same target is the next
control before choosing a default.


The final control compiles the portable scalar body under exactly the same
per-function ISA flags as each explicit implementation. The prefix remains
inlined into the full rank function, so the compiler can unroll or vectorize the
portable body using those instructions. Both final jobs landed on Ice Lake;
all seven variants pass all oracles. The pairs below are within one job and ISA:

| Ice Lake compiler | ISA | Independent portable → explicit ns | Dependent portable → explicit ns |
| :--- | :--- | ---: | ---: |
| Clang 20.1 | AVX2 | 12.565 → 8.870 | 18.452 → 16.650 |
| Clang 20.1 | AVX512BW | 8.010 → 7.654 | 15.994 → 16.315 |
| Clang 20.1 | AVX512VPOPCNTDQ | 7.011 → 5.892 | 13.891 → 12.677 |
| GCC 15.2 | AVX2 | 15.217 → 9.011 | 19.441 → 17.870 |
| GCC 15.2 | AVX512BW | 15.084 → 8.059 | 20.114 → 18.063 |
| GCC 15.2 | AVX512VPOPCNTDQ | 14.981 → 6.575 | 19.953 → 15.120 |

These are five-trial medians. In this final dataset, Clang's dependent AVX2 has
two roughly 490 ns trials; GCC's independent portable BW has a 493 ns trial,
and its separate target-POPCNT baseline has a 473 ns dependent trial. Other
variation is visible in the raw data. No outlier is removed, and no causal
explanation for these stalls was measured. In particular, the Clang AVX2
median represents the middle of three ordinary and two very slow trials.

VPOPCNTDQ improves both patterns against its same-ISA portable body in both
jobs. AVX2 also improves those medians, while the earlier Broadwell comparison
against target-POPCNT scalar favored scalar for dependent queries. BW improves
throughput but costs about 2% dependent latency in this Clang run; GCC favors
its explicit implementation in both patterns. The evidence supports specific
compiler/target/workload tradeoffs rather than a universal Intel speedup.
The same-ISA complete-rank control was not run on Broadwell.


One additional Clang execution used the unchanged same-ISA source, to obtain
another naturally assigned host without selecting results by CPU. It also ran
on Ice Lake; all 70 timing rows and every oracle pass are preserved. Its paired
medians were:

| ISA | Independent portable → explicit ns | Dependent portable → explicit ns |
| :--- | ---: | ---: |
| AVX2 | 12.425 → 8.842 | 18.452 → 16.624 |
| AVX512BW | 7.983 → 7.646 | 16.041 → 16.376 |
| AVX512VPOPCNTDQ | 7.000 → 5.947 | 13.901 → 12.681 |

The portable BW dependent run includes a 488.656 ns outlier; it is retained.
This second Clang process repeats the same tradeoffs, including the small BW
dependent regression. No further host attempts were used to obtain a preferred
CPU or result.

Evidence
--------

* [Compiler commands, header/test hashes, feature probe and runtime output](results/intel_prefix_checks.json).
* [Driver](results/intel_prefix_check.py.txt), [capability probe](results/intel_prefix_caps.cc.txt)
  and [assembly probe](results/intel_prefix_assembly.cc.txt), preserved as text artifacts.
* [AVX2 assembly](results/intel_prefix_avx2.s.txt), [AVX512BW assembly](results/intel_prefix_avx512bw.s.txt)
  and [VPOPCNTDQ assembly](results/intel_prefix_vpopcnt.s.txt).
* [Artifact hashes and tested/committed header comparison](results/intel_prefix_artifacts.json).

The driver uses at most two compiler jobs and was run under the host's CPU and
build-directory resource lease. All paths and commands used are in the manifest.

Native execution records preserve the exact submitted source, request and
response; the CSV files extract every timing row without discarding outliers.
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
