# Complete queries with comparison state

This experiment compares the complete query implementation at `71a92a9` with
ordinary front coding and exact cut LCPs at `2d58df0`. It also measures the latter
with physical block width `W=16`; cascade stride stays `K=15` throughout.

## Method

The [shared harness](query_compare.cc) retains the fixture, timers and integer
oracle from [query_chain.cc](query_chain.cc). Both revisions compile that same
source. The added backing-array accounting runs outside the timers; the optional
`W=16` definition changes only the candidate's policy instantiation.

Four native catalogs contain `N`, `N/4`, `N/16` and `N/64` records. Sequential
index construction links them, then each trial prepares the small query root.
For `N=4096`, preparation adds one routing catalog to an 87-entry head. For
`N=65536`, it adds two routing catalogs to a 1,390-entry head. The corresponding
queries visit five and six catalogs, respectively.

Keys hold an eight-byte big-endian integer after zero or 64 shared prefix bytes.
Bit-profile keys add a meaningful parity bit. Variable values identify the source
level and integer key. Half the queries come from the base catalog's keys; the
rest sample the integer identifier range. Integer `lower_bound` supplies the
expected ordered source, native ordinal and complete value independently of
encoded string comparison, rank and offset navigation.

Every process first prepares a warm root and verifies every query result. It
then times one root preparation and 4,096 complete queries, followed by another
full result verification. Timed matches and value checksums must match the
oracle. The Python runner also requires identical head sizes, routing depths,
catalog visits, match counts and checksums across revisions and block widths.
Encoded byte equality is not required.

Preparation includes validation, allocation and index finalization, but excludes
source encoding and construction of the existing chain. Query time includes
cursor creation/destruction, owned query and result storage, one-catalog steps,
and a checksum consuming every returned value byte. Root destruction is outside
the preparation timer.

Five fresh-process trials alternate variant order:
`baseline, candidate W15, candidate W16`, then the reverse. Each process measures
byte followed by bit. This controls variant order, but does not isolate operating
system activity, fix core frequency or measure cold storage. The harness requests
user-initiated QoS. All builds and runs use the host's exclusive CPU/build-directory
resource gate with sequential compilation.

## Results

Native M2 Max host, AppleClang 21, C++20, `-O3 -DNDEBUG`. Current `W=15` reduced median complete-query time by **38.2–47.5%** in all six cases. Every baseline query-time range was above the corresponding current `W=15` range. These are comparisons within this run, not against timings in older reports.

| Base records | Prefix bytes | Profile | Baseline µs/query | Current W15 | Less query time | Current W16 |
| ---: | ---: | --- | ---: | ---: | ---: | ---: |
| 4,096 | 0 | byte | 5.320 | 3.148 | 40.8% | 3.258 |
| 4,096 | 0 | bit | 10.068 | 6.219 | 38.2% | 6.584 |
| 4,096 | 64 | byte | 5.565 | 3.174 | 43.0% | 3.250 |
| 4,096 | 64 | bit | 10.433 | 6.201 | 40.6% | 6.693 |
| 65,536 | 64 | byte | 7.284 | 3.823 | 47.5% | 4.122 |
| 65,536 | 64 | bit | 13.766 | 7.712 | 44.0% | 8.815 |

`W=16` increased median query time by 2.4–14.3% compared with current `W=15`, while reducing the counted arrays by only 0.07–0.15%. I would retain `W=15` for this scalar implementation. The byte-profile trial ranges overlap in the shortest-prefix small case; five trials do not establish a universal hardware ranking.

### Preparation

Preparation results are mixed and noisier: each trial measures one short root build. In the zero-prefix small case, current `W=15` medians increased by 16.7% for bytes and 31.3% for bits; the other cases range from a 9.7% reduction to a 4.3% increase. The bit-profile zero-prefix candidate trials span 23.00–52.67 µs. There is no general preparation-speedup claim.

| Base records | Prefix bytes | Profile | Baseline preparation µs | Current W15 | Current W16 |
| ---: | ---: | --- | ---: | ---: | ---: |
| 4,096 | 0 | byte | 17.50 | 20.42 | 22.33 |
| 4,096 | 0 | bit | 22.79 | 29.92 | 22.62 |
| 4,096 | 64 | byte | 19.25 | 17.38 | 18.33 |
| 4,096 | 64 | bit | 26.04 | 24.21 | 26.67 |
| 65,536 | 64 | byte | 121.21 | 126.38 | 122.62 |
| 65,536 | 64 | bit | 216.00 | 206.17 | 209.58 |

