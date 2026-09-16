Matched byte and bit lookups
===========================

I compare complete replacement lookups for identical logical string tables,
using byte KV02 and sort-owned bit KV03 files with their fractional indexes.
Both use the existing Elias–Fano directories, 15:1 sampling and codec blocks of
15 records. The bit backspace code is exponential-Golomb order zero. Policies
are explicit so a change to the library's default does not change this test.

This collection freezes production headers at
`384f8443a102334539226f4e6e2a6a4a8b63300b`, including the reservoir bit reader.
It is separate from the earlier [offset comparison](../search_compare/report.md),
whose decoder predates that change.

Results on Apple M2 Max
-----------------------

Byte lookups provide **2.04× the throughput** of bit lookups across the 54
matched cases, with individual ratios from **1.78× to 2.23×**. The core
8K/128K subset alone gives 2.06× across 48 cases; the additional 524K case
includes all six query/access combinations. These are geometric means of
ratios of process-median query times. Every byte process-median range is
disjoint from its bit counterpart in the faster direction.

Representative independent hits show the complete-file tradeoff alongside
lookup time. A negative final column means the bit files are smaller:

| Base records | Keys | Byte lookup | Bit lookup | Byte throughput | Bit file size vs byte |
| ---: | --- | ---: | ---: | ---: | ---: |
| 8,192 | structured, 16 bytes | 2.668 µs | 5.737 µs | 2.150× | -4.864% |
| 131,072 | structured, 16 bytes | 3.288 µs | 6.820 µs | 2.074× | -5.030% |
| 8,192 | hash, 128 bytes | 3.083 µs | 6.430 µs | 2.086× | +0.979% |
| 131,072 | hash, 128 bytes | 3.954 µs | 7.918 µs | 2.003× | +1.090% |
| 524,288 | hash, 128 bytes | 4.583 µs | 8.462 µs | 1.847× | +1.135% |

For example, the 131,072-record structured/16-byte fixture occupies 6,762,352
bytes in byte format and 6,422,208 in bit format. The matched hash-like/128-byte
fixture occupies 30,321,360 and 30,651,952 bytes respectively. Bit encoding saves
about 4.7–5.0% for the structured fixtures here; the hash-like bit fixtures are
about 0.66–1.14% larger. Space and speed both depend on the data and grammar.
These results support byte encoding for this ordinary string-table workload;
they do not claim that every key/value codec has the same tradeoff.

The snapshot includes the reservoir bit reader. The earlier pre-reservoir
measurements remain separate; this collection changes query count and timing
controls, so its difference from that earlier collection is not an isolated
measurement of the reservoir's effect.

All 972 observations are retained. The largest within-process trial ratio is
1.422, and the largest ratio between process medians within a case/profile is
1.228. Median CPU/wall time across process/query-group medians is 99.74%; the
lowest individual trial is 91.81%. The largest excursion occurs in a 524K byte
mixed/dependent group. Variation is visible, but the byte advantage remains
across all process ranges. No slow trial was removed or recollected.

The [paired summary](results/2026-09-16-m2max/summary.csv) retains every query
kind and access pattern, absolute times, process ranges and complete-file
bytes. [CPU/wall diagnostics](results/2026-09-16-m2max/diagnostics.csv),
[raw timed rows](results/2026-09-16-m2max/queries.csv.gz),
[space observations](results/2026-09-16-m2max/space.csv.gz) and
[validation provenance](results/2026-09-16-m2max/provenance.json) are retained.
The [artifact manifest](results/2026-09-16-m2max/manifest.json) records their hashes.

Workloads
---------

Keys have 16 or 128 bytes, with either structured shared prefixes or hash-like
contents. Each live value contains 32 bytes, encoded using the ordinary
`optional<string>` grammar. This is a constant logical value length, not a
separate fixed-value codec. The core matrix has 8,192 and 131,072 base records;
a matched 524,288-record hash-like/128-byte case extends the size range.

Each fixture maps four native runs of $N$, $N/4$, $N/16$ and $N/64$ records,
a terminal side run of $N/128$, and enough empty routing heads to bound the
first catalog by 15 entries. Newer runs repeat subsets of the base keys. Every
byte/bit pair has identical keys, values and query lists. Logical and query
fingerprints verify this separately from encoded-stream fingerprints, which
naturally differ between formats.

The benchmark checks exact returned values before timing. It also checks every
warmup and timed checksum against an independent logical oracle. A separate
untimed test introduces unequal newer values and tombstones and exhaustively
checks their keys and neighboring misses.

Three fresh processes per format measure three trials of hits, misses and
mixed queries, each in independent and value-dependent order. Each trial makes
8,192 lookups using a resident list of 4,096 query keys. Adjacent byte and bit
processes alternate their order. Query encoding, cascade traversal, record
parsing, comparison, value materialization and result destruction are inside
the timer; file construction and metadata opening are outside it. Files and
queries are warmed. This is not a cold-storage or SQLite transaction benchmark.

On Apple, each benchmark process requests and verifies `USER_INITIATED` QoS on
its own thread. Wall and thread CPU clocks bracket each complete trial; there
are no per-query clock calls. The runner preserves every observation. The
summary compares medians of process medians, with process ranges to expose
variation. These ranges describe repeatability, not confidence intervals.

Space is the sum of unique complete `.kv` and `.index` file lengths, including
envelopes, section descriptors and alignment. It excludes filesystem block
allocation and temporary fixture/oracle memory. The space rows also retain
individual payload, offset, rank, cut and flag byte counts. This is a paired
string/string workload, not the fixed-key or NUL-terminated codec fixtures in
the separate space study.

Reproduction
------------

Use the source revision recorded with the observations. The existing header
extractor copies the named revision without applying an offset overlay:

```sh
python3 optional/search_compare/prepare.py build-byte/headers \
  --revision 384f8443a102334539226f4e6e2a6a4a8b63300b
cmake -S optional/byte_lookup_compare -B build-byte/release \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO='-O3 -g -DNDEBUG' \
  -DEVERETT_BENCH_HEADER_ROOT="$PWD/build-byte/headers/include"
cmake --build build-byte/release -j2
python3 optional/byte_lookup_compare/run.py build-byte/release \
  build-byte/headers build-byte/results
python3 optional/byte_lookup_compare/summarize.py build-byte/results
```

For correctness, configure another build with
`-DEVERETT_BENCH_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug`, build it and run
`ctest --test-dir build-byte/check --output-on-failure`. The test executable
performs 16,384 mapped changed-value/tombstone/miss checks. Two smaller query
tests also check the byte and bit timing/QoS paths; their times are discarded.
