Bounded fixed-key search
========================

I measured flat scalar binary search and bounded SIMD pivots over identical
32-, 64- and 128-bit keys. The SIMD path wins some short windows, but a single
size cutoff is a poor default. In particular, scalar bit lifting needs only
four comparisons for 15 keys and avoids range clamps at that complete window.

These are key-array lookup measurements. They do not include fractional-index
routing, value access, page faults or database publication. The `.ff` and `.fv`
formats do not yet have a complete production query reader.

Measured choices
----------------

The table gives the range of scalar/SIMD latency ratios across eight workloads
for each width and count. A ratio above one favors SIMD. These are medians of
three process medians, not confidence intervals.

| Key bits | Entries | M2 Max NEON | i9-12900K AVX2 |
| ---: | ---: | ---: | ---: |
| 32 | 4 | 1.26–2.36 | 1.21–6.25 |
| 64 | 4 | 1.43–2.71 | 1.50–4.99 |
| 128 | 4 | 1.46–3.21 | 1.34–1.87 |
| 32 | 15 | 0.58–0.98 | 0.87–1.47 |
| 64 | 15 | 0.90–1.14 | 0.32–0.86 |
| 128 | 15 | 1.07–1.63 | 0.43–0.87 |
| 128 | 32 | 1.14–2.19 | 0.49–0.98 |

Automatic dispatch uses conservative measured choices:

- NEON: four entries at every width; also two, three, fifteen, sixteen and
  thirty-two entries for 128-bit keys.
- AVX2: four entries for 32-bit keys; two through four for 64-bit keys;
  two or four for 128-bit keys.
- Other targets: scalar binary search. Explicit SIMD remains available.

A separate M2 Max run checks the resulting automatic path. For 128-bit keys,
its scalar/automatic ratios are 1.10–1.76 at fifteen entries, 1.32–1.95 at
sixteen, and 1.12–2.41 at thirty-two. Every selected automatic case has a
favorable median in that run. Dispatch is not free: unselected cases can
differ from explicit binary search because of the wrapper and generated code;
32-bit fifteen-entry ratios span 0.90–1.04. I keep both explicit methods
available. The AVX2 run measures the selected primitives, not a second timing
run of the final dispatch wrapper. Upper bounds have correctness coverage but
these timing runs measure lower bounds only.

Method
------

`fixed_search.cc` tests every count from one through thirty-two, plus a
sixty-three-entry binary fallback control. Keys have either random words or
shared leading words. Each case uses one hot window or 8192 windows, varied
byte alignment, and independent or result-dependent requests. The allocated
window footprint ranges up to about 8.4 MiB; this is resident-memory traffic,
not a cold-storage measurement.

Each collection has three fresh processes, each with three rotated repeats
of 65536 lookups per method: 21,384 timed rows covering 792 workloads.
Independent `std::lower_bound` results validate warmups and every timed
checksum, including the dependent recurrence. Fixture creation and validation
are outside the timed region. No timing rows were discarded.

The M2 Max uses Apple Clang 21 at `-O3`, with local worker builds and timings
paused. The AVX2 run uses Ubuntu Clang 20, pinned to P-core CPU 4 on an
i9-12900K, with `-march=x86-64 -mavx2 -mpopcnt -mno-avx512f`. This does not
establish AVX-512 performance or a universal crossover on other processors.

Protected-page tests separately cover empty spans, short tails, unaligned
starts and valid page crossings. NEON and portable sanitizer runs passed;
SSE2 and AVX2 also ran on x86. AVX-512 has compile coverage only.

Reproduction and evidence
-------------------------

```sh
c++ -std=c++20 -O3 -DNDEBUG -Iinclude bench/fixed_search.cc -o fixed-search
mkdir fixed-results
./fixed-search 3 65536 > fixed-results/process-0.csv
./fixed-search 3 65536 > fixed-results/process-1.csv
./fixed-search 3 65536 > fixed-results/process-2.csv
python3 bench/fixed_search_analyze.py fixed-results
```

The [retained collections](results/fixed_search_20260916/manifest.json) identify
the source and binary hashes for each run. Each collection contains raw CSVs
in `raw.tar.gz`, all per-case medians and process ranges in `cases.csv`, and
per-width/count summaries in `summary.csv`. Extract the raw archive into its
directory and run the analyzer to reproduce its derived tables. The selection
runs used header revision `5263d76` and benchmark revision `cb2b029`; the M2
dispatch run records its changed header hash separately. Current source
reproduction makes a new measurement rather than replacing those observations.
