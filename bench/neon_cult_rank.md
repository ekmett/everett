# NEON reductions and the Cult bitmap rank

I compare the current NEON `rank15_view::rank` with a qword-first reduction and
with the actual `cult::sim::rank_view<N>::rank` from an externally supplied
header. Every variant answers the same `rank(15*g)` boundary on the same bitmap.
The source and runner are [neon_cult_rank.cc](neon_cult_rank.cc) and
[neon_cult_rank.py](neon_cult_rank.py).

## What I measured

* `neon_byte_first` is Everett's committed `aeaaa9d` header. It loads four
  128-bit vectors, masks the selected low/high nibbles, adds byte populations,
  and finishes with `vaddlvq_u8`.
* `neon_qword_first` changes only that final reduction. After the four vectors
  have been added, it reinterprets the result as two 64-bit lanes and uses
  `vaddvq_u64`, then the existing scalar `sum_bytes` fold. Each byte is at
  most 120 before the qword addition and 240 afterward, so no cross-byte carry
  can corrupt the result. Both views share the exact class/checkpoint storage
  and use the same public `rank` and `class_at` interface.
* `cult_bitmap_rank` calls the unchanged external Cult header. Its directory
  has a 12-byte record per 512 source bits: one absolute 32-bit rank and seven
  packed 9-bit ranks at 64-bit boundaries. A query uses the directory, an optional
  population count of a full 32-bit word, and a masked 32-bit population count.
  This is the concrete CPU implementation, distinct from the earlier Poppy
  shader-layout comparison. The adapter only translates `g` to `15*g`; for the
  rank-plus-class operation it additionally counts the following 15 raw bits.

The runner reads the external Cult header in place, validates its SHA-256, and
never copies it into the package or uploads it. Its original license applies;
the benchmark's license does not relicense the external header.

| Input | Revision / SHA-256 |
| --- | --- |
| Everett base revision | `aeaaa9d9896db9aec1a003a0a5ba7b0b6174bd06` |
| Everett rank15 header | `80bbeb9777caf634abaf990863f7d93753c1c4e15e72f1537ad554b82352a34c` |
| Generated NEON qword candidate | `0eced9c87eeedf1f36e15251e221de346599a64692c90777ce25f18720a93fe7` |
| Cult checkout revision | `ce97bdaea4a1f6134225eb613acc6b0d16981b45` |
| External `src/cult/sandbox/rank.h` | `96773e2dbaa8dacd92c22173f97e63bb5bbc2604f1194541cfefe83eb1f79ed0` |

The runner extracts the pinned Everett header from Git into its ignored build
directory and generates the candidate by changing that one reduction expression.
Subsequent production-header changes therefore do not silently change this
comparison. The source SHA, compiler version, flags, and run parameters are in
[hot metadata](results/neon_cult_hot_m2max.json) and
[large metadata](results/neon_cult_large_m2max.json).

## Method and validation

I used an Apple M2 Max with 96 GiB RAM and AppleClang 21.0.0
(`clang-2100.1.1.101`), compiling with
`-std=c++20 -O3 -DNDEBUG -Wall -Wextra -Werror`. The performance-cluster cache
sizes exposed by the host are 128 KiB L1D and 16 MiB L2. The benchmark requests
user-initiated QoS and runs under the shared exclusive CPU/build resource gate.

SplitMix64 seed `0x123456789abcdef` generates the source words. The Cult raw
words are copied from those bits, and each packed class counts its corresponding
15-bit span. The independent oracle accumulates populations of the original
64-bit words; it does not read either implementation's rank directory.

The timed queries use shared deterministic arrays. Independent queries can
overlap execution; dependent queries feed the previous result into selecting
the next group. Dependent time includes that common multiply/XOR/multiply-high
selection work and function dispatch, so it is not a direct measurement of
hardware load latency. Every variant uses the same function-pointer dispatch to a noinline query wrapper. Construction, allocation, oracle checks and I/O are
outside timing. Each mode gets a warmup, and variant order rotates by trial.

`rank` returns the lower prefix. `pair_rank_plus_class` returns
`2*rank(g) + class_at(g)`, the sum of the lower and upper prefixes.
`pair_two_ranks` computes the identical result with two public rank calls.
All three operations are measured in both independent and dependent patterns.

Every actual query is validated against the oracle, including each variant's
complete dependent answer chain. Extra checks cover the early groups, final
group, middle group, and terminal rank. Timed checksums agree across variants.
The hot validation also passes ASan/UBSan. These datasets have complete 15-entry
groups; this comparison does not add new claims about partial-group tails.
A separate read-only review found no correctness blocker in the benchmark.

