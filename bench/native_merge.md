Native merge frontiers and framed output
=======================================

The integrated implementation reduces median complete-merge time by
**12.50–80.31%** across the twelve M2 Max fixtures. Zero-added-prefix byte keys
improve by 12.50–17.11%, resolving the earlier short-byte median regression in
this paired run. The four 4 KiB-prefix cases improve by 63.12–80.31%.

These are direct measurements of `e45e47f14b45a2f9726d23c80d1d19b33de7745a`
against `4429df0`. The final candidate carries exact longest-common-prefix (LCP)
lengths between merge heads, handles a successor's first policy unit directly,
and shares internal frame output without retaining a third full key in the
merger. It also includes the generic Elias–Fano/profile refactor, cold exceptions,
explicit unit shifts and additive byte rounding. This combined comparison does
not assign its gains to any single change.

Final direct comparison
-----------------------

Median nanoseconds per distinct output key; negative change means less time.
Each median pools three rounds in each of five alternating process trials.
Those fifteen observations are not fifteen independent processes.

| Policy | Prefix bytes | Baseline | Integrated candidate | Change |
|---|---:|---:|---:|---:|
| byte fixed | 0 | 100.906 | 83.641 | -17.11% |
| byte variable | 0 | 123.777 | 108.299 | -12.50% |
| bit fixed | 0 | 198.364 | 157.323 | -20.69% |
| bit variable | 0 | 239.904 | 200.643 | -16.37% |
| byte fixed | 64 | 106.890 | 85.060 | -20.42% |
| byte variable | 64 | 134.394 | 113.299 | -15.70% |
| bit fixed | 64 | 210.083 | 161.385 | -23.18% |
| bit variable | 64 | 263.059 | 206.662 | -21.44% |
| byte fixed | 4096 | 432.320 | 85.111 | -80.31% |
| byte variable | 4096 | 459.366 | 113.520 | -75.29% |
| bit fixed | 4096 | 529.348 | 166.295 | -68.58% |
| bit variable | 4096 | 567.505 | 209.307 | -63.12% |

The baseline and candidate ranges are disjoint in ten of twelve cases, including
all four long-prefix fixtures. The overlapping cases are byte-fixed/prefix 0
and byte-variable/prefix 64. The [360 raw rows](results/native_merge_final_m2max.csv)
retain every round and its timing; [metadata](results/native_merge_final_m2max.json)
records full revisions, compiler commands and source/header hashes. A
[summary table](results/native_merge_summary.csv) gives minimum, median and maximum
for every fixture in all five comparisons. The original
key/value oracle and output checks passed for every row. These are measured
resident-memory improvements on one host, not universal latency guarantees.

Framed-output comparison
------------------------

The isolated comparison is `9c2c3be` → `819578c`. Its only production changes
factor native frame output into one internal component and route the merger
through it. The merger supplies the exact retained length and borrowed literal;
the output keeps only the previous key's length. The two input cursors still
retain decoded keys. The public native writer retains its own predecessor for
order checking and shares the same rollback-safe framing primitive.

This removes the merger's third full-key allocation and changed-suffix copy.
It does not eliminate output payload copies or the input decoders' key buffers.
The following medians use the same twelve fixtures and trial schedule as above:

| Policy | Prefix bytes | Previous writer | Framed output | Change |
|---|---:|---:|---:|---:|
| byte fixed | 0 | 99.197 | 88.257 | -11.03% |
| byte variable | 0 | 119.914 | 113.742 | -5.15% |
| bit fixed | 0 | 176.524 | 161.494 | -8.51% |
| bit variable | 0 | 213.793 | 209.208 | -2.14% |
| byte fixed | 64 | 95.548 | 88.852 | -7.01% |
| byte variable | 64 | 120.750 | 112.361 | -6.95% |
| bit fixed | 64 | 177.055 | 160.080 | -9.59% |
| bit variable | 64 | 215.324 | 209.231 | -2.83% |
| byte fixed | 4096 | 96.036 | 86.899 | -9.51% |
| byte variable | 4096 | 123.197 | 113.673 | -7.73% |
| bit fixed | 4096 | 179.176 | 163.612 | -8.69% |
| bit variable | 4096 | 217.092 | 206.495 | -4.88% |

All twelve medians improve by 2.14–11.03%. Many minimum-to-maximum ranges overlap;
the two 64-byte-prefix bit-fixed ranges are disjoint. These observations support
the change on this host without establishing a universal speedup. The
[360 raw observations](results/native_merge_framed_m2max.csv) preserve every range,
and [metadata](results/native_merge_framed_m2max.json) pins the isolated comparison.
All original-record and output-wire checks passed.

The complete `819578c` checkpoint also passed strict Release/O3 ASan/UBSan native
writer, allocation-failure rollback, native merge and mapped merge tests, plus
installed and embedded package consumers (six checks). Those functional suites
cover paused/moved builders, strict ordering, fixed and variable values, malformed
later inputs, input pins and chronological composition. They are not timing data.

Frontier comparisons
--------------------

The first run compares `e45e47f` with `13c914d`, which carries each head's exact
LCP with the previous output key and feeds a known retained length to the native
writer. Its 4 KiB-prefix medians improve by 58.39–73.22%; the two zero-added-prefix
byte fixtures regress by 8.82% and 10.45%. The [complete first-run rows](results/native_merge_frontier_m2max.csv)
and [metadata](results/native_merge_frontier_m2max.json) retain that observation.

The next run compares the same baseline with `32923e7`. This additionally handles
a successor's first differing policy unit directly, falling back to suffix
comparison when a nonmaximal encoding repeats common content. Median nanoseconds
per distinct output key are below. Each median pools three rounds in each of five
alternating process trials; the fifteen observations are not fifteen independent
processes. Negative change means less time.

