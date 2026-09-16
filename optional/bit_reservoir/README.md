Bit reader reservoir experiment
==============================

This benchmark compares retaining unread bits in the sort-owned reader with
the previous stateless implementation, which repeatedly loaded and aligned
adjacent fields. The [M2 Max measurements](results/2026-09-16-m2max/report.md)
show faster frame parsing and larger-value merges, alongside regressions in
small-value merges and general Golomb counts.

The current reader keeps up to 64 MSB-first bits, refills with bounded loads,
and retains cached bits when
skipping short payloads. A longer skip advances the address without reading
the skipped bytes. All wire formats, count values and error positions stay
unchanged.

The baseline is commit `cb2b029a657d2bbcd7135265cb3060b2b11cd961`.
`profile_detail`'s stateless decoders remain unchanged and serve as independent
oracles for values, exception types/messages and consumed positions. The new
reader also exposes `skip_bits`; it has the same bounds behavior as `take_bits`
without constructing a borrowed view.

What this measures
------------------

Each executable builds the same fixtures before timing:

- Consecutive exponential-Golomb counts of orders zero and three, including
  short and wide distributions, and general Golomb counts with modulus seven.
- Actual KV03 native files with either shared-prefix or hash-like string keys,
  optional values of length 0–6 or 0–512 bytes, and one tombstone per 17 records.
- A `sort_record_reader` stream over those same logical records. Its reader
  persists across records; this is not the KV03 file framing.
- Disjoint native merges and merges with half the output keys present in both
  inputs. Duplicate bindings have identical values, and the newer occurrence
  replaces the older occurrence. Tombstones remain encoded; these are ordinary
  replacement merges, not strong-delete rebuilding.

`frames` visits mapped KV03 headers without reconstructing complete keys.
`cursor` reconstructs keys and skips values. `records` decodes complete typed
keys and values. `merge-*` measures the native merge builder, including output
allocation, FC encoding, EF construction and output destruction. It excludes
fractional-index rebuilding, file envelopes, CRC, output mapping and durability.
Merges are compared to independently batch-encoded canonical output bytes before
timing. Complete decoded keys and values are also checked outside the timer.

Fixture creation, file mapping, validation and warmup are excluded. Inputs are
resident and reused; this is not a cold-page or I/O benchmark. Each timed
iteration includes a compiler memory barrier, and every row reports a checked
checksum and the native input bit extent. Merge timings use output records as
their denominator; overlap merges consume 1.5 input records per output record.

The narrow change affects `sort_bit_reader`, including KV03 key/value parsing.
KV03 still creates a fresh reader for each payload and reads the preceding
backspace through the existing stateless decoder. `sort_record_reader` can reuse
the reservoir across a whole stream. Raw KV02 native and borrowed/index profile
headers still use their existing decoders. Those are separate integration points,
not measured improvements in this experiment.

Reproduce
---------

Build the same harness against two header trees:

```sh
mkdir -p build-reservoir/baseline-headers
git archive cb2b029a657d2bbcd7135265cb3060b2b11cd961 include |
  tar -x -C build-reservoir/baseline-headers
cmake -S optional/bit_reservoir -B build-reservoir/baseline \
  -DEVERETT_HEADER_ROOT="$PWD/build-reservoir/baseline-headers/include" \
  -DCMAKE_BUILD_TYPE=Release
cmake -S optional/bit_reservoir -B build-reservoir/candidate \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-reservoir/baseline -j1
cmake --build build-reservoir/candidate -j1
ctest --test-dir build-reservoir/candidate --output-on-failure
build-reservoir/baseline/reservoir build-reservoir/old-inputs 32768 5 4
build-reservoir/candidate/reservoir build-reservoir/new-inputs 32768 5 4
```

Use `-DEVERETT_RESERVOIR_SANITIZE=ON` in a separate build for the differential and
guard-page tests. They cover arbitrary initial bit offsets, 0/64-bit fields,
full 129-bit exponential-Golomb counts, general/truncated Golomb remainders,
malformed fields, continued use after failure, and inaccessible payload skips.
The optional directory is independent of the package's ordinary build.