## Storage

This is a fixed-universe comparison: each row within a case uses identical
logical bits and query positions. I report exact encoded arrays and a separate
terminal-count field. Everett's runtime view/vector objects, allocation
bookkeeping/capacity slack, the shared query arrays, and the untimed source and
oracle buffers are excluded. `shrink_to_fit` is requested for packed vectors,
but these figures describe encoded sizes rather than allocator-resident bytes.

| Case | Logical bits | Variant | Raw or class bytes | Directory bytes | Endpoint count | Total |
| --- | ---: | --- | ---: | ---: | ---: | ---: |
| Hot | 253,440 | Packed | 8,448 | 1,056 | 8 | 9,512 |
| Hot | 253,440 | Cult bitmap | 31,680 | 5,940 | 4 | 37,624 |
| Large | 780,902,400 | Packed | 26,030,080 | 3,253,760 | 8 | 29,283,848 |
| Large | 780,902,400 | Cult bitmap | 97,612,800 | 18,302,400 | 4 | 115,915,204 |

Cult's directory costs 18.75% of the bitmap bytes, plus its terminal count.
The packed classes/checkpoints together cost 30% of the equivalent raw bitmap
bytes for these complete groups. Earlier tables that reported 9,504 packed
bytes excluded the terminal 8-byte count; this table makes that field explicit.
The shared query array is 128 KiB for the hot case and 4 MiB for the large case.
Both large encoded representations exceed the exposed 16 MiB L2; this does not
claim that every access misses every cache level. Pointer alignment is retained
in the CSV.

## Hot results

Median nanoseconds per operation, five trials of 1,048,576 queries each.
[Full hot CSV](results/neon_cult_hot_m2max.csv) retains minimum and maximum values.

| Variant | Independent rank | Independent rank + class | Independent two ranks | Dependent rank | Dependent rank + class | Dependent two ranks |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Current NEON byte first | 6.802 | 8.030 | 10.739 | 17.350 | 18.322 | 21.361 |
| NEON qword first | 5.731 | 9.583 | 11.601 | 20.192 | 20.872 | 24.131 |
| Cult bitmap rank | 7.368 | 10.611 | 11.946 | 18.612 | 21.676 | 23.507 |

Qword first improves independent single-rank throughput here. The current
byte-first implementation wins every paired and dependent comparison against
that alternative. The current NEON code therefore remains the selected
implementation; this benchmark changes no production kernel.

## Larger same-universe results

Median **[minimum, maximum]** nanoseconds per operation, three trials
of 524,288 queries. I retain the ranges because these larger timings vary.
[Full large CSV](results/neon_cult_large_m2max.csv) also includes two-rank pairs.

| Variant | Independent rank | Independent rank + class | Dependent rank | Dependent rank + class |
| --- | ---: | ---: | ---: | ---: |
| Current NEON byte first | 21.448 [19.850,22.240] | 24.092 [23.346,24.646] | 106.597 [97.593,142.198] | 114.724 [98.988,117.774] |
| NEON qword first | 17.977 [16.513,21.516] | 23.492 [21.744,24.802] | 120.373 [114.223,127.609] | 117.032 [106.141,130.735] |
| Cult bitmap rank | 18.318 [18.098,20.102] | 21.405 [21.026,21.449] | 167.289 [153.595,176.036] | 166.223 [156.725,169.526] |

Cult's independent queries remain competitive despite its larger representation;
its independent paired result is the smallest in this run. The packed
representations have markedly smaller dependent-query medians. The changes
across patterns and broad large-case ranges prevent a single universal speed
ranking. These are resident-memory microbenchmarks, not mmap fault, storage I/O,
complete lookup, or end-to-end update measurements.

## Reproduction

Run these commands through the host's usual exclusive CPU/build resource gate.
Only the external header path depends on the Cult checkout location.

```sh
python3 bench/neon_cult_rank.py hot \
  --cult-header /path/to/cult/src/cult/sandbox/rank.h \
  --output build-neon-cult-rank/hot.csv
python3 bench/neon_cult_rank.py large \
  --cult-header /path/to/cult/src/cult/sandbox/rank.h \
  --trials 3 --queries 524288 --output build-neon-cult-rank/large.csv
python3 bench/neon_cult_rank.py check \
  --cult-header /path/to/cult/src/cult/sandbox/rank.h \
  --sanitize --trials 1 --queries 32768
```
