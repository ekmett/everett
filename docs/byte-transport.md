Byte String Tables
==================

I use the byte profile for ordinary string tables. Byte-aligned parsing and
copying are a useful starting point, and bit packing does not consistently
produce smaller complete files on the measured workloads. The ordinary API is:

```cpp
#include <optional>
#include <string>

import everett.sqlite;

auto storage = everett::multiverse<>::create("byte-data");
auto db = storage.connect("main");
db.put("name", "Everett");
auto before = db.snapshot();
db.put("name", "Byte strings");
// before.get("name") still returns "Everett".
```

The default `storage_policy<>` has one tagless `unsorted<std::optional<std::string>>` sort,
15:1 sampling and byte-counted front coding. It uses the same charged redundant
merge scheduler, persistent worlds, named sessions and transactions as other
typed tables. Its native files use the byte-aligned `KV02` profile.

`multiverse<string_policy>` selects an explicit bit table with the same public
operations. I keep that path available for bit-oriented key grammars and
measured compression gains; I do not assume that smaller controls alone justify
its parsing cost. The time and space results below compare the complete native
merge and file costs that they actually measure.

Record Boundaries
-----------------

The outer record gives us the key suffix length and value length. We use those
boundaries directly for the built-in string sort:

| Component | Encoding |
| --- | --- |
| Logical key | Raw string bytes, after any registry sort code |
| FC counts | Canonical unsigned LEB128, counting bytes |
| Missing value | One `00` byte |
| Present value | One `01` byte followed by the string bytes |

An empty string value is `01`; a tombstone is `00`. Zero bytes and arbitrary
binary strings need neither escaping nor terminators. A key may be empty or a
prefix of another key: its outer FC framing supplies the end, and ordering is
ordinary unsigned-byte lexicographic ordering. These are framed logical keys,
not concatenations of self-delimiting raw strings.

Native suffixes, values and borrowed index keys are byte aligned. The sparse
Elias–Fano offsets count bytes. A block start records an absolute retained-prefix
length; subsequent native records backtrack relative to their predecessor.
The fractional index continues to carry its own comparison context.

The value tag consumes a whole byte so payload copying can use the aligned
path. There is no inner string length or trailing padding. A tag other than
`00` or `01`, a tombstone followed by extra bytes, a missing tag or an unaligned
value is rejected when the typed value is read. The low-level profile can still
store arbitrary byte payloads; it does not assign them string semantics.

Sort and Schema Boundaries
--------------------------

This specialization applies only to the exact built-in
`unsorted<std::optional<std::string>>` sort under a byte policy. Custom sorts
keep their declared codecs, including any framing they require. The bit
profile keeps its sort-owned bit grammar. The specialization also works for
the built-in sort inside a byte `sort_list`; the registry's one-byte sort code
still precedes each logical key.

The default tagless byte schema is
`everett.optional-string/tagless/byte-profile-v2`. A named session checks that
schema when it opens. Applications with an explicit schema identifier must
choose an identifier that describes these byte string codecs as well as their
sort codes, hashing and composition. Opening under a different schema does not
reinterpret the existing records.

Checks cover empty, binary and prefix keys; missing and empty values; canonical
framing; ordinary and mixed-sort queries; chronological replacement merges;
resolved scans; retained snapshots; transaction flushes and branches; saved
worlds; and mapped reopening. The built-in bit codec and a custom byte codec
have separate byte-for-byte regression checks.

Time and Space
--------------

The [matched whole-lookup comparison](../optional/byte_lookup_compare/README.md)
includes the current reservoir decoder on the bit side. Across 54 matched
M2 Max cases, byte lookups deliver 2.04 times the throughput, with every byte
process-median range faster than its bit counterpart. The bit files save about
4.7–5.0% for structured keys and are about 0.66–1.14% larger for hash-like keys
in those ordinary string fixtures. These measured tradeoffs are why I make
byte encoding the default for common tables.

The [matched CPU measurements](../optional/cpu_profile_compare/report.md) compare
the byte and bit string paths on identical logical records. They include native
output allocation, encoding, sparse offsets, a fresh mapping and checksums;
durable publication and fractional-index construction are outside the timer.

On the larger measured fixtures, the byte path runs 2.5–4.3 times as fast during
native merges. Complete byte files cost about 13.6% more space for the structured
keys with sixteen-byte values, and about 1% more for those keys with larger
variable values. The hash-like cases are slightly smaller in byte form. These
are properties of the tested key and value distributions, not a universal space
ordering between the codecs. The report retains exact file sizes and all trials.

The [complete-chain space comparison](../optional/profile_space_compare/report.md)
also counts fractional indexes and compares fixed-width integer sorts. It
covers the same logical records at 1,024, 8,192, 32,768 and 131,072 rows.
For the largest all-present string fixtures with sixteen-byte values, giving
the low-level byte writer their common seventeen-byte encoded width changes
the complete-file tradeoff to 5.42% more space for decimal-ending keys, 1.38%
less for those keys followed by `/state`, and 2.65% less for binary keys.
That hint includes the presence tag; `connection` does not infer it automatically.

Bit front coding can retain part of the last shared byte, so ordinary byte
string keys already benefit from some bit compression. Fixed-width key grammar,
value framing and the key distribution matter independently of alignment.
The comparison keeps these effects separate and models terminated suffixes
without presenting them as an implemented format.

Byte GPU Construction
---------------------

The optional [byte GPU driver](../optional/byte_gpu_merge/README.md) reads the
same mapped KV02 files directly. The GPU finds sampled offsets, parses frames,
merges records and writes a complete byte-profile output with Elias–Fano
navigation. It needs no CPU key reconstruction pass. Metal tests compare the
entire output against the CPU writer, including its checksum and padding.
This is a native-file merge experiment; ordinary connections still use the
CPU, and the driver does not perform durable publication or strong-delete
cleanup.

The [initial M2 Max measurements](../optional/byte_gpu_merge/report.md) cover
4K, 32K and 128K records per input. At 128K, the fixed-value case takes 5.934 ms
on Metal versus 11.908 ms on the CPU, including complete output construction and
checksums. Small merges favor the CPU. Variable-value results and their larger
CPU timing spread remain in the report; these fixtures do not define an
automatic dispatch threshold.
