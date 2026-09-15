# Owning queries with shared navigation views

The shared owning/mapped query path retains shape and bounds checks while
internally built arrays avoid repeated padding and EF-endpoint reads. In the
final comparison, median complete-query times are within 0.8% of the version
before portable navigation views. This recovers the initial owning-path
regression; it does not establish a new speedup over that earlier version.

## What changed

The shared-view/navigation revision `3882d3c` introduced byte-backed directory
words, bounded rank accumulation, shared blob projection and generic query
owners. Owning queries still constructed checked `profile_view` instances for
their native and borrowed arrays at every catalog. Those constructors read the
padding and first/terminal EF offsets each time.

The final candidate `d8bc932` changes `profile_array::view()` to use
`profile_view::from_sections`. Its private sections already come from checked
builders. Each returned view still checks metadata and section shapes; it does
not retain pointers across array copies or moves. The public `profile_view`
constructor and explicit `validate_contents()` retain their content checks.
Query parsing, rank overflow checks and projected-range bounds are unchanged.

The focused ownership tests cover valid default/built-empty arrays, nonempty
moved-from shape rejection, independent copied allocations, moved destinations,
replacement of the original owner and public malformed-EF rejection. Native
and borrowed byte/bit roles are covered.

## Method

Both experiments use the unchanged [shared harness](query_compare.cc) and
[runner](query_compare.py), with the fixture and timer scope described in
[the complete-query comparison](query_compare.md). Both variants use `K=W=15`.
Each fresh process checks every expected source, native ordinal and full value
before and after timing. The runner also checks catalog counts, match counts and
value checksums across variants.

Each experiment has five alternating baseline/candidate process trials, three
record/prefix cases, both byte and bit profiles, and 4,096 queries per case. The
baseline in both experiments is `8214d4be528010aabd9aac76a0bcfb91ec7a5b14`.

- Initial candidate: `3882d3ccf91c5c3f964308b2693369917bae8034`.
- Final candidate: `d8bc932600719e576cbdc9a8cffe4ead2da62dcc`.

The final change is available on main at `eafad7b`, whose measured query and
navigation headers match the final candidate. Reproduction uses that reachable
revision.

Native M2 Max, AppleClang 21, C++20, `-O3 -DNDEBUG`, user-initiated QoS. All
compilation and timing use the exclusive CPU/build-directory resource gate.
There is no core pinning, fixed frequency or cold-storage measurement. Hardware
probes denied by the local sandbox are recorded as unavailable in the metadata.

## Initial regression

The first paired run found a 4.2–8.9% median regression. Times below are
microseconds per complete query.

| Records | Prefix bytes | Profile | Baseline | Shared views with repeated content checks | More query time |
| ---: | ---: | --- | ---: | ---: | ---: |
| 4,096 | 0 | byte | 3.129 | 3.409 | 8.94% |
| 4,096 | 0 | bit | 6.131 | 6.457 | 5.30% |
| 4,096 | 64 | byte | 3.129 | 3.403 | 8.73% |
| 4,096 | 64 | bit | 6.208 | 6.471 | 4.23% |
| 65,536 | 64 | byte | 3.782 | 4.110 | 8.66% |
| 65,536 | 64 | bit | 7.632 | 7.996 | 4.77% |

The [initial CSV](results/shared_query_m2max.csv) and
[metadata](results/shared_query_m2max.json) preserve all 60 rows, compiler
commands, header/source hashes and process order.

## Final owning-array path

The second experiment compares the final candidate directly with the same
pre-view baseline. Its medians are effectively at parity. A negative change
means less query time.

| Records | Prefix bytes | Profile | Baseline µs/query | Final candidate | Change |
| ---: | ---: | --- | ---: | ---: | ---: |
| 4,096 | 0 | byte | 3.069 | 3.059 | -0.32% |
| 4,096 | 0 | bit | 6.056 | 6.012 | -0.74% |
| 4,096 | 64 | byte | 3.067 | 3.067 | -0.02% |
| 4,096 | 64 | bit | 6.070 | 6.035 | -0.58% |
| 65,536 | 64 | byte | 3.740 | 3.736 | -0.12% |
| 65,536 | 64 | bit | 7.593 | 7.596 | +0.04% |

Trial ranges overlap. The zero-prefix candidate includes a high trial, giving
ranges of 3.029–3.612 µs for bytes and 5.965–6.617 µs for bits. These are ranges
of trial averages, not per-query tail latencies. Preparation remains mixed and
noisy because each process times one short root build; no general preparation
improvement is claimed.

The [final CSV](results/shared_query_fixed_m2max.csv) and
[metadata](results/shared_query_fixed_m2max.json) contain every result. Comparisons
above are within each paired run. The runs do not support a precise percentage
attribution between the intermediate and final candidates.

All 120 release profile/trial rows passed the independent full-result and
cross-variant checks. Encoded-array sizes are identical within each experiment.
This accounting covers arrays reachable from one prepared root and excludes the
separate warm root, capacities, fixed objects/view fields and allocation
overhead; it does not establish unchanged total process memory. No encoded
byte-hash equality check is claimed.

The final candidate also passed focused CMake ASan/UBSan profile and query
suites plus the installed-package consumer check. The
[check record](results/shared_query_checks.json) identifies exact source hashes
and test configuration. These checks are independent of release timing.

## Reproduction

Run each command under the host's CPU/build-directory resource gate. The runner
snapshots headers from the supplied commits and compiles one shared harness.

```sh
python3 bench/query_compare.py --baseline 8214d4b --candidate 3882d3c \
  --build-dir build-shared-query-m2max --records 4096 --queries 4096 \
  --prefix 0 64 --larger-records 65536 --larger-prefix 64 --trials 5 --no-w16

python3 bench/query_compare.py --baseline 8214d4b --candidate eafad7b \
  --build-dir build-owning-view-m2max --records 4096 --queries 4096 \
  --prefix 0 64 --larger-records 65536 --larger-prefix 64 --trials 5 --no-w16
```

This benchmark measures owning in-memory queries. Mapped queries, page faults,
disk I/O, recovery scans and native x86 performance are separate measurements.
