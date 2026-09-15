Complete queries with absolute block prefixes
============================================

The final implementation removes the initial byte-query regressions while
retaining the smaller version-2 encoding and avoiding predecessor-block reads
for routed comparison repair.
Against the original implementation, complete-query medians range from **3.04%
faster to 0.91% slower**. Most trial ranges overlap. Preparation is mixed,
including a 20.97% slower small byte-policy median; its trial ranges overlap too.

Final complete-query comparison
-------------------------------

* Original: `18daa213552ec44872f9c042a7977fbe7c81c07d`.
* Final measured candidate: `7fe6ee841a496497057d31a7c1e291c9d1d8f02c`.
* Reachable integration: `030a7a12bbfeaf395d7f1ae9f27eaf4729372b4c`.

Every Everett header in the recursive `query.h` include closure is byte-identical
between the measured and reachable candidates. The [reproduction record](results/query_absolute_reproduction.json)
lists those hashes; unrelated merge, file-writing and catalog code is outside
the harness's include closure.

Median nanoseconds per complete query, five trials; lower is better.

| Base records | Prefix bytes | Policy | Original | Final | Change |
| ---: | ---: | :--- | ---: | ---: | ---: |
| 4,096 | 0 | byte | 1,852.987 | 1,869.883 | +0.91% |
| 4,096 | 0 | bit | 2,819.773 | 2,734.080 | −3.04% |
| 4,096 | 64 | byte | 1,827.026 | 1,814.819 | −0.67% |
| 4,096 | 64 | bit | 2,869.151 | 2,815.633 | −1.87% |
| 65,536 | 64 | byte | 2,248.464 | 2,222.890 | −1.14% |
| 65,536 | 64 | bit | 3,477.315 | 3,438.558 | −1.11% |

Only the small 64-byte-prefix bit case has disjoint query ranges. All 60 final
rows pass the full-result oracle and cross-revision semantic checks. No case
retains the initial regression above 5% with disjoint ranges. This is one direct
comparison; percentages from the intermediate experiments are not added to it.

Median microseconds to prepare the root from an already-built chain:

| Base records | Prefix bytes | Policy | Original | Final | Change |
| ---: | ---: | :--- | ---: | ---: | ---: |
| 4,096 | 0 | byte | 7.458 | 8.458 | +13.41% |
| 4,096 | 0 | bit | 9.792 | 10.000 | +2.12% |
| 4,096 | 64 | byte | 7.750 | 9.375 | +20.97% |
| 4,096 | 64 | bit | 10.917 | 10.958 | +0.38% |
| 65,536 | 64 | byte | 65.875 | 64.584 | −1.96% |
| 65,536 | 64 | bit | 110.292 | 106.750 | −3.21% |

Every preparation range overlaps. These short single-build measurements show
the median differences but do not isolate their cause or establish a general
preparation speedup.

What changed
------------

The first frame of a physical FC block now stores its retained-prefix position
directly. Later frames still backspace from their physical predecessor. This
removes the block's separate predecessor-length checkpoint and lets an encoded
cursor traverse literals and values without reconstructing keys. The profile
encoding version is 2.

At a routed cut, the exact cut LCP repairs the outgoing comparison without
reading the preceding borrowed block. A comparison context can therefore have
an unknown full key length; `full_units()` reports that honestly with an optional.
The optional state fits existing padding: both policy contexts remain **40 bytes**
on this host. Encoded records shrink from **112 to 96 bytes**, and profile views
remain **272 bytes**. The [size probe](results/query_absolute_sizes_m2max.json)
preserves its source, commands and exact results.

I checked the framing change with the existing [query harness](query_compare.cc)
and [runner](query_compare.py), whose bytes are unchanged between the original
baseline and the first version-2 candidate. Queries still compare full results
against the same independent integer-key oracle. Wire bytes differ, so semantic
agreement and backing-array sizes are checked separately.

Navigation work
---------------

The [untimed audit](query_absolute_work.cc) runs the same fixture and checks every
source, ordinal and value. For $K=W=15$, the original and initial version-2 paths
have identical record visits, skipped headers and compared-bit counts. Totals
over 4,096 queries are:

| Base records | Native skipped / visited | Borrowed skipped / visited |
| ---: | ---: | ---: |
| 4,096 | 89,389 / 120,034 | 73,481 / 52,020 |
| 65,536 | 85,071 / 120,158 | 85,704 / 88,257 |

