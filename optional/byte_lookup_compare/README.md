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
