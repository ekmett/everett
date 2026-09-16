Complete Byte/Bit Space Results
==============================

The measured byte penalty is small for random keys and larger values. It is
more noticeable when nearby keys differ in their last few bits. At 131,072
rows, decimal-ending string keys with sixteen-byte values cost **10.38% more**
in the current byte path, or **5.42% more** when I explicitly declare their
common encoded value width. Binary string keys cost **0.07% more** by default;
with that same width declaration, the byte files are **2.65% smaller**.

Those are complete `.kv` plus `.index` sizes for the same logical records and
searchable layout. They are not performance results. Integer codec choices
matter enough to reverse the ordering, so I keep the actual typed formats and
identical-grammar controls separate.

Measured Layout and Scope
-------------------------

I built 1,800 complete files across 200 layouts: five modes, ten datasets and
four sizes. Every layout has three disjoint nonempty native files, one shared
empty native, all required fractional-index files and enough empty carriers
to make the root directly searchable. The two larger native populations are
one quarter and one half of the table; a second quarter is a native-only
secondary. All modes use 15:1 sampling and fifteen-record codec blocks.

The serializer includes each file's 96-byte envelope, body directory, CRCs,
alignment, sparse offsets, rank data, cut LCPs, false-borrow bits, and sort
metadata where present. All files were reopened and scanned, all native and
borrowed records were compared, and each prepared cascade was probed. Logical
oracle SHA-256 identities match across all modes for all forty datasets/sizes.
No SQLite metadata, filesystem allocation rounding, retained history or
allocator capacity enters these totals.

These fixtures contain present values and unique live keys. They do not model
tombstone populations, skewed sort populations, long strings or an entire
scheduler history. [The harness guide](README.md) gives the exact construction,
codec choices and reproduction commands. The [complete tables](results/tables.md)
retain every size and the raw-codec controls.

Current Typed Paths
-------------------

At 131,072 rows, the totals below count four unique `.kv` files and six
`.index` files. Positive deltas mean byte files are larger.

| Key / value distribution | Byte native | Bit native | Byte indexes | Bit indexes | Complete byte delta |
| --- | ---: | ---: | ---: | ---: | ---: |
| Ordered u64 / fixed u64 | 1,452,968 | 2,130,608 | 158,520 | 152,808 | −29.43% |
| Random u64 / fixed u64 | 2,215,504 | 2,130,608 | 213,984 | 218,024 | +3.44% |
| Ordered u64 / 0–512-byte values | 34,511,744 | 34,880,176 | 158,520 | 152,808 | −1.04% |
| Random u64 / 0–512-byte values | 35,272,512 | 34,880,168 | 213,984 | 218,024 | +1.11% |
| Decimal-ending strings / 16-byte values | 2,809,048 | 2,534,592 | 171,088 | 165,288 | +10.38% |
| Same keys plus `/state` / 16-byte values | 3,595,864 | 3,511,368 | 227,376 | 230,608 | +2.17% |
| Binary strings / 16-byte values | 4,576,960 | 4,565,136 | 288,976 | 297,552 | +0.07% |
| Decimal-ending strings / 0–512-byte values | 34,360,632 | 34,090,368 | 171,088 | 165,288 | +0.81% |
| Same keys plus `/state` / 0–512-byte values | 35,147,120 | 35,064,712 | 227,376 | 230,608 | +0.22% |
| Binary strings / 0–512-byte values | 36,127,816 | 36,117,520 | 288,976 | 297,552 | +0.0047% |

The typed byte side is KV02 with `sort_list<S>`; the bit side is KV03 with
`bin<tip<S>, sort_undefined>`. They have the same single sort and the same
logical keys and values, but their physical grammar differs. The integer sort
uses `unsigned_key<64>` and either `unsigned_value<64>` or `string_value<>`.
The string sort is the built-in `unsorted<std::optional<std::string>>`.

KV03's integer codec writes all 64 key bits. KV02 front codes their ordered
bytes. That is why the ordered-integer row strongly favors the current byte
path: it is not a benefit inherent in byte alignment. The same raw KV02
front-coding grammar on both sides instead makes byte files **13.11% larger**
for ordered u64 keys and fixed u64 values. For random u64 keys, those raw byte
files are **1.11% smaller**. A fixed-width FC integer codec would be another
choice; this experiment does not implement it.

Fixed Values: Make the Width Explicit
------------------------------------

The fixed integer sorts declare their widths on both sides, so both remove
values from the Elias–Fano universe and omit redundant value lengths. Ordinary
optional-string sorts promise variable size even when this particular batch
has equal-length values. The current KV03 writer observes a common encoded
width; the incremental KV02 writer requires that width before construction.

I therefore added `typed-byte-known`, which passes the supported width of
seventeen bytes to the byte writer for these all-present, sixteen-byte string
values. The presence tag is included. It produces real KV02 files and checks
that every value fits. This is an explicit low-level writer hint, not an automatic option of
`connection`. It is a file-local construction choice, not a promise
that future tombstones also occupy seventeen bytes.

