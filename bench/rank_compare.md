# Full-vector and packed-rank comparison

I compare the same original bit vector at the same exclusive boundaries,
`rank(15*g)`, using the full bitmap directory, packed 15-entry populations, and
the typed `rank_groups_view<15>` path. There is no RRR representation here.

The standalone [benchmark](rank_compare.cc) and [runner](rank_compare.sh) pin
the baseline headers to `7732b1ed551dccc05256964106b7091e56e65bc0` and the SIMD
rank15 header to `91bd022eaaf6334cdf6b1391389c7ea2eb706d48`. The runner extracts
those committed headers into an ignored build directory. Updating the installed
or source-tree headers therefore does not silently change the baseline.

```sh
# Run through the host's normal resource gate when one is configured.
sh bench/rank_compare.sh check 1 32768
sh bench/rank_compare.sh all 5 1048576 core > build-rank-compare/results.csv
```

The arguments are dataset (`all`, `check`, or a case name below), trials,
queries per timing batch, and optional `core`. Omitting `core` also exercises
full sequential sweeps and controlled `g mod 128` / `(15*g) mod 512` offsets.
Those additional timing modes are available but are not part of the recorded
measurement below. `check` validates the two hot datasets without timing them.

## What the variants measure

- `bitmap_rank`: the original Everett full-bit-vector `rank_view::rank`, with
  its 2048/512 directory and scalar loop over the preceding 64-bit words.