### Why W16 needs more scalar work

An [independent integer model](results/query_compare_header_work.json) reproduces the shared fixture and the current header-navigation rules; its catalog and match totals agree with the timed CSV. It predicts the following header counts. These are modelled parsing work, not measured hardware accesses or cycles.

| Base records | W15 pre-lane headers/query | W16 pre-lane headers/query | Candidate headers/query, either W | W15 total headers/query | W16 total |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 4,096 | 39.76 | 47.33 | 42.01 | 81.77 | 89.33 |
| 65,536 | 41.69 | 63.19 | 50.88 | 92.58 | 114.08 |

The pure-native terminal and empty-native routing catalogs enter at multiples of `K`. With `W=K=15`, those positions are physical block starts. `W=16` loses that alignment and replays more earlier headers, despite slightly smaller offset arrays. The unchanged candidate counts isolate the additional pre-lane work in this model. It is a concrete explanation to investigate for the observed regression, not a cycle-level attribution. A SIMD control layout might alter the tradeoff; it should be measured before changing the default.

This fixture required no extra false-borrow value access outside a projected native range in the model. Independent correctness tests cover that case. The timed benchmark does not measure the cost of every possible chain shape.

All 90 profile/trial rows passed exact pre/post verification and cross-variant checks. Each small-case trial returned 2,796 native matches over 20,480 catalog visits; the larger case returned 2,810 matches over 24,576 visits. The [CSV](results/query_compare_m2max.csv) includes every query/preparation time, checksum and array-size component. [Metadata](results/query_compare_m2max.json) records hashes, compiler commands and execution order.

## Storage accounting

The harness measures the logical live sizes of native and borrowed payload
vectors, the two physical EF directories, grouped rank arrays, false-borrow flags,
and the candidate's exact cut-LCP array. Each shared native allocation is counted
once across the complete chain reachable from **one prepared root**. The separately retained warm root is excluded. Directory samples use their actual C++
array element sizes on this host.

These totals exclude vector capacity, fixed object/metadata fields, allocator and
shared-pointer control blocks, original key arrays, query scratch and result
objects. They are backing-array measurements, not a serialized file size or a
peak-memory estimate. The current cut array uses eight bytes per virtual cut.

| Base records | Prefix bytes | Profile | Baseline array bytes | Current W15 array bytes | Change |
| ---: | ---: | --- | ---: | ---: | ---: |
| 4,096 | 0 | byte | 61,983 | 62,355 | +0.60% |
| 4,096 | 0 | bit | 59,642 | 59,310 | -0.56% |
| 4,096 | 64 | byte | 62,875 | 62,867 | -0.01% |
| 4,096 | 64 | bit | 60,139 | 60,118 | -0.03% |
| 65,536 | 64 | byte | 1,110,740 | 1,101,686 | -0.82% |
| 65,536 | 64 | bit | 1,081,912 | 1,071,470 | -0.97% |

The exact cut arrays occupy 3,128 bytes in the small case and 49,768 bytes in the larger case. Ordinary native encoding saves enough payload here that the counted totals stay within +0.60% to −0.97% of baseline. The nearly equal small-case totals should not be described as total-memory savings: fixed metadata and object overhead are outside this accounting.

## Reproduction and verification

The [runner](query_compare.py) snapshots all headers at both resolved revisions
and one shared harness body. It records SHA256 hashes for the source, runner and
every header, plus compiler commands, process order and fixture sizes. Existing
`query_chain` source, reports and raw results are unchanged.

Run under the host's CPU/build-directory resource gate:

```sh
python3 bench/query_compare.py --build-dir build-query-compare-m2max \
  --records 4096 --queries 4096 --prefix 0 64 \
  --larger-records 65536 --larger-prefix 64 --trials 5
```

A scoped ASan/UBSan run passed the same three variants; its [CSV](results/query_compare_m2max_check.csv) and [metadata](results/query_compare_m2max_check.json) preserve that check. Instrumented timings are not performance measurements:

```sh
python3 bench/query_compare.py --build-dir build-query-compare-check \
  --records 128 --queries 128 --prefix 17 --larger-records 0 --trials 1 --sanitize
```

Hardware sysctl probes restricted by the local sandbox are recorded as
unavailable rather than inferred. These measurements do not include native x86,
page faults, disk I/O, compaction, persistence, or categorical-arrow evaluation.
