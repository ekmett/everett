Blob and pipeline performance
=============================

Small rank, select and string kernels are useful only if they help their callers.
I use this comparison to measure complete in-memory operations with the same
inputs and encoded formats:

- Building a native key/value blob and its empty fractional index.
- Sampling an existing base and constructing three fractional-index links with
  `index_pipeline`, including `finish()` and its rank/select directories.
- Searching a known window in the final pair, including key decoding, rank,
  sampled offsets, false-borrow handling and returned values.

The last operation starts with the group and its boundary key already known.
It does not include a first-catalog binary search or a complete cascade. These
are resident-memory measurements, with no mmap faults, file I/O or durability
operations.

Inputs and checks
-----------------

The [source](blob_pipeline.cc) generates integer identifiers encoded as eight
big-endian bytes after a configurable common prefix. Bit-profile keys append
one meaningful bit, deliberately retaining a partial final byte. Because every
identifier has the same width, the independent catalog oracle can sort integer
IDs without calling the key comparison being measured.

There are four source blobs. Each successive blob has one quarter as many native
records, spread across the same key range. Some keys coincide between streams to
exercise false borrows. The oracle merges native and borrowed integer IDs with
native occurrences first on equality, then takes every policy-selected group
boundary. It checks the pipeline's decoded borrowed keys bit by bit, retained
native allocations and exact target pins. It also checks every timed query's
native value and borrowed routing ordinal before measurement. Half the queries
are selected native hits; the remainder sample the whole identifier range.

The policy matrix covers byte and bit units, variable values, fixed eight-byte
values, fixed thirteen-bit values, and group sizes 7, 15 and 31. Primitive and
codec tests cover the wider correctness matrix independently.

The [runner](blob_pipeline.py) compiles the same benchmark against two captured
header trees. The baseline is `62ead3fab9d0ee5bda1b47780b7905a45aae1182`.
The measured candidate is `d0271623cd6af865ac39ef8e04fe39f899d399a2`.
It records the candidate revision, the hash of every header, compiler flags and
benchmark source hash. The runner captures the benchmark body before compiling
either variant, as well as capturing the headers. Baseline and candidate process
order alternates by trial;
operation order rotates within each process. Fixture construction and one
pipeline build warm the code and data before measurement. Correctness checks and
output hashing remain outside the timed regions. Build timings include their
allocation work; the returned object is destroyed after the timer stops. Both
timed builds have every encoded section consumed afterward, including the empty
borrowed stream and origin metadata of a base blob.

The runner rejects unequal encoding digests and result checksums between
variants. The encoding digest covers native/borrowed byte arrays, rank classes
and checkpoints, sampled-offset arrays, false-borrow flags and relevant sizes.
It is a comparison aid, not a proof of collision freedom or a serialization of
the complete file header. Policy descriptors are constant within each fixture.
`digest_bytes` counts the digest's framed input, not an allocator footprint or
file size.

Native M2 results
-----------------

Apple M2 Max, 96 GiB memory, AppleClang 21, C++20, `-O3 -DNDEBUG`.
The host resource gate serialized these runs with cooperating builds and
benchmarks. The OS selected cores and clocks; unrelated host activity was not
excluded. These inputs are small and resident, with 4,096 native records in the
base, 1,024/256/64 in the three added stages, and 4,096 queries per round.
I ran five alternating baseline/candidate trials with three rounds each.

With 64 common-prefix bytes, the median times are **baseline → candidate**:

| Policy | Base build, µs | Three-link pipeline, µs | Known-window search, ns |
| --- | ---: | ---: | ---: |
| Byte, variable, K=15 | 777.0 → 227.8 | 416.6 → 255.2 | 1243.9 → 942.0 |
| Bit, variable, K=15 | 1271.9 → 369.6 | 1152.6 → 526.3 | 5243.4 → 1749.7 |
| Byte, fixed8, K=7 | 667.3 → 192.7 | 661.3 → 394.4 | 903.2 → 690.2 |
| Bit, fixed13, K=31 | 1006.6 → 295.4 | 703.9 → 394.1 | 6196.4 → 2166.0 |

Base construction improves about 3.4 times across these four policies. Pipeline
construction improves 1.6–2.2 times. Known-window search improves about 1.3 times
for byte profiles and 2.9–3.0 times for bit profiles. These compare each policy
with itself; byte and bit values have different physical lengths.

Removing the shared prefix gives the following baseline/candidate ratios.
A value below 1 means the candidate is slower:

| Policy | Base build | Three-link pipeline | Known-window search |
| --- | ---: | ---: | ---: |
| Byte, variable, K=15 | 1.33× | 1.07× | 0.93× |
| Bit, variable, K=15 | 2.33× | 1.38× | 1.39× |
| Byte, fixed8, K=7 | 1.41× | 1.07× | 0.97× |
| Bit, fixed13, K=31 | 1.93× | 1.19× | 1.33× |

The short byte-key window medians regress by about 3–7%, while the bit profiles
still improve. There is no universal speedup claim. Small-call overhead matters
when only an eight-byte identifier remains to compare and reconstruct.
For variable byte values with K=15, the short-window trial ranges are
845.6–1304.6 ns baseline and 888.3–1520.0 ns candidate. For fixed eight-byte
values with K=7, they are 643.6–670.5 ns and 662.9–701.2 ns respectively.

Both runs passed the independent catalog oracle, and every baseline/candidate
encoding digest and result checksum agreed. The exact benchmark body is
`ecb47bb`; its SHA256 is recorded with both runs. A small combined ASan/UBSan
run passed the same harness separately from the timing evidence.

- [64-byte-prefix trials](results/blob_pipeline_m2max_prefix64.csv) and
  [metadata](results/blob_pipeline_m2max_prefix64.json).
- [Zero-prefix trials](results/blob_pipeline_m2max_prefix0.csv) and
  [metadata](results/blob_pipeline_m2max_prefix0.json).

Reproduction
------------

Run through the host's resource gate. The package runner does not prescribe a
host-specific gate or change processor settings. On macOS, the benchmark asks
for user-initiated QoS; it does not pin a core or fix its clock.

```sh
python3 bench/blob_pipeline.py --records 4096 --prefix 64 \
  --queries 4096 --rounds 3 --trials 5 \
  --candidate d0271623cd6af865ac39ef8e04fe39f899d399a2 --output build-blob-pipeline/results.csv
```

Use `--prefix 0` for short keys or `--prefix 256` for longer common prefixes.
Use separate build directories when retaining several runs, because header
snapshots and per-trial files are replaced within the selected build directory.
The output CSV retains every trial; each trial reports the mean of its three
rounds. The console summary reports medians of those means and baseline/candidate
ratios. The runner's `--candidate working-tree` mode expects the historical
include paths; use it only in a worktree of the measured revision. The explicit
candidate above keeps the command independent of the current Diet headers.

A small sanitizer check exercises the same independent oracle without treating
instrumented timings as performance evidence:

```sh
python3 bench/blob_pipeline.py --sanitize --records 128 --prefix 17 \
  --queries 64 --rounds 1 --trials 1 \
  --candidate d0271623cd6af865ac39ef8e04fe39f899d399a2 \
  --build-dir build-blob-pipeline-check
```
