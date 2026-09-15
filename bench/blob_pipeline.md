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
It records the candidate revision, the hash of every header, compiler flags and
benchmark source hash. Baseline and candidate process order alternates by trial;
operation order rotates within each process. Fixture construction and one
pipeline build warm the code and data before measurement. Correctness checks and
output hashing remain outside the timed regions. Build timings include their
allocation work; the returned object is destroyed after the timer stops.

The runner rejects unequal encoding digests and result checksums between
variants. The encoding digest covers native/borrowed byte arrays, rank classes
and checkpoints, sampled-offset arrays, false-borrow flags and relevant sizes.
It is a comparison aid, not a proof of collision freedom or a serialization of
the complete file header. Policy descriptors are constant within each fixture.
`digest_bytes` counts the digest's framed input, not an allocator footprint or
file size.

Reproduction
------------

Run through the host's resource gate. The package runner does not prescribe a
host-specific gate or change processor settings. On macOS, the benchmark asks
for user-initiated QoS; it does not pin a core or fix its clock.

```sh
python3 bench/blob_pipeline.py --records 4096 --prefix 64 \
  --queries 4096 --rounds 3 --trials 5 \
  --candidate HEAD --output build-blob-pipeline/results.csv
```

Use `--prefix 0` for short keys or `--prefix 256` for longer common prefixes.
Use separate build directories when retaining several runs, because header
snapshots and per-trial files are replaced within the selected build directory.
The output CSV retains every trial; the console summary reports medians and
baseline/candidate ratios. `--candidate working-tree` captures the current
header contents and records their hashes, including uncommitted changes.

A small sanitizer check exercises the same independent oracle without treating
instrumented timings as performance evidence:

```sh
python3 bench/blob_pipeline.py --sanitize --records 128 --prefix 17 \
  --queries 64 --rounds 1 --trials 1 \
  --build-dir build-blob-pipeline-check
```
