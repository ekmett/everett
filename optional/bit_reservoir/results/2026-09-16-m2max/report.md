Bit reservoir on M2 Max
======================

The reservoir improves count decoding and native frame parsing, with mixed
results for complete native merges. I am adopting the bounded reader while
keeping the small-value merge regressions visible: they take about 2.3–6.2%
longer here, and general Golomb-7 counts also take about 5.5% longer.

The [benchmark instructions](../../README.md) reproduce the comparison against
the previous reader without changing the wire format.

Results
-------

Times are nanoseconds per decoded count or output record. Each cell is the
median of three fresh-process medians, each over five measured trials. The
speed ratio is baseline time divided by reservoir time; larger than one is
faster. `overlap` in the last column means the three-process ranges overlap, so
that small difference is particularly uncertain. These descriptive ranges are
not confidence intervals or a test of statistical significance.

| Workload | Baseline ns | Reservoir ns | Ratio | Process ranges |
| --- | ---: | ---: | ---: | --- |
| counts/eg0-small | 10.56 | 8.68 | 1.217× | disjoint |
| counts/eg0-wide | 17.25 | 16.64 | 1.036× | disjoint |
| counts/eg3 | 15.84 | 12.93 | 1.225× | disjoint |
| counts/g7 | 14.72 | 15.53 | 0.948× | disjoint |
| prefix-6/frames | 67.77 | 63.04 | 1.075× | disjoint |
| prefix-6/cursor | 104.47 | 101.51 | 1.029× | overlap |
| prefix-6/records | 137.28 | 131.72 | 1.042× | overlap |
| prefix-6/merge-disjoint | 348.34 | 356.43 | 0.977× | disjoint |
| prefix-6/merge-overlap | 376.50 | 399.74 | 0.942× | disjoint |
| prefix-512/frames | 79.90 | 64.87 | 1.232× | disjoint |
| prefix-512/cursor | 116.30 | 103.41 | 1.125× | disjoint |
| prefix-512/records | 254.29 | 223.27 | 1.139× | disjoint |
| prefix-512/merge-disjoint | 430.73 | 395.86 | 1.088× | disjoint |
| prefix-512/merge-overlap | 468.59 | 419.37 | 1.117× | disjoint |
| hash-6/frames | 71.76 | 65.15 | 1.101× | disjoint |
| hash-6/cursor | 136.98 | 138.36 | 0.990× | overlap |
| hash-6/records | 127.71 | 120.26 | 1.062× | disjoint |
| hash-6/merge-disjoint | 378.12 | 390.67 | 0.968× | disjoint |
| hash-6/merge-overlap | 413.54 | 426.00 | 0.971× | overlap |
| hash-512/frames | 78.15 | 68.40 | 1.143× | disjoint |
| hash-512/cursor | 144.39 | 135.95 | 1.062× | disjoint |
| hash-512/records | 222.73 | 204.26 | 1.090× | disjoint |
| hash-512/merge-disjoint | 473.68 | 436.84 | 1.084× | disjoint |
| hash-512/merge-overlap | 526.84 | 460.99 | 1.143× | disjoint |

The clearest changes are:

- Short order-zero and order-three exponential-Golomb counts improve by about
  22% in throughput. Wide order-zero counts improve by about 4%. Golomb-7 counts
  take about 5.5% longer.
- All four KV03 frame scans improve, with ratios from 1.075× to 1.232×. Cursor
  and complete-record decoding gains are smaller, with some overlapping ranges.
- Merges with 0–512-byte values improve by 1.084–1.143×. Merges with 0–6-byte
  values take about 2.3–6.2% longer. Three of those four regressions have
  disjoint process-median ranges; the hash-key overlap case does not.

This demonstrates why the count loop is insufficient to choose the default.
I have not isolated the native-merge regressions to a particular instruction,
register spill or code-layout effect.

Work and timing boundaries
--------------------------

The host is an Apple M2 Max, using AppleClang 21.0.0 and C++20 `-O3` release
compilation. There are 32,768 output records per native case and 262,144 values
per count case. The two header versions are built from the same harness.

The fixture names distinguish shared-prefix and hash-like keys, followed by
the maximum optional-value length in bytes. Every seventeenth record is a
stored tombstone. Disjoint merges consume one input record per output record;
overlap merges consume 1.5, with identical bindings on the repeated keys.
Ordinary replacement composition keeps the newer binding. These runs do not
perform strong-delete rebuilding.

`frames` parses mapped KV03 frames without full-key reconstruction. `cursor`
reconstructs the key while skipping its value. `records` uses the separate
sort-owned record stream and fully decodes both key and value; it retains one
reader across the stream. KV03 currently creates a fresh reader for each
payload, and its preceding backspace still uses the stateless decoder.

The native merge timer includes input parsing, comparison, output allocation,
FC writing, EF construction, and output destruction. It excludes input fixture
construction/opening/validation, fractional-index rebuilding, output envelopes,
CRC, output mmap and durability. Inputs are warmed and resident. These are
native-reader and native-merge measurements, not end-to-end storage or query
latency measurements. They make no claim about cold-page I/O or other CPUs.

Three alternating baseline/candidate process pairs produced 864 rows: 720
measured observations and 144 excluded warmup observations. All per-workload
counts, bit extents and checksums agree. All twelve final native fixture files
have identical SHA256 digests across all six processes. Each process checks
fully decoded keys/values and independently batch-encoded canonical merge
output bytes outside the timer.

Correctness and reproduction
----------------------------

Baseline: `cb2b029a657d2bbcd7135265cb3060b2b11cd961`.
Measured candidate: `90f624635d5373c166cc79750f0e7405406f20df`.
Independent tests: `eac41d9`.

The candidate header SHA256 is
`67a3d14a460fe303cd9381eb10649b049a84df78bec6cf6d4b83cd649bf5f38f`.
The original baseline header SHA256 is
`2601c7ca318c0faee3cd502d3e80589819261f09a2431684e8a3e36655c25fd1`.
The measured candidate was integrated as `f6ad6ea`, with the same header hash.
The baseline header tree can be extracted from its committed revision for
reproduction; the C++ harness is unchanged from the measured revision.

The strict ASan/UBSan optional CTest run passed both fixture/merge validation and
the independent differential/guard-page suite (51.9 seconds total). Tests compare
values, exception types/messages and consumed positions with the original
stateless decoders, including continued reads after failure. They cover empty
and inaccessible payload views, unaligned starts/tails, fixed widths 0–64,
full 129-bit exponential-Golomb counts, order-63 counts, truncated Golomb
remainders and overflow. The candidate uses bounded loads; skipped payload
pages are not loaded by the skip operation.

Follow the [build instructions](../../README.md) to reproduce the paired run.
[Raw observations](results.csv),
[process spreads](summary.json), [fixture identities](fixtures.json),
[checks](checks.json) and [source/binary metadata](metadata.json) are retained.
The [manifest](manifest.json) hashes the original collected evidence. Its
metadata records the measured revision; subsequent documentation changes are
outside the timing run.
