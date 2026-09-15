Complete queries after the rank and Elias–Fano refactor
=====================================================

The combined changes reduce median complete-query time by **8.37–14.89%** on
the M2 Max across the six measured cases. Preparation is mixed: the short-key
bit case is 29.05% slower at the median. This measures the combined implementation;
the gains cannot be assigned to Elias–Fano alone or added to earlier results.

Revisions and scope
-------------------

* Baseline: `e45e47f14b45a2f9726d23c80d1d19b33de7745a`.
* Candidate: `9c2c3beeb0f5ce6cf3d81eb5963d41efdc22041e`.

The candidate removes cached rank totals, makes rank queries address existing
positions, and uses a generic Elias–Fano sequence whose API selects an entry by
ordinal. The profile owns sampling, the explicit EOF entry and fixed-value
stride restoration. Shape and admission checks establish representable extents
before ordinary offset arithmetic. Exception construction is outlined into cold
helpers; invalid Elias–Fano selection still throws and is marked unlikely.

Other changes between these revisions include a successor-comparison method
used by native merges. The measured query and preparation paths use the shared
query harness and its existing interfaces. This is a comparison of the pinned
header sets, not a controlled experiment isolating each refactor.

The [runner](query_compare.py) and [C++ harness](query_compare.cc) are unchanged.
Both revisions use the same fixture, oracle, compiler options and harness bytes.
The default byte and bit policies both use $K=W=15$; this run does not compare
other group or physical block widths.

Query results
-------------

Median nanoseconds per complete query, five trials. Lower is better.

| Base records | Prefix bytes | Policy | Baseline | Candidate | Change |
| ---: | ---: | :--- | ---: | ---: | ---: |
| 4,096 | 0 | byte | 3,107.28 | 2,679.17 | −13.78% |
| 4,096 | 0 | bit | 4,491.31 | 4,115.50 | −8.37% |
| 4,096 | 64 | byte | 3,147.03 | 2,715.32 | −13.72% |
| 4,096 | 64 | bit | 4,650.37 | 4,230.42 | −9.03% |
| 65,536 | 64 | byte | 3,826.71 | 3,256.90 | −14.89% |
| 65,536 | 64 | bit | 5,721.51 | 5,183.11 | −9.41% |

The complete baseline and candidate trial ranges are disjoint in every query
case. Raw rows retain every trial, match count, value checksum and catalog visit
count rather than just these summaries.

Preparation results
-------------------

Median microseconds to build the prepared root from an already-built exact chain.
This includes root validation and any empty-native routing prefix construction;
it excludes construction of the four native data levels and their initial indexes.

| Base records | Prefix bytes | Policy | Baseline | Candidate | Change |
| ---: | ---: | :--- | ---: | ---: | ---: |
| 4,096 | 0 | byte | 20.000 | 20.500 | +2.50% |
| 4,096 | 0 | bit | 18.791 | 24.250 | +29.05% |
| 4,096 | 64 | byte | 26.167 | 22.042 | −15.76% |
| 4,096 | 64 | bit | 29.708 | 27.833 | −6.31% |
| 65,536 | 64 | byte | 124.000 | 106.417 | −14.18% |
| 65,536 | 64 | bit | 175.500 | 165.917 | −5.46% |

Preparation ranges overlap except in the larger byte case. For short bit keys,
the baseline range is 16.334–28.291 µs and the candidate range is 20.417–28.334 µs.
The median regression is visible, but these short, single-build measurements
do not establish its cause. The refactor changes both root validation work and
construction of empty profile directories, so a separate repeated-preparation
experiment would be needed to attribute it. No broad preparation speedup is
claimed.

Encoded arrays and correctness
------------------------------

Every measured category of encoded-array bytes agrees between revisions: native
payload, borrowed payload, physical offset arrays, rank arrays, cut-LCP arrays
and false-borrow flags. Totals are:

| Base records | Prefix bytes | Byte policy | Bit policy |
| ---: | ---: | ---: | ---: |
| 4,096 | 0 | 62,355 | 59,310 |
| 4,096 | 64 | 62,867 | 60,118 |
| 65,536 | 64 | 1,101,686 | 1,071,470 |

These are array bytes reachable from **one prepared root**, counting each shared
native array once. They exclude object fields, capacities, allocators, fixture
keys and expected values, query scratch, result objects, and the separately
retained warm root. Equal array sizes do not establish byte-for-byte encoding
equality or total process-memory equality.

All **60 release rows** and **12 ASan/UBSan rows** pass. The fixture builds four
native levels with $N$, $N/4$, $N/16$ and $N/64$ entries, then constructs exact
fractional links using `index_pipeline`. Keys consist of a fixed common prefix
and a big-endian integer; the bit policy adds a final parity bit. An independent
integer-key lower-bound oracle checks every emitted source, ordinal and complete
value before and after timing. Half the queries select base-level keys; the
remainder cover the key domain. Timed traversal consumes every returned value
byte into a checksum. Cross-revision checks also require identical result counts,
catalog visits and preparation shape. The sanitizer run uses smaller inputs and
its timings are not performance evidence.

Method and reproduction
-----------------------

The native host is an Apple M2 Max running Apple Clang 21. Release builds use
C++20, `-O3 -DNDEBUG` and strict warnings; sanitizer builds use `-O1 -g` with
ASan/UBSan. Each fresh process completes an oracle warm-up, one preparation and
query measurement, and a final full oracle pass. Five trials alternate baseline
and candidate process order. Each release trial performs 4,096 queries. The
harness checks that the user-initiated QoS request succeeds. The host's CPU and
build-directory leases exclude other participating heavy jobs; frequency and
core assignment are not fixed. This measures resident owning queries, without
mmap page faults, cold storage or network activity.

```sh
python3 bench/query_compare.py --baseline e45e47f --candidate 9c2c3be \
  --build-dir build-query-refactor/release --records 4096 --queries 4096 \
  --larger-records 65536 --prefix 0 64 --trials 5 --no-w16
python3 bench/query_compare.py --baseline e45e47f --candidate 9c2c3be \
  --build-dir build-query-refactor/sanitize --records 128 --queries 128 \
  --larger-records 1024 --prefix 0 64 --trials 1 --no-w16 --sanitize
```

Run each command under the host's resource lease when configured. Existing
snapshot directories are removed before headers are materialized. Both metadata
files record exact revisions, every header hash, compiler commands, process
order and source hashes:

* Harness SHA-256: `5200a36f81c194c854443a3e031db7eb6158062df88f77829fb9e8049269df18`.
* Runner SHA-256: `92c73c1a5eeb01d50919178ff272541283beb5445730d2b50b18af63cde21fd3`.
* [Release rows](results/query_refactor_m2max.csv) and [metadata](results/query_refactor_m2max.json).
* [Sanitizer rows](results/query_refactor_sanitizer.csv) and [metadata](results/query_refactor_sanitizer.json).
* [Artifact and executable hashes, sizes and Mach-O section output](results/query_refactor_artifacts.json).