These counts are the same for both policies and the measured prefixes. They
count record headers, not individual count-code decodes or CPU instructions.
The new route carries an unknown length 1,070 times in each small case and 1,305
times in each large case. This fixture performs no extra false-borrow value
probes; independent protected-page tests exercise that recovery path and
preceding-block avoidance. [Raw work rows](results/query_absolute_work_m2max.csv)
and [metadata](results/query_absolute_work_m2max.json) retain the separate bit
comparison counts and the $W=16$ control.
The [final counter check](results/query_absolute_final_work_m2max.csv) repeats
all six $W=15$ cases on the final parser and requires every reported field to
equal the initial version-2 audit; it passes. Its [metadata](results/query_absolute_final_work_m2max.json)
pins the final headers and exact source.

Initial regression and isolated changes
--------------------------------------

Before measuring, I used a practical investigation threshold: a median regression
above 5% with non-overlapping trial minimum/maximum ranges. This is a debugging
trigger, not a statistical confidence interval. The first version-2 candidate
crossed it for both byte-policy cases with a 64-byte prefix:

| Base records | Prefix bytes | Policy | Original ns/query | Initial version 2 | Change |
| ---: | ---: | :--- | ---: | ---: | ---: |
| 4,096 | 0 | byte | 1,917.328 | 2,063.182 | +7.61% |
| 4,096 | 0 | bit | 2,917.236 | 3,059.163 | +4.87% |
| 4,096 | 64 | byte | 1,927.012 | 2,103.902 | +9.18% |
| 4,096 | 64 | bit | 3,057.526 | 3,183.054 | +4.11% |
| 65,536 | 64 | byte | 2,290.883 | 2,516.408 | +9.84% |
| 65,536 | 64 | bit | 3,591.268 | 3,730.591 | +3.88% |

I preserved each subsequent comparison rather than adding its percentage to
another experiment:

* Returning scalar location state instead of carrying a complete encoded frame
  through the pre-lane loop reduced medians by 2.03–3.68% in its paired run.
* Bounding the absolute retained prefix once against the physical frame offset
  permits ordinary key-length addition after payload bounds checks. Its isolated
  timing was mixed, from −3.87% to +4.89%, with overlapping ranges throughout.
* A direct comparison including those two changes removed the larger byte
  regression, but the small 64-byte-prefix case still regressed 5.59% with
  disjoint ranges.
* The next change gives skipped frames a scalar parsing path. It shares the
  count and payload checks with full parsing, but returns only key length and
  next offset. It does not construct suffix/value views or a complete encoded
  record for data that the search will skip. Its paired comparison improves all
  six medians by 1.35–4.72%; the small 64-byte-prefix byte and bit ranges are
  disjoint. The final table above compares the complete result directly with
  the original implementation.

The absolute-prefix bound uses physical offsets after fixed-value stride
restoration. Every inherited unit appeared in an earlier literal, so the retained
prefix cannot exceed that position. Relative frames inherit the same bound from
the previous frame's end. Once suffix and value lengths fit the admitted extent,
the key length also fits it. Tests include impossible absolute prefixes,
maximal count operands and malicious EF offsets into literal data.

Array sizes
-----------

For $K=W=15$, the format change saves approximately 0.4–0.6% of the encoded
arrays in this fixture. Parsing changes preserve these sizes.

| Base records | Prefix bytes | Policy | Original bytes | Version-2 bytes |
| ---: | ---: | :--- | ---: | ---: |
| 4,096 | 0 | byte | 62,355 | 61,961 |
| 4,096 | 0 | bit | 59,310 | 58,943 |
| 4,096 | 64 | byte | 62,867 | 62,473 |
| 4,096 | 64 | bit | 60,118 | 59,847 |
| 65,536 | 64 | byte | 1,101,686 | 1,095,462 |
| 65,536 | 64 | bit | 1,071,470 | 1,067,110 |

These are encoded arrays reachable from **one prepared root**, counting shared
native arrays once. They include native and borrowed FC payload, both EF
directories, rank classes/checkpoints, cut-LCP arrays and false-borrow flags.
They exclude capacities, object headers, allocators, fixture/oracle keys, query
scratch and the separately retained warm root. They are not total process
memory or storage-device traffic.

