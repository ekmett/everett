# Small bit-profile count decoding

A bounded 16-bit lookahead reduces median complete-query time by 26.0–26.9%
for the bit-profile cases measured here. Byte-profile medians range from 0.3%
less to 1.7% more time, with overlapping trial ranges. The encoded format and
array sizes are unchanged.

## What changed

Bit-profile counts use exponential-Golomb codes. A count from 0 through 254
fits in at most 15 bits. When at least 16 meaningful bits remain, `read_count`
loads those bits once, counts the leading zeroes, and extracts the complete
small code from that field. A nonzero physical bit offset can make that load
span three bytes; all loaded bytes lie within the supplied view.

Larger counts and shorter tails use the original decoder, starting at the
original offset. This preserves its overflow, truncation and consumed-offset
behavior. Byte counts and Golomb unary backspaces retain their existing paths.
No writer, directory, query comparison or serialization rule changes.

The independent count tests cover 0–256, all eight physical bit offsets,
nonzero logical offsets, tails from 0–16 bits, every truncated code prefix,
and out-of-range initial offsets. Protected-page cases put the final encoded
byte immediately before an inaccessible page. Existing tests cover maximum
64-bit counts, malformed overflow codes and exact error offsets.

## Method

The baseline is `eafad7bbc33f57494664505a4cb5f1a346555a7b`; the measured
candidate is `7586eef2e82a2023162dd4e5ab31ef3832a0d87c`. Both use the unchanged
[complete-query harness](query_compare.cc) and [runner](query_compare.py), with
the fixture and timing scope described in [the query comparison](query_compare.md).
Both variants have `K=W=15`.

The candidate is available on main at `1967b4911ab74e46cf944efa4990db7e75033a1c`;
its measured query/navigation headers and focused profile tests match the
candidate. Reproduction uses that reachable revision.

There are five alternating baseline/candidate process trials for each case,
4,096 queries per process, and both byte and bit profiles. Each fresh process
checks every expected native source, ordinal and full value before and after
timing. The runner also checks catalog visits, match counts and value checksums
across variants. This is a complete owning-query measurement, not an isolated
count-decoder microbenchmark.

Native M2 Max, AppleClang 21, C++20, `-O3 -DNDEBUG`, user-initiated QoS. Builds
and timing hold the exclusive CPU/build-directory resource lease. There is no
core pinning, fixed frequency or cold-storage measurement. Denied hardware
probes are recorded as unavailable in the metadata.

## Results

Times are microseconds per complete query. A negative change means less time.

| Records | Prefix bytes | Profile | Baseline | Candidate | Change |
| ---: | ---: | --- | ---: | ---: | ---: |
| 4,096 | 0 | byte | 3.174 | 3.166 | -0.23% |
| 4,096 | 0 | bit | 6.174 | 4.513 | -26.91% |
| 4,096 | 64 | byte | 3.158 | 3.149 | -0.27% |
| 4,096 | 64 | bit | 6.221 | 4.605 | -25.98% |
| 65,536 | 64 | byte | 3.795 | 3.858 | +1.66% |
| 65,536 | 64 | bit | 7.739 | 5.721 | -26.08% |

The bit-profile trial ranges are disjoint in all three cases: baseline versus
candidate is 6.128–6.231 versus 4.453–4.593 µs, 6.204–6.266 versus
4.580–4.840 µs, and 7.627–7.912 versus 5.596–5.760 µs. Byte-profile ranges
overlap. These are ranges of trial averages, not per-query tail latencies.

Preparation timings remain mixed, particularly for the unchanged byte path;
each process times one short root build. No general preparation improvement
is claimed. The query fixture contains many small count fields, so it does not
establish the same gain for workloads dominated by counts above 254. Such
counts incur the additional bounded lookahead before falling back.

All 60 release rows passed the full-result and cross-variant checks. The
[CSV](results/small_count_m2max.csv) and [metadata](results/small_count_m2max.json)
preserve every timing, process order, compiler command and source/header hash.
Array sizes are identical across variants. This accounting covers encoded
arrays reachable from one prepared root; it excludes the separate warm root,
capacities, objects, fixture keys, results and allocation overhead.

Focused CMake ASan/UBSan profile and query tests plus the installed-package
consumer passed, 3/3. The [check record](results/small_count_checks.json)
preserves the configuration, source hashes and CTest output. Sanitizer checks
are separate from release timing. Native x86, mapped queries, page faults,
disk I/O and recovery scans were not measured here.

## Reproduction

Run under the host's CPU/build-directory resource gate. The runner snapshots
headers from both commits and compiles one shared harness.

```sh
python3 bench/query_compare.py --baseline eafad7b --candidate 1967b49 \
  --build-dir build-small-count-m2max --records 4096 --queries 4096 \
  --prefix 0 64 --larger-records 65536 --larger-prefix 64 --trials 5 --no-w16
```
