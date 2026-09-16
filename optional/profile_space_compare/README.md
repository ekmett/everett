Complete Byte/Bit File Space
===========================

I compare complete `.kv` and `.index` files for the same logical records. This
is a space experiment: it contains no performance timings and does not change
production formats. [The retained report](report.md) separates the current
typed paths from controls using the same generic record grammar.

Reproduce
---------

```sh
cmake -S optional/profile_space_compare -B build-space -DCMAKE_BUILD_TYPE=Release
cmake --build build-space -j1
ctest --test-dir build-space --output-on-failure
python3 optional/profile_space_compare/collect.py build-space/profile-space /tmp/profile-space-results
python3 optional/profile_space_compare/analyze.py /tmp/profile-space-results
```

`EVERETT_SPACE_SANITIZE=ON` enables ASan/UBSan for the four small CTest cases.
Each case exercises all eight datasets. The collector runs 1,024, 8,192, 32,768
and 131,072 logical records in four modes, retains exact file sizes and hashes,
and removes the disposable files after validating them. `--keep-files` keeps
those files for inspection. `--sizes` selects other positive multiples of four.

Inputs and Encodings
--------------------

Each size has ordered and permuted 64-bit integers, structured strings and
16-byte high-entropy binary strings. Fixed values are eight bytes for integers
and sixteen bytes for strings. Variable values have deterministic lengths
between zero and 512 bytes. Binary keys include embedded NUL bytes. There are
no tombstones, overwritten rows, compression dictionaries or entropy codecs.

| Mode | Native format | Keys | Values |
| --- | --- | --- | --- |
| `raw-byte` | KV02 | Raw order bytes, byte FC | Raw bytes; fixed width declared where applicable |
| `raw-bit` | KV02 | Identical order bytes, bit FC | Identical raw bytes; fixed width declared where applicable |
| `typed-byte` | KV02 | Current typed byte transport | Current sort codecs and byte padding |
| `typed-bit` | KV03 | Current sort-owned bit grammar | Current sort codecs, no byte padding |

The typed modes have the same single sort: `unsigned_key<64>` with
`unsigned_value<64>` or `string_value<>`, or the built-in
`unsorted<std::optional<std::string>>`. A byte `sort_list<S>` contributes an
eight-bit sort code; `bin<tip<S>, sort_undefined>` contributes one bit. Codes
stay fixed inside each file and front coding or sort seeds amortize them.
These are comparable tables, not wire-compatible files.

The integer KV03 codec writes each integer in full; it is deliberately not
front coded. KV02 front codes the ordered integer bytes. The raw controls show
what changes when both profiles use front coding. Custom variable byte values
retain their own exponential-Golomb length and final byte padding as well as
the generic outer extent. The built-in byte optional-string codec uses the
outer extent directly. An eight-bit presence tag there replaces KV03's one-bit
tag and inner string length. All string values in this experiment are present.

The KV03 writer detects a common encoded value width at construction. The
incremental KV02 writer uses the policy's declared width. Fixed integer sorts
and raw fixed-width controls declare it; the built-in optional-string sort
does not promise a fixed width merely because this fixture has equal lengths.
The report retains actual per-file common widths and does not silently change
these production choices to make the formats agree.

Complete Graph
--------------

Sorted rows are partitioned by ordinal modulo four: one quarter goes into a
native-only secondary, one quarter into a main native, and one half into the
terminal main native. An empty native connects the main and secondary routes;
further empty carriers reduce the root to at most fifteen augmented entries.
All carriers share one empty `.kv`, counted once. Each main node has its own
IX03 file, including the terminal node. The sampling and physical codec block
widths are both fifteen in every mode. All native rows occur exactly once.
This is one complete searchable layout, not an entire retained history or a
prediction of every scheduler state.

The harness writes actual files using the library serializers, checks their
physical byte sizes, reopens them, scans checksums/framing/navigation, compares
all decoded native and borrowed records, and probes the prepared cascade.
Each mode writes an identical length-framed logical oracle; the collector
requires matching SHA-256 identities before comparing space. SQLite catalogs,
filesystem allocation units, directory entries, snapshots of other worlds and
allocator capacities are excluded. Every per-file envelope, section directory,
alignment gap, offset index, rank class/checkpoint, false-borrow bit and cut LCP
is included.

Accounting
----------

`files.csv` contains one row per unique native or fractional-index file. Byte
components sum exactly to the complete physical file length. `data_bits` is
split into literal key bits, encoded value bits and framing bits; byte padding
at the end of the stream is the difference between `data_bytes * 8` and
`data_bits`. Encoded values include their tag/length/padding; the logical oracle
separately records unencoded key and value byte counts. KV03 dictionary paths,
seed IDs and dictionary boundaries belong to `sort_metadata_bytes`. Integer
keys count as literal payload even when their type already determines length.
The two borrowed streams' offset counts and universes are summed in each index
row; neither sum is a fictitious combined EF representation.

The NUL-termination calculation is a separate stream model. It substitutes
`00 ff` for embedded zero bytes and a `00 00` terminator for the current suffix
length, keeping the same FC cuts, values and other controls. The model does not
produce a supported file or rebuild its EF offsets and checksums. It therefore
cannot be reported as a complete-file size. A one-byte delimiter is valid only
for a NUL-free suffix alphabet; it has no general binary-string interpretation.
