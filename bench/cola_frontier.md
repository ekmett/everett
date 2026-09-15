Carried LCPs in COLA construction
=================================

Carrying exact longest-common-prefix (LCP) lengths through both the three-way
target sampler and its parent builder reduces complete COLA build time on these
long-prefix fixtures. The direct before/after measurement has 4,096-byte-prefix
medians 77.0–85.2% lower, with disjoint observed ranges in all four cases.
Short-key ranges overlap; their lower medians do not establish a separate
short-key speedup.

Direct combined comparison
--------------------------

The direct run compares `77507b8` with `6cc772a` (accepted as `ac5d4c4`). Both
versions retain the same aligned comparator, native inputs, output format and checked public APIs. This
is the measured combined result; it is not a product of the separate stage
percentages below.

The metric throughout is **nanoseconds per augmented occurrence of the newly
built node**, including builder construction, stepping and finalization. Each
cell is **median [minimum, maximum]**, from 15 timed observations. These are
complete build measurements, not timers around individual selectors.

| Unit | K | Prefix bytes | Before | Both frontiers | Median change |
| --- | ---: | ---: | ---: | ---: | ---: |
| byte | 3 | 0 | 161.5 [154.6, 177.8] | 161.2 [155.0, 171.2] | -0.2% |
| byte | 15 | 0 | 173.6 [166.6, 183.1] | 171.1 [161.6, 185.8] | -1.4% |
| bit | 3 | 0 | 251.3 [246.0, 270.1] | 241.2 [235.6, 260.2] | -4.0% |
| bit | 15 | 0 | 298.7 [280.2, 320.8] | 261.9 [250.8, 314.8] | -12.3% |
| byte | 3 | 4096 | 984.7 [940.0, 1036.6] | 164.2 [155.2, 174.8] | -83.3% |
| byte | 15 | 4096 | 1154.5 [1112.9, 1194.5] | 170.8 [167.2, 197.3] | -85.2% |
| bit | 3 | 4096 | 1083.8 [1062.4, 1115.3] | 249.0 [236.5, 269.3] | -77.0% |
| bit | 15 | 4096 | 1261.4 [1230.5, 1302.3] | 254.9 [250.4, 284.5] | -79.8% |

Sampler frontier
----------------

This comparison changes only `cola_sample_cursor` in `cola_index.h`:
`77507b8` to `0ba3ee3` (accepted as `d1d74e7`). Each live head carries its exact
bit LCP with the preceding emitted occurrence. Unequal frontiers select the next head without
rescanning their shared prefix; equal frontiers compare only their suffixes.
The selected stream advances with `profile_cursor::advance_comparison`, and
the losing heads retain the ordered-LCP minimum. Native/main/secondary tie
order and sampled occurrence positions stay unchanged.

Measured by itself, this stage lowers long-prefix medians by 47.0–57.1%, with
all four observed range pairs disjoint. All short-key range pairs overlap.

| Unit | K | Prefix bytes | Before | Sampler frontier | Median change |
| --- | ---: | ---: | ---: | ---: | ---: |
| byte | 3 | 0 | 154.9 [150.9, 374.2] | 152.3 [145.3, 325.3] | -1.7% |
| byte | 15 | 0 | 180.8 [166.5, 337.7] | 165.8 [153.9, 293.5] | -8.3% |
| bit | 3 | 0 | 257.4 [245.6, 416.1] | 248.9 [234.8, 384.8] | -3.3% |
| bit | 15 | 0 | 301.6 [279.1, 431.1] | 265.6 [260.3, 350.8] | -11.9% |
| byte | 3 | 4096 | 1000.5 [951.1, 1127.5] | 504.1 [472.6, 640.3] | -49.6% |
| byte | 15 | 4096 | 1162.1 [1119.3, 1201.9] | 498.8 [483.9, 549.4] | -57.1% |
| bit | 3 | 4096 | 1102.2 [1052.3, 1161.7] | 584.0 [557.8, 612.1] | -47.0% |
| bit | 15 | 4096 | 1283.4 [1246.7, 1355.4] | 608.1 [599.8, 698.2] | -52.6% |

Builder frontier
----------------

The second comparison, `0ba3ee3` to `6cc772a`, holds the improved sampler fixed
and carries the same exact-bit frontiers through the parent builder. The sampler
returns the minimum adjacent LCP across the K occurrences it advances. The
builder can then select its next native/main/secondary head and update each
cut LCP without comparing against a separate full previous-key buffer. It
retains the previous key's length for equality detection; strict native ordering
is checked while advancing native cursors.

The four long-prefix medians fall another 56.7–66.6%, with disjoint ranges.
Short-prefix median changes span −5.2% to +6.2%, with every range pair
overlapping. This is an incremental comparison against the sampler-only version;
its percentages must not be multiplied into a claimed overall measurement.

