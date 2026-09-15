Stored spacers in complete bitmap rank
=====================================

Storing the three run populations at bit positions 0, 11 and 22 removes their
per-query widening. In this bounded M2 Max test, complete bitmap rank improved
by **2.57%** at the independent-query median; dependent-query time changed by
**+0.30%**. The ranges overlap, and this is not evidence of a general latency
improvement.

| Pattern | Baseline median | Candidate median | Change | Baseline range | Candidate range |
| :--- | ---: | ---: | ---: | :--- | :--- |
| Independent | 5.943 ns | 5.790 ns | −2.57% | 5.905–5.986 ns | 5.722–8.279 ns |
| Dependent | 18.419 ns | 18.475 ns | +0.30% | 18.310–18.487 ns | 18.284–18.629 ns |

The first candidate independent trial took 8.279 ns; it remains in the raw data
and range. Five trials are too few to assign a cause to that outlier or to the
small dependent-query difference.

Scope and validation
--------------------

The baseline is `c2703dbf28faa772c7b2b858f9c628d0b26f494f` and the candidate is
`6f7a160d0c43890a5373f2512705676e96805113`. This candidate changes the
stored bitmap run packing and spells bit arithmetic with shifts, masks and
bounded additive rounding. The later Intel prefix implementation is **not**
part of this measurement. Both snapshots use their existing NEON partial-prefix
path on this host.

The [harness](rank_spacers.cc) derives the hot bitmap case, independent/dependent
query generator, warm-up and alternating order from `other_rank.cc`. Each
revision builds its own index from the same deterministic bitmap: their packed
run words cannot be shared. The fixture has 253,577 logical bits, including a
partial final word and block, and 32,704 bytes of encoded bitmap and directory
arrays per index. A 65,536-word query stream occupies another 524,288 bytes.
The source bitmap, independent oracle, both index allocations and benchmark
objects are excluded from the per-index encoded-size field. This is a resident
workload, not a claim that all benchmark data fits in L1 cache.

Before timing, a bit-at-a-time oracle checks every valid rank position and the
derived total for both revisions. It also checks every generated query, including
the complete dependent chain. Each of five trials then makes 1,048,576 calls
to the complete public `rank_view::rank` through an equally non-inlined wrapper.
Independent positions do not depend on earlier results. Dependent positions
mix in the preceding rank result before reducing to the query domain.
Every trial checks cross-revision checksums. The CSV retains all twenty timing
rows and the final verified checksum for each pattern. A separate ASan/UBSan
check passes the every-position oracle and 65,536 generated queries per pattern;
its CSV contains only the header because check mode performs no timing.

Review of the arithmetic found that the stored ten-bit populations fit at
offsets 0, 11 and 22 within the 32-bit word. The multiply uses a 64-bit value;
the selected three-run sum is at most 1,536 and fits its eleven-bit result lane.
Full and partial builders write the same layout, and exact 512-bit boundaries
still return from the directory without reading the bitmap payload. The
separate Elias–Fano packer rounding guard identified during review is outside
this bitmap benchmark.

Reproduction
------------

The native M2 Max run uses Apple Clang 21, C++20, strict warnings and
`-O3 -DNDEBUG`, with a checked user-initiated QoS request. Each variant warms up
for 32,768 queries. Trial order alternates within each pattern under the host's
exclusive CPU/build-directory lease. Frequency and core affinity are not fixed.
No Intel performance claim follows from these M2 measurements.

```sh
python3 bench/rank_spacers.py --build-dir build-rank-spacers/release \
  --baseline c2703db --candidate 6f7a160 --queries 1048576 --trials 5
python3 bench/rank_spacers.py --build-dir build-rank-spacers/sanitize \
  --baseline c2703db --candidate 6f7a160 --queries 65536 --trials 1 --sanitize
```

Run under the host resource lease when configured. The runner snapshots pinned
headers and only renames the baseline namespace. It records both original and
adapted baseline hashes, candidate headers, source, runner, executable, compiler
commands and execution order. The measured harness and runner are committed at
`2d7c71b`.

* [Release trials](results/rank_spacers_m2.csv) and [metadata](results/rank_spacers_m2.json).
* [Sanitizer output](results/rank_spacers_sanitizer.csv) and [metadata](results/rank_spacers_sanitizer.json).
