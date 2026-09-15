# Complete queries after bit-arithmetic cleanup

I compared `9c2c3be` with `4429df0` using the unchanged
[complete-query harness](query_compare.cc) and [runner](query_compare.py).
The result is broadly flat: these six fixtures show no clear material query
regression from the integrated changes. I do not infer a general speedup from
spelling arithmetic as shifts and masks.

The candidate includes explicit bit/byte shifts and masks, add-before-rounding
with admission bounds, packed rank spacer changes, and shared native frame
output. This is an integration comparison; it does not isolate the cost of any
one check or source-level expression. In particular, the new bounds in
`bit_string::validate` and `profile_detail::resize` are present in the measured
candidate.

## Method

Both revisions compile one byte-identical harness with AppleClang 21,
C++20, `-O3 -DNDEBUG` for native arm64 on the local M2 Max host. The metadata
records the compiler and platform; sandbox restrictions prevented the optional
CPU-brand, memory-size and CPU-count probes. Both builds and all timed processes
held the host's exclusive CPU/build-directory lease.

I used `K=W=15`, variable values, 4,096 queries per trial, and five fresh-process
trials for each case. Baseline and candidate execution order alternates; each
process measures the byte profile before the bit profile. Four native catalogs
contain `N`, `N/4`, `N/16` and `N/64` entries. The prepared root adds one routing
catalog for `N=4096` and two for `N=65536`.

Keys contain an eight-byte big-endian integer after zero or 64 shared prefix
bytes; bit-profile keys add a meaningful parity bit. The independent integer
oracle checks exact source identity, ordinal and complete value before and after
timing. Timed work includes cursor construction/destruction, query and result
ownership, catalog navigation, and a checksum consuming every returned value
byte. It excludes original chain construction. Root preparation has its own
separate timer.

## Query results

Values are microseconds per complete query. The change column compares medians;
negative means less elapsed time. Ranges show the minimum and maximum of five
trials, not confidence intervals.

| Base entries | Prefix bytes | Profile | Baseline median | Candidate median | Change | Baseline range | Candidate range |
| ---: | ---: | --- | ---: | ---: | ---: | --- | --- |
| 4,096 | 0 | byte | 2.706 | 2.654 | −1.9% | 2.692–2.745 | 2.635–2.677 |
| 4,096 | 0 | bit | 4.091 | 4.123 | +0.8% | 4.068–4.154 | 4.070–4.180 |
| 4,096 | 64 | byte | 2.670 | 2.669 | −0.0% | 2.640–2.704 | 2.624–2.688 |
| 4,096 | 64 | bit | 4.184 | 4.173 | −0.3% | 4.149–4.193 | 4.155–4.198 |
| 65,536 | 64 | byte | 3.406 | 3.225 | −5.3% | 3.317–3.486 | 3.161–3.477 |
| 65,536 | 64 | bit | 5.155 | 5.264 | +2.1% | 5.085–5.334 | 5.078–5.546 |

The largest median increase is 2.1% in the larger bit case, whose ranges overlap
broadly. The larger byte median falls by 5.3%, but its ranges also overlap. The
small zero-prefix byte case has separate ranges and a modest 1.9% decrease.
I would keep the cleanup on this evidence without attributing these differences
to individual instructions. Five warm trials do not establish cold-mapping,
x86, disk-I/O or general workload performance.

Preparation is mixed and measures just one short root build per process:

| Base entries | Prefix bytes | Profile | Baseline preparation µs | Candidate preparation µs |
| ---: | ---: | --- | ---: | ---: |
| 4,096 | 0 | byte | 17.50 | 18.83 |
| 4,096 | 0 | bit | 27.50 | 27.04 |
| 4,096 | 64 | byte | 25.71 | 16.46 |
| 4,096 | 64 | bit | 23.83 | 25.83 |
| 65,536 | 64 | byte | 112.92 | 105.04 |
| 65,536 | 64 | bit | 171.33 | 158.67 |

I make no general preparation-speed claim from these short measurements.

## Correctness, storage and reproduction

All 60 profile/trial rows passed their integer oracle and cross-revision checks
for head sizes, routing depths, catalog visits, match counts and checksums.
Each small-case trial returned 2,796 matches over 20,480 catalog visits; the
larger case returned 2,810 matches over 24,576 visits. Every counted payload and
auxiliary-array component is identical between the two revisions. The harness
does not compare the complete encoded bytes.

The [raw CSV](results/query_rounding_m2max.csv) retains every trial and storage
component. The [metadata](results/query_rounding_m2max.json) retains resolved
revisions, all header hashes, unchanged harness/runner hashes, compiler commands,
process order and environment probes. Counted storage excludes capacities,
fixed objects, allocation overhead, retained warm roots and query scratch.

Run the unchanged runner under the host's exclusive CPU/build-directory gate:

```sh
python3 bench/query_compare.py \
  --baseline 9c2c3beeb0f5ce6cf3d81eb5963d41efdc22041e \
  --candidate 4429df0a8507705a9c4ccc0f6366344cf543f396 \
  --no-w16 --records 4096 --queries 4096 --prefix 0 64 \
  --larger-records 65536 --larger-prefix 64 --trials 5 \
  --build-dir build-query-rounding
```

The recorded harness SHA256 is
`5200a36f81c194c854443a3e031db7eb6158062df88f77829fb9e8049269df18`;
the runner SHA256 is
`92c73c1a5eeb01d50919178ff272541283beb5445730d2b50b18af63cde21fd3`.