| 131,072-row string fixture | Default byte total | Width-known byte total | Bit total | Width-known byte delta |
| --- | ---: | ---: | ---: | ---: |
| Decimal-ending keys | 2,980,136 | 2,846,112 | 2,699,880 | +5.42% |
| Same keys plus `/state` | 3,823,240 | 3,690,512 | 3,741,976 | −1.38% |
| Binary keys | 4,865,936 | 4,733,728 | 4,862,688 | −2.65% |

The explicit width removes 131,072 bytes of value lengths and another
1,136–2,952 bytes of EF storage in these fixtures. Index files are unchanged.
The remaining difference cannot be blamed on a redundant value-length field.

Where Bit Storage Saves Space
-----------------------------

The decimal-ending keys share a long prefix and often differ near the end of
the final byte. Ordinary bit front coding already uses that information. It
is not only compressing counts: in the 131,072-row typed fixture, native key
literals take **1,398,840 bits** in byte mode and **649,528 bits** in bit mode,
a saving of about **5.72 bits per row**. No extra entropy or order-preserving
compression was applied to the input strings.

The width-known native byte stream uses 2,097,152 framing bits; the bit stream
uses 1,438,440. Conversely, the byte presence tag plus sixteen raw bytes takes
136 bits per value, while KV03's one-bit tag, exponential-Golomb length and
sixteen bytes take 138. Keeping these terms separate explains the result more
clearly than calling every difference a bit-versus-byte cost.

The `/state` suffix changes that balance. Every changed prefix must repeat
those trailing bytes, and the bit suffix/backtrack counts become larger.
Byte LEB128 still fits those counts in one byte. With a known value width,
byte storage wins this fixture despite retaining fewer bits of each key.
The high-entropy fixture similarly spends most of its key space on literals.

Explicit bit-key codecs also exist: `fc_bit_key<>` accepts non-byte-length keys,
and `unsigned_key<N>` can describe a non-byte integer width. Their existence
does not mean the ordinary string examples apply an additional key compression
scheme. An order-preserving dictionary or other compressed key alphabet could
change the tradeoff; none was measured here.

Size Effects
------------

The ordinary typed byte penalty for decimal-ending keys and sixteen-byte
values grows from **7.43% at 1,024 rows** to **10.38% at 131,072 rows**, as fixed
file metadata becomes less prominent. Random u64/fixed-u64 moves from +5.08%
to +3.44%. Binary-string/fixed-value files move from −0.84% to +0.07%.
The variable-value string cases remain within about one percent across the
entire measured range. These are distributions, not a universal bound.

The [per-file CSV](results/files.csv) gives all byte components. In the
width-known decimal-ending case, byte EF arrays total 9,840 bytes; bit EF arrays
total 12,536 bytes. This experiment keeps ordinary EF everywhere and makes no
claim about the speed or complete-file size of a different offset format.

Would NUL Termination Help?
--------------------------

For a NUL-free suffix shorter than 128 bytes, the existing suffix length is
one byte. A one-byte terminator also costs one byte. It changes scanning and
skipping, but gives no space saving for those suffixes. Known fixed-width
integers can infer their missing suffix width from the retained prefix instead;
that is a stronger specialization than replacing a length by a terminator.

For arbitrary binary strings, I modeled `00 ff` for an embedded zero and
`00 00` for the terminator, keeping the existing FC cuts. Replacing only the
suffix-length field adds one byte per suffix plus one per zero in these
short-suffix fixtures: **140,437 bytes** across all native and index streams
for the raw decimal-ending case, or **148,720 bytes** for the raw binary case.
The typed sort discriminator can add a few extra escaped zeros.

That is an exact stream model, not a supported new file. It does not recalculate
EF offsets, byte alignment or checksums and is not counted as a complete-file
result. A terminator also gives up direct skipping from an explicit suffix
length unless another piece of the grammar supplies the boundary.

Direction
---------

Byte alignment looks affordable for many of these tables, especially when the
sort supplies a fixed value width and high-entropy keys dominate. Short final
bit differences are a real strength of bit front coding. I would keep those
facts distinct from current generic wrapping costs: a byte-native sort grammar
could remove controls whose lengths the sort already knows, and a fixed-width
FC integer codec could improve either profile. Those are implementation
opportunities, not performance results from this space experiment.

Provenance and Checks
---------------------

Measured source: `9fbf906`, with production headers from `b2b893c`. Initial
source `09627d4` produced a subset of 1,152 files; the expanded run reproduced
every one of their file lengths and SHA-256 identities. The added decimal-ending
fixture and explicit-width control have separate rows. No original result was
silently replaced.

Release and ASan/UBSan each passed five CTest cases at 1,024 rows, covering all
ten datasets per case. An independent reviewer also parsed 24 retained byte
native files, reconstructed 6,144 records against their logical oracles, checked
the width-17 headers and residual universes, and checked the NUL model against
constructed escaped suffixes, including embedded zeros.

[Metadata and header hashes](results/metadata.json),
[file hashes](results/file-sha256.json), [logical fixtures](results/fixtures.json),
[checks](results/checks.json), [summaries](results/summary.json) and
[comparisons](results/comparisons.json) are retained. `analyze.py` regenerates the
summaries and tables from the raw CSVs and checks exact component totals and
matched logical identities. No timing measurement was collected under the
concurrent integration build.