| Policy | Prefix bytes | Baseline | Frontier + first unit | Change |
|---|---:|---:|---:|---:|
| byte fixed | 0 | 102.933 | 111.491 | +8.31% |
| byte variable | 0 | 133.631 | 135.099 | +1.10% |
| bit fixed | 0 | 210.617 | 193.059 | -8.34% |
| bit variable | 0 | 255.831 | 246.536 | -3.63% |
| byte fixed | 64 | 107.933 | 110.486 | +2.37% |
| byte variable | 64 | 133.815 | 137.009 | +2.39% |
| bit fixed | 64 | 214.457 | 194.005 | -9.54% |
| bit variable | 64 | 253.339 | 237.160 | -6.39% |
| byte fixed | 4096 | 447.871 | 112.292 | -74.93% |
| byte variable | 4096 | 467.087 | 135.885 | -70.91% |
| bit fixed | 4096 | 547.455 | 199.074 | -63.64% |
| bit variable | 4096 | 587.308 | 236.158 | -59.79% |

The [raw rows](results/native_merge_unit_m2max.csv) include minimum-to-maximum
spread through every recorded round; [metadata](results/native_merge_unit_m2max.json)
records exact revisions, commands and execution order. Short fixtures have
substantial overlapping ranges. The long-prefix reductions have disjoint
baseline/candidate ranges in all four policies. The two frontier runs are
separate paired experiments; their percentages are not multiplied together.

An additional `32923e7` → `819578c` run is retained as [raw observations](results/native_merge_wide_m2max.csv)
and [metadata](results/native_merge_wide_m2max.json). It crosses generic-EF and
cold-exception integration as well as framing, so it is not the isolated framing
comparison used above.

What the timer includes
-----------------------

Each fixture has 16,384 distinct output keys. A key consists of 0, 64 or 4,096
copies of byte `p` followed by an eight-byte big-endian integer. The bit policy
adds three meaningful bits. Thus the zero-added-prefix case still has eight-byte
keys, and adjacent integer encodings themselves share high zero bytes. Both
policies use $K=W=15$. Fixed values contain eight bytes or thirteen bits; variable
values contain 8–24 policy units.

The two inputs each hold about two-thirds of the keys, with about one-third
shared. Overlapping older values deliberately differ from the expected newer
values, so replacement must choose the correct chronological source. Inputs and
the batch oracle are constructed before timing. One warm merge is fully checked.

The measured interval constructs the merger and its two decoding cursors, runs
`step(128)` until completion, builds the final Elias–Fano directory and destroys
the merger. It excludes input construction, oracle validation and destruction
of the returned output. The input owners, original fixture keys, expected output
and warm output remain alive throughout. This is a resident-memory experiment;
it does not establish cold-mapping, LLC-capacity, I/O or durable-publication cost.
The timer divides by distinct output keys, not by total consumed input records.

Every measured result is checked after the timer. Its payload, metadata and every
Elias–Fano word/sample are compared with batch encoding. Decoding is separately
checked against the original integer-ordered full keys and their expected values.
The batch encoder is shared library code; the original key/value oracle does not
use merge comparisons to decide the result. The runner also requires identical
payload sizes and complete-output fingerprints across variants and trials.
This checks actual content, not only record counts or digest equality within one
binary. The workload measures replacement; noncommutative composition, malformed
sources, pause/move ownership and injected allocation failures have separate tests.

Reproduction and provenance
---------------------------

The host is an Apple M2 Max running macOS 26.6.2 and Apple Clang 21. Builds use
C++20, `-O3 -DNDEBUG -Wall -Wextra -Wpedantic -Werror`. Each process requests
`QOS_CLASS_USER_INITIATED`; this harness does not check that request's return code.
CPU affinity and frequency are not fixed. Every compile and timing run uses the
host's exclusive CPU-heavy and build-directory leases.

The [shared runner](native_merge.py) materializes all headers from exact Git
revisions and alternates baseline/candidate order across five process trials.
The [current harness](native_merge.cc) and the [first-run source snapshot](results/native_merge_frontier_harness.cc)
differ only in untimed EF metadata access needed for the generic codec: the latter
reads the old EF `record_count`, while the former reads the profile's record count
and no longer compares that removed EF member. Their timed merge body is identical.

* First-run harness SHA-256: `def7ec472c20e2fb66530718196190d3011ed466413b943ccedcf72d07f21682`.
* Later harness SHA-256: `d6d1825338c4c305dd269d704756d65a573533ae25cca2e13b28322d16019109`.
* Runner SHA-256: `44081c1fe74421b5c561ca8369ef8bcb6ea7889222bddbbd037c42c0e2049b81`.

Run under the host resource gate when configured:

```sh
python3 bench/native_merge.py --baseline e45e47f --candidate 4429df0 \
  --build-dir build-native-merge-final --records 16384 --prefix 0 64 4096 \
  --rounds 3 --trials 5
python3 bench/native_merge.py --baseline 9c2c3be --candidate 819578c \
  --build-dir build-native-merge-framed --records 16384 --prefix 0 64 4096 \
  --rounds 3 --trials 5
```

The first comparison used the harness and runner at `13c914d`, with candidate
`13c914d`. The metadata files retain every original input-header hash.
The [artifact check](results/native_merge_checks.json)
records executable hashes/sizes, verifies stored source/header hashes and checks
matching payload sizes and output fingerprints across all 1,800 completed rows.
No stage percentages are multiplied to infer a combined result: the headline
comes from the direct final comparison above.

-Edward Kmett