The initial version-2 run also compared $W=16$ with $K=15$. It saved a small
additional number of directory/payload bytes but had slower query medians in
all six cases. This control uses the initial parser, before the scalar fixes;
it does not establish the final parser's $W=16$ performance.

| Base records | Prefix bytes | Policy | $W=15$ ns/query | $W=16$ ns/query | $W=16$ array bytes |
| ---: | ---: | :--- | ---: | ---: | ---: |
| 4,096 | 0 | byte | 2,063.182 | 2,223.175 | 61,937 |
| 4,096 | 0 | bit | 3,059.163 | 3,242.645 | 58,837 |
| 4,096 | 64 | byte | 2,103.902 | 2,201.101 | 62,449 |
| 4,096 | 64 | bit | 3,183.054 | 3,362.915 | 59,714 |
| 65,536 | 64 | byte | 2,516.408 | 2,837.168 | 1,095,046 |
| 65,536 | 64 | bit | 3,730.591 | 4,310.425 | 1,064,879 |

Method and evidence
-------------------

The host is an Apple M2 Max with Apple Clang 21. Release runs use C++20,
`-O3 -DNDEBUG` and strict warnings. Five fresh-process trials alternate revision
order. Each process warms the exact-result oracle, measures preparation and
4,096 complete queries, then checks all results again. Preparation includes root
validation and any empty-native routing prefix; it excludes construction of the
four native levels and their initial indexes. The fixture has $N$, $N/4$, $N/16$
and $N/64$ native records, fixed prefix bytes and ordered integer suffixes; the
bit policy adds a parity bit.

Every returned value contributes to the timed checksum. Revision comparisons
also require identical match counts, catalog visits and prepared shape. The
harness verifies its QoS request. CPU/build-directory leases exclude other
participating heavy jobs; core assignment and frequency are not fixed. These
are resident owning queries, without cold mmap faults, storage or network I/O.

All raw observations, including scheduling outliers, remain in the linked files.
Metadata records exact revision pins, every header hash, the shared source and
runner hashes, compiler commands and process order:

| Comparison | Raw rows | Metadata |
| :--- | :--- | :--- |
| Original `18daa21` → initial version 2 `e042920`, including $W=16$ | [90 rows](results/query_absolute_initial_m2max.csv) | [Metadata](results/query_absolute_initial_m2max.json) |
| Initial `e042920` → scalar location `17dff1c` | [60 rows](results/query_absolute_locate_m2max.csv) | [Metadata](results/query_absolute_locate_m2max.json) |
| Scalar location → absolute bound `a12eb80` | [60 rows](results/query_absolute_bound_m2max.csv) | [Metadata](results/query_absolute_bound_m2max.json) |
| Original → location plus bound | [60 rows](results/query_absolute_bound_combined_m2max.csv) | [Metadata](results/query_absolute_bound_combined_m2max.json) |
| Bound → scalar skipped frames `7fe6ee8` | [60 rows](results/query_absolute_skip_m2max.csv) | [Metadata](results/query_absolute_skip_m2max.json) |
| Original → final combined candidate | [60 rows](results/query_absolute_final_m2max.csv) | [Metadata](results/query_absolute_final_m2max.json) |
| Original → initial version 2, ASan/UBSan | [12 rows](results/query_absolute_asan_m2max.csv) | [Metadata](results/query_absolute_asan_m2max.json) |

The [artifact manifest](results/query_absolute_artifacts.json) records the sizes
and SHA-256 hashes of the source, rows and metadata files.

Sanitizer timings are not performance evidence. The final focused profile,
comparison and complete-query suites pass **3/3** with `-O3` and ASan/UBSan.
They cover byte/bit keys, fixed and variable values, independent $K/W$
combinations, ordinary and explicit-restart streams, partial comparison
contexts, malformed framing and protected payload pages. The [check record](results/query_absolute_checks_m2max.json)
retains flags, exact source hashes and the complete test output.

To repeat the final comparison from reachable revisions:

```sh
python3 bench/query_compare.py --baseline 18daa21 --candidate 030a7a1 \
  --build-dir build-query-absolute --records 4096 --queries 4096 \
  --prefix 0 64 --larger-records 65536 --larger-prefix 64 --trials 5 --no-w16
```

Run under the host resource lease when configured. The runner removes old
snapshot directories before materializing pinned headers. Sanitizer smoke
tests use `--sanitize --records 64 --queries 128 --larger-records 0 --trials 1`;
their elapsed times are not query-performance measurements.
