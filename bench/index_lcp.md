Index construction with carried LCPs
====================================

The complete three-stage in-memory pipeline takes 14.5–17.7% less time on the
4096-byte shared-prefix fixtures in this M2 Max run. All four long-prefix
observation ranges are disjoint. Short-key results are mixed and noisy: the
byte/interleaved pipeline median rises 7.4% in the first run and 4.6% in an
unchanged repeat, while the unchanged sample traversal control rises 10.0% and
5.3%, respectively. These observations do not establish a short-key speedup or
prove that the short-key cost is unchanged.

What changed
------------

The comparison is `79ba498b808e99734515621152a7759a181a0d23` →
`370687ac12a8fb94085cc6e023949694377d7de5`. The first patch reuses the coded
sample decoder's checked comparison. The second carries exact bit LCPs between
the merged frontier and both heads, and uses minima of adjacent LCPs for cut
metadata and outgoing samples. Internal borrowed/output framing consumes those
already known prefixes. Public checked entry points, occurrence ordering,
backpressure, counters, and encoded bytes are preserved. The sampler itself is
unchanged. This measures both patches together, not their separate contributions.

Complete pipeline timing
------------------------

Units are nanoseconds per augmented source occurrence, not per output record.
Each cell summarizes 15 observations (five fresh-process trials, three rounds).
Negative change means less elapsed time. These are descriptive ranges, not
confidence intervals; rounds within a process are not independent experiments.

| Prefix bytes | Profile / fixture | Baseline median [min, max] | Candidate median [min, max] | Change |
|---:|---|---:|---:|---:|
| 0 | byte / interleaved | 43.25 [41.38, 62.81] | 46.45 [40.92, 88.92] | +7.4% |
| 0 | bit / interleaved | 78.87 [74.01, 100.90] | 77.41 [70.82, 113.75] | -1.8% |
| 0 | byte / duplicates | 44.38 [40.10, 51.87] | 42.64 [38.87, 65.64] | -3.9% |
| 0 | bit / duplicates | 70.19 [65.05, 78.10] | 68.02 [63.87, 85.16] | -3.1% |
| 4096 | byte / interleaved | 212.48 [210.43, 230.94] | 174.97 [168.56, 181.07] | -17.7% |
| 4096 | bit / interleaved | 243.91 [240.88, 258.43] | 203.45 [197.78, 211.11] | -16.6% |
| 4096 | byte / duplicates | 207.14 [202.31, 225.95] | 172.66 [167.46, 181.98] | -16.6% |
| 4096 | bit / duplicates | 231.07 [227.52, 242.13] | 197.61 [191.57, 216.59] | -14.5% |

All four short-prefix ranges overlap. A single repeat of just those fixtures
used identical source, headers, commands and settings; both runs are retained:

| Prefix bytes | Profile / fixture | Baseline median [min, max] | Candidate median [min, max] | Change |
|---:|---|---:|---:|---:|
| 0 | byte / interleaved | 42.27 [41.16, 85.35] | 44.23 [41.16, 63.85] | +4.6% |
| 0 | bit / interleaved | 77.93 [71.45, 114.52] | 76.17 [72.61, 92.00] | -2.3% |
| 0 | byte / duplicates | 42.52 [40.09, 54.92] | 42.32 [38.87, 49.14] | -0.5% |
| 0 | bit / duplicates | 69.23 [65.19, 79.53] | 69.82 [64.23, 75.00] | +0.9% |

Every repeat range overlaps too. The other short pipeline medians move between
−3.9% and +0.9% across the two runs. Long-prefix sample controls move between
−0.2% and +1.4%, with overlapping ranges. No additional reruns were selected.

Method and correctness
----------------------

The unchanged [harness](sample_frontier.cc) and [runner](sample_frontier.py) build
4096 native source records with either interleaved borrowed records or repeated
borrowed records including false borrows, for byte and bit profiles. The bit
profile also has a non-byte-aligned suffix. Each pipeline traverses the source
and constructs three index stages. Inputs, reference results and a warm-up are
prepared before timing. The timed interval includes pipeline construction,
stepping and final metadata construction; output/pipeline destruction and full
verification follow the timer. The sample control includes traversal and a
small checksum. There is no file I/O or durability operation in either timer.

Independent integer-key order and membership oracles check sample occurrences,
false borrows and public counters. Completed borrowed keys are compared with
original generated keys. Encoded payload, metadata, EF/rank arrays, cut LCPs and
false flags are compared against untimed batch results, then cross-version
checksums and work counters are matched by the runner. All 480 initial rows and
240 repeat rows passed. The batch encoder is a parity oracle, not an independent
proof of every encoding detail.

The candidate also passed strict Release/O3 ASan+UBSan profile, borrowed writer,
sampling, index builder, index pipeline and mapped-blob suites (6/6), and the
independently authored mapped-index test across eight byte/bit/fixed/variable/K/W
policies. That test checks original-key rank/cut/sample/query oracles, complete
`.index` parity, source pins/unlink and protected native value pages.

Reproduction and limits
-----------------------

Measured on the local M2 Max with Apple Clang 21.0.0, macOS 26.6.2 arm64, using
`-std=c++20 -Wall -Wextra -Wpedantic -Werror -O3 -DNDEBUG`. The host CPU lease
serialized heavy work; no CPU affinity was imposed. Compiler text, complete
header hashes, source/runner SHA-256, commands, UTC bounds and alternating trial
order are in the [initial metadata](results/index_lcp_m2max.json) and
[repeat metadata](results/index_lcp_short_repeat_m2max.json). Raw observations:
[initial CSV](results/index_lcp_m2max.csv),
[repeat CSV](results/index_lcp_short_repeat_m2max.csv).

From a checkout retaining those commits, under the local CPU resource gate:

```sh
CXX='ccache clang++' python3 bench/sample_frontier.py \
  --baseline 79ba498b808e99734515621152a7759a181a0d23 \
  --candidate 370687ac12a8fb94085cc6e023949694377d7de5 \
  --build-dir build-index-lcp-reproduce --records 4096 \
  --prefix 0 4096 --rounds 3 --trials 5
```

The repeat uses `--prefix 0` and a separate build directory. Neither run measures
streamed file output, mmap latency, larger cache-exceeding datasets, native x86,
or the unfinished scheduling/resumption protocol. The builder still retains
its existing full current-key contexts and adds six scalar LCP fields.

-Edward Kmett