- `poppy512_neon_cpu`: a CPU translation using the 2048/512 layout from
  [my Poppy shader](https://github.com/ekmett/vr/blob/master/shaders/poppy.glsl).
  It loads all four 128-bit vectors, masks the prefix and reduces byte
  populations with NEON. The three packed ten-bit counts are summed with
  explicit parentheses. This is not literal GLSL or GPU timing; the reduction
  differs from the shader's lane prefix scan. The comparison stays below
  `2^32` source bits, and the raw allocation has zero padding sufficient for
  all four vector loads at its end.
- `rank15_baseline`: the original packed-word summation implementation.
- `rank_groups15_baseline`: the original typed path, which reads and sums
  individual classes after its checkpoint.
- `rank15_simd`: the four-vector, whole-prefix packed implementation. It shares
  the exact encoded classes and checkpoint allocation with both packed baselines.

The [original CSV](results/rank_compare_m2max.csv) also retains
`rank15_candidate`, an intermediate scalar prototype measured in the same
process. That optional prototype is not required by the runner or by the
comparisons here; the runner reproduces the five variants listed above. The
SIMD snapshot used in the measured process has SHA-256
`f11fbcfa831d1b617ede57d09bd009ba35a24fe09eee3d84a839a393553a1de4`, identical to
the pinned `91bd022` header.

## Method and checks

These measurements used an Apple M2 Max with 96 GiB RAM and AppleClang 21
(`clang-2100.1.1.101`), `-std=c++20 -O3 -DNDEBUG`. The exposed performance-cluster
L2 size was 16 MiB; the system did not expose an `hw.l3cachesize` value. The
thread requested user-initiated QoS. A shared exclusive CPU resource gate kept
the cooperating workers' builds and timings separate.

Each row records five trials of 1,048,576 queries; variant order rotates between
trials. The source bits come from deterministic SplitMix64 output. Queries are
deterministic and shared across all variants within a case. Construction,
allocation, oracle checking and file I/O are outside the timed region.

Independent random rows measure amortized nanoseconds per call, including
query-array indexing and function dispatch. The dependent row feeds each answer
into selection of the next query, so it includes the common multiply/XOR/query
selection overhead and limits overlap between successive rank operations. It
is not an isolated hardware load-latency measurement.

`pair_two_ranks` returns `rank(g) + rank(g+1)`. `pair_rank_plus_class` computes
the same result from `2*rank(g) + class_at(g)`; the bitmap version counts the
next fifteen source bits, crossing a word boundary when necessary.

An independent oracle uses raw-word population prefixes, not packed classes.
Every generated query, including the complete dependent chain, is checked
against it; timed checksums also agree across all variants. Hot-case checks of
all variants and pair modes passed ASan/UBSan. The standalone pinned-header
runner was separately compiled with strict warnings and its oracle check passed.

## Storage and cache comparisons

Byte counts are exact encoded array sizes. They include the raw bitmap's final
vector-load padding, rank directory and epoch base, or packed classes plus
checkpoints. They exclude C++ view/vector objects, allocator bookkeeping,
query arrays and the untimed oracle. Allocated vector capacities equal the
reported section sizes in this run. Both full and packed data pointers were
64-byte aligned in every recorded case; checkpoint alignment is in the CSV.

| Case | Logical bits | Full data + metadata | Packed data + metadata | Shared query array |
| --- | ---: | ---: | ---: | ---: |
| `hot_full32KiB` | 253,440 | 31,680 + 1,000 = 32,680 B | 8,448 + 1,056 = 9,504 B | 128 KiB |
| `hot_packed32KiB` | 873,600 | 109,248 + 3,424 = 112,672 B | 29,120 + 3,640 = 32,760 B | 256 KiB |
| `large_full96MiB` | 780,902,400 | 97,612,800 + 3,050,408 = 100,663,208 B | 26,030,080 + 3,253,760 = 29,283,840 B | 8 MiB |
| `large_packed96MiB` | 2,684,353,920 | 335,544,256 + 10,485,768 = 346,030,024 B | 89,478,464 + 11,184,808 = 100,663,272 B | 8 MiB |

Within each case, logical size and query positions are identical. Comparing
full storage in a `full` case with packed storage in the corresponding `packed`
case instead holds storage near 32 KiB or 96 MiB while increasing the packed
logical universe. We should keep those two comparisons separate. The largest
case places both representations well beyond the exposed 16 MiB CPU cache.

## Recorded results

The hot `hot_full32KiB` case below uses the same 253,440 bits for every row.
All entries are median nanoseconds per operation; the CSV preserves minima
and maxima, including outliers in the pair column.

| Variant | Random rank | Two-rank pair | Rank + class pair | Dependent random rank |
| --- | ---: | ---: | ---: | ---: |
| Full bitmap | 14.861 | 23.501 | 18.577 | 23.255 |
| Poppy-layout NEON CPU | 4.888 | 9.521 | 7.336 | 18.765 |
| Original rank15 | 15.782 | 28.241 | 18.152 | 19.963 |
| Original typed K=15 | 43.783 | 78.672 | 45.561 | 45.528 |
| Whole-prefix packed SIMD | 5.375 | 11.276 | 6.558 | 17.119 |

The full-vector SIMD comparison matters: it is faster than packed SIMD for
independent single-rank calls in this hot case. Packed SIMD retains the smaller
representation and has the smaller pair and dependent-query medians here.
The old typed two-rank pair and the new packed rank-plus-class pair measure
different algorithms for the same two-boundary result; their 78.672 versus
6.558 ns is not a claim about an entire blob lookup.

Large resident-memory results were noisy even with explicit QoS. Below I retain
median **[minimum, maximum]** rather than turning these data into a general
performance ranking. These spreads also prevent treating the independent and
dependent columns as a clean hardware latency decomposition.

| Case / variant | Independent rank, ns | Dependent rank, ns |
| --- | ---: | ---: |
| 96 MiB full / bitmap | 68.042 [66.812, 69.525] | 286.890 [150.485, 314.094] |
| 96 MiB full / Poppy CPU | 50.452 [43.622, 50.550] | 214.396 [170.665, 302.246] |
| 96 MiB full / packed SIMD | 39.748 [31.445, 41.378] | 137.298 [102.283, 171.042] |
| 96 MiB packed / bitmap | 39.272 [33.530, 77.190] | 223.048 [146.319, 313.036] |
| 96 MiB packed / Poppy CPU | 27.145 [24.768, 57.111] | 167.843 [156.076, 251.630] |
| 96 MiB packed / packed SIMD | 27.174 [26.726, 38.231] | 201.521 [134.779, 322.577] |

The original data retain all baselines, paired operations, both approximately
equal-footprint cases and every trial range. These are resident-memory
microbenchmarks, without mmap page faults, storage I/O, complete catalog
searches or end-to-end update scheduling.