| Unit | K | Prefix bytes | Sampler only | Sampler + builder | Median change |
| --- | ---: | ---: | ---: | ---: | ---: |
| byte | 3 | 0 | 156.3 [146.7, 168.5] | 158.3 [153.4, 208.2] | +1.3% |
| byte | 15 | 0 | 159.1 [154.9, 177.7] | 169.0 [162.0, 198.6] | +6.2% |
| bit | 3 | 0 | 243.8 [234.5, 263.0] | 244.7 [236.0, 272.5] | +0.4% |
| bit | 15 | 0 | 267.2 [260.3, 287.7] | 253.2 [248.2, 270.1] | -5.2% |
| byte | 3 | 4096 | 481.1 [469.2, 516.7] | 163.9 [156.1, 183.1] | -65.9% |
| byte | 15 | 4096 | 503.3 [483.9, 521.3] | 167.9 [162.6, 178.3] | -66.6% |
| bit | 3 | 4096 | 578.1 [565.7, 611.2] | 250.5 [235.6, 265.6] | -56.7% |
| bit | 15 | 4096 | 620.6 [607.0, 634.2] | 264.8 [249.1, 292.6] | -57.3% |

Method and limits
-----------------

The [C++ fixture](cola_layout.cc) is unchanged from the
[comparison-layout experiment](cola_layout.md). The new node has 2,048 native
records and samples a main node with 4,096 native records plus borrowed samples,
and a 4,096-record terminal secondary. The main node's two routes share a native
leaf. Keys have an ordered four-byte suffix after either zero or 4,096 common
bytes. Bit policies append three meaningful bits. Values are variable-width;
K is 3 or 15, W defaults to K, and the bit/K=3 case uses Golomb-3 controls.
This fixture contains overlapping native and borrowed keys.

The M2 Max host runs Apple Clang 21.0.0 with `-O3 -DNDEBUG -std=c++20` under the
exclusive host CPU resource lease. Each variant runs in five fresh processes,
alternating baseline/candidate order; each process warms up every fixture and
then records three timed rounds. Baseline runs first in three of the five
trials. No affinity was requested. Construction of the input fixture,
serialization and result/builder destruction are outside the timer.

Every process writes its eight warm-up IX03 files. The runner compares all
bytes across both variants and all trials. Each timed result is serialized
after timing and checked against its process's warm-up CRC; the CSV also
records exact output size. All sizes, occurrence counts and CRCs agree across
versions. This is wire-equivalence evidence against the existing implementation,
not an independent semantic oracle or a cryptographic equality check for each
timed output. The separate [COLA tests](../tests/cola_index.cc) supply
original-key/query oracles. Both sampler-only and final builder candidates passed the strict
O3 ASan/UBSan core suite; the final candidate also passed the mapped-store
guide example.

Ranges are observations, not confidence intervals. All outliers are retained.
These measurements cover owning, in-memory construction and the tested input
shapes on this compiler/host. They do not measure mapped I/O, sealing, SQLite
publication, a scheduler, or a portable bound on key-comparison work. The
alignment decision from the preceding experiment is held fixed in both variants.

Reproduction and raw evidence
-----------------------------

Run the [runner](cola_frontier.py) under the host's CPU resource gate. It copies
both committed header trees into a new build directory; it does not compile
against changing working-tree headers. The build directory must not exist.

```sh
python3 bench/cola_frontier.py --baseline 77507b8 --candidate ac5d4c4 \
  --trials 5 --rounds 3 --build-dir build-cola-frontier-overall
python3 bench/cola_frontier.py --baseline 77507b8 --candidate d1d74e7 \
  --trials 5 --rounds 3 --build-dir build-cola-sampler-frontier
python3 bench/cola_frontier.py --baseline d1d74e7 --candidate ac5d4c4 \
  --trials 5 --rounds 3 --build-dir build-cola-builder-frontier
```

The exact fixture SHA-256 is
`8757aad627c34e9622a07ec5ef420680a477a698abbbd91121570207f4df0a27`;
the runner SHA-256 is
`39811b608c7ddd801cf6fd63db5154e35a5a47e455aa99de4c834c51344cc206`.
Each run retains all 240 timed observations and its unmodified metadata:

- Direct combined result: [CSV](results/cola_frontier_overall_m2max.csv),
  [metadata](results/cola_frontier_overall_m2max.json).
- Sampler stage: [CSV](results/cola_sampler_frontier_m2max.csv),
  [metadata](results/cola_sampler_frontier_m2max.json).
- Builder stage: [CSV](results/cola_builder_frontier_m2max.csv),
  [metadata](results/cola_builder_frontier_m2max.json).

The metadata records full measured source revisions, every header hash, compile
commands, compiler/platform, binary hashes, execution order, whole-file hashes
and start/completion times. Accepted revision spellings in the reproduction
commands retain every measured header in this fixture's include closure. Their
`multiverse.h` differs from the recorded tree, but this fixture does not include
that facade.
