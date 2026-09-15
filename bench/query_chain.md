# Complete prepared-chain queries

This fixture measures the complete query API: preparing a small routing head,
creating a query cursor, following its exact target links and retrieving every
matching native entry. It complements the
[known-window comparison](blob_pipeline.md), which begins with a selected window.
These are measurements of the new API, without a baseline speedup claim.

## Method

The [source](query_chain.cc) builds four native blobs containing
4,096/1,024/256/64 records. The three added native stages are indexed with
`index_pipeline`. Their final augmented head has 87 entries. Preparing it under
`K=15` adds one empty-native routing catalog containing six samples; each query
in this fixture consequently visits five catalogs.

Keys contain a fixed-width, big-endian integer after either zero or 64 common
prefix bytes. Bit-profile keys append one meaningful parity bit. Both profiles
use variable values identifying the source level and integer key. The integer
identifiers are retained separately for an oracle using ordinary integer
`lower_bound`, without calling the encoded comparison or routing machinery.
Half the queries are selected from the base's native keys; the rest are drawn
from the entire identifier range.

Before timing, every result's exact source pair, native ordinal and complete
value is checked against that oracle. A warm preparation/query pass precedes
five trials. Each trial prepares a new root, then runs 4,096 queries. Preparation
includes metadata validation, allocation and index finalization, and excludes
destruction of the prepared root. Source blob encoding and construction of the
existing index chain are outside that interval.

Query timing includes query-key copying, cursor construction/destruction,
one-catalog steps, owned result creation/destruction and a small checksum over
every returned value byte. Consuming all value bytes keeps their copies
observable. The checksum is therefore part of the reported time. The timed
match count and checksum must agree with the oracle; exact result validation is
repeated outside the timer for every prepared root.

These are small, resident-memory fixtures. They measure neither page faults nor
storage I/O, and they do not evaluate replacements or categorical arrows.
The source has four catalogs; an arbitrary user-built chain can be much deeper.

## Native M2 results

Apple M2 Max, AppleClang 21, C++20, `-O3 -DNDEBUG`, five trials. The benchmark
requests user-initiated QoS without pinning a core or fixing its frequency.
Builds and timings held the host's CPU/build-directory resource lease; unrelated
operating-system activity was not excluded. Profile and prefix cases run in a
fixed order, so their small differences are not controlled comparisons.

| Profile | Common prefix | Preparation median | Complete query median | Query trial range |
| --- | ---: | ---: | ---: | ---: |
| Byte | 0 bytes | 13.17 µs | 3.794 µs | 3.680–4.290 µs |
| Bit | 0 bytes | 21.92 µs | 7.056 µs | 7.034–7.166 µs |
| Byte | 64 bytes | 14.00 µs | 3.922 µs | 3.869–4.004 µs |
| Bit | 64 bytes | 25.00 µs | 7.387 µs | 7.357–7.527 µs |

Every trial made 20,480 catalog visits and returned 2,796 native matches over
4,096 queries. Both profiles and both prefixes produced the same match checksum.
The [CSV](results/query_chain_m2max.csv) retains all trials, preparation times,
entry counts and checksums. [Metadata](results/query_chain_m2max.json) records
the resolved header revision, hashes, compiler command and fixture parameters.

## Reproduction and checks

The [runner](query_chain.py) snapshots both the harness and all headers before
compilation. It defaults to the query implementation at `2bf591f`; later header
edits do not silently change the measured code. The recorded SHA256 identifies
the exact harness body. Run it through the host's normal exclusive resource gate:

```sh
python3 bench/query_chain.py --records 4096 --queries 4096 --prefix 0 64 --trials 5
```

The CSV and metadata are written under `build-query-chain`. An independent small
ASan/UBSan harness run passed with:

```sh
python3 bench/query_chain.py --build-dir build-query-chain-check \
  --records 128 --queries 128 --prefix 17 --trials 1 --sanitize
```

The component oracle suite separately exercises multiple prefix levels, byte/bit
policies, fixed and variable values, group sizes 3/7/15/31, empty and overlapping
chains, partial contexts, copy/move behavior and ownership. See the
[query contract](../docs/query.md) for the API and its work/trust boundaries.
