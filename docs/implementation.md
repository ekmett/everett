# Everett implementation status

Updated 2026-09-15. Specification: [Everett design](design.md).

This ledger records what works, what the tests establish, and what remains to
be built. The C++20 foundations live in `include/everett/`, in
namespace `everett`. Immutable files, mmap queries and saved catalog roots work;
the mutable world runtime and bounded redundant-level scheduler remain
implementation work.

## Ownership and acceptance

One integration owner handles the main checkout, build/package integration and
combined verification. Component work happens in isolated worktrees from
a committed revision, preserving unrelated work and leaving reviewed checkpoints
for integration. These are development responsibilities.

| Role | Owned components | Acceptance |
| --- | --- | --- |
| Navigation and recovery | `rank.h`, `rank15.h`, `elias_fano.h`, `durability.h`; `tests/rank.cc`, `tests/durability.cc` | rank and sparse-offset oracles, counter transitions, publication ordering, failure and resumption cases |
| Grouped navigation and object files | `rank_groups.h`, `mapped_file.h`, `file.h`, `object_path.h`; group/mapping/file tests | policy groups, checked binary envelopes, retained mappings and canonical sharded paths |
| Portable mapped blobs | `word_view.h`, `sections.h`, `mapped_blob.h`; `tests/mapped_blob.cc` | little-endian section encoding, metadata-only typed opening, exact dependency identities and complete mapped query chains |
| Immutable object sealing | `object_writer.h`; `tests/object_writer.cc` | streamed CRC, exclusive creation, no-clobber installation, OS barrier ordering, failure identities and retained outputs |
| Checksums | `crc32c.h`, generated backends, pinned generator and package notices; `tests/crc32c.cc` | independent CRC oracle, bounded loads, reproducible generation, target guards and multi-translation-unit installed consumption |
| Key primitives | `key_detail.h`, `profile.h`; `tests/profile.cc`; [key policies](keys.md) | bounded comparisons, bit movement, count framing and independent bit-level oracles |
| Typed profiles and backing reader | `policy.h`, `profile.h`, `profile_blob.h`, `multiverse.h`; profile/blob/multiverse tests | byte/bit and value-layout matrix, ordinary FC, exact cut LCP, same-policy aliases and unchanged native allocation on reindex |
| Complete encoded-chain queries | `query.h`; `tests/query.cc` | bounded root preparation, exact target traversal, all native matches, partial contexts, cursor budgets and ownership |
| Native construction and merging | `native_writer.h`, `native_merge.h`; native writer/merge tests | streaming record acceptance, preserved FC/EF bytes, chronological composition, input pins and failure state |
| Persistent catalog | `sqlite_catalog.h`; focused, adversarial, VFS and process-interruption tests; optional package consumer | reserved IDs, exact prepared graphs, close/reopen saves, binary operation replay, uncertain commits and conservative pins |
| World semantics and ownership | `fingerprint.h`, `pins.h`, `world.h`; `tests/world.cc`, `tests/pins.cc` | disjoint batch permutations, snapshots, old-value validation, contributions, replay and reference export |
| Design documentation | [design](design.md), [arrows](arrows.md), [rebuilding](rebuild.md), [durability](durability.md), this ledger | consistent contracts, cited derivations, implementation limits and independently usable terminology |

As components change, we update this ledger with the reviewed revision, actual
checks and remaining limits. Host-specific resource coordination stays outside
this package.

## Implemented foundations

### Rank and sparse offsets

`rank_groups<K>` stores one borrowed-entry population per virtual group of
`K = 2^r - 1` occurrences. Classes for 3, 7, 15 and 31 occupy two, three, four
and five bits. A 64-bit prefix checkpoint covers 128 classes. `rank15` supplies
the packed four-bit implementation used by `rank_groups<15>`; its checkpoint
covers 1920 occurrences, independently of the full-bitvector layout below.
A blob projects both endpoints with one rank query and the current class.
Rank accepts only existing positions/groups. Owners and views store no cached
total; `count()` derives it from the final real element, and an empty structure
returns zero. The bitmap and packed owners/views each save eight bytes.
Ordinary mapped opening reads no rank payload to obtain a redundant total.

On little-endian AArch64, rank15 loads a complete checkpoint in four NEON
vectors, masks classes beyond the requested boundary and widens the final
byte reduction. AVX2 uses two loads and AVX-512F/BW one. The x86 paths use
`VPSADBW` before reducing lanes. Each byte pair sums to at most 30; a complete
checkpoint sums to 1920. Short checkpoints use only readable scalar words.
Groups of three use scalar packed sums; seven and thirty-one use bounded NEON
bit-plane reductions on AArch64 and portable word reductions elsewhere.
Other group sizes use the generic class loop, bounded by 127 classes.
Instruction selection follows the compiler target, with no runtime dispatch
or exported ISA flags.

The separate full-bitvector `rank_view` has 64-bit epoch counts every $2^{32}$
bits, 32-bit counts every 2048 bits and three ten-bit populations for the first
three 512-bit runs. Populations are individual, with zero spacers at bits 10 and 21. The
three counts fit in 32 bits at shifts 0, 11 and 22; a masked multiply sums them
without widening at query time. The selected run uses bounded NEON popcount
on AArch64 or portable word operations. A query at a 512-bit boundary uses the
directory without reading the bitmap. Construction handles complete 2048-bit
blocks with four 512-bit popcounts and a separate bounded tail. This backend
supplies rank alone and stores no select support. Intel targets have bounded
AVX2 nibble-lookup/SAD, AVX-512BW lookup/SAD and AVX-512VPOPCNTDQ paths, chosen
by compiler features. Short allocations retain the bounded scalar tail.

`elias_fano` encodes a monotone sequence independently of how its caller sampled
that sequence. `elias_fano_view::select(i)` returns the value at an existing
ordinal. The empty structure has no values; a profile explicitly encodes its
final stream extent as an additional value. The profile layer chooses physical
width `W`, independently of virtual stride `K`, and restores fixed-width payload
strides in its own byte/bit units. Low-level select arithmetic uses plain unsigned
addition and multiplication; metadata admission establishes representable extents.
Dense select scans at most 4096 high bits; sparse groups store exception positions.
Within a selected word, broadword byte-prefix arithmetic locates the bit; BMI2
targets use `PDEP`.

The shared Elias–Fano writer packs low fields in width-specialized tiles of
`64/gcd(width,64)` values and assigns each high word once. AArch64 uses NEON
narrowing for complete width-eight and width-sixteen tiles, and bounded
adjacent comparisons to validate monotone input. Other widths use constant
shifts. Stored low/high arrays, select samples and sparse exceptions agree
with independent scalar construction.

Views borrow native-endian spans or explicitly little-endian byte spans and
validate their shapes. Unaligned mapped arrays use bounded byte loads without
creating `uint64_t` objects in mapped storage. Shape checking does not validate
every directory element; the explicit mapped semantic scanner does that work. The [sampling analysis](sampling.md) distinguishes local window
correctness from level capacity and whole-chain storage bounds.

#### Tests and measurements

Tests cover every class boundary, maximum populations, isolated nybbles,
random packed words, short tails, fixed-stride overflow and Elias–Fano widths.
Protected pages exercise bounded loads and directory-only bitmap queries.
Counter-transition fixtures cross $2^{32}$ and $2^{33}$ without allocating
those enormous bitmaps. Forced-portable paths are tested separately from ISA
paths; native Windows and Linux evidence is confined to the linked reports.

I keep the source snapshots, independent oracles, raw trials and limitations in
the benchmark reports rather than treating primitive timings as storage-I/O
predictions:

| Report | What it measures |
| --- | --- |
| [Packed rank / full bitmap](../bench/rank_compare.md) | Complete rank and window endpoints, resident sizes and dependent queries on M2 Max |
| [Linux rank](../bench/rank_compare_quartus.md) | AVX2 scalar, SAD and qword-first reductions on Core i9-12900K |
| [Windows rank](../bench/rank_compare_windows.md) | AVX2/AVX512 on Ryzen 9 7950X3D, including scheduling outliers |
| [NEON and Cult rank](../bench/neon_cult_rank.md) | Packed rank against the actual external Cult CPU directory |
| [Other grouped / bitmap rank](../bench/other_rank.md) | Groups 3, 7 and 31 and complete bitmap queries |
| [Rank without cached totals](../bench/rank_bounds.md) | Existing-position queries, whole-query timing and view size |
| [Stored spacer lanes](../bench/rank_spacers.md) | Complete bitmap queries with the gaps written during construction |
| [Intel prefix paths](../bench/intel_prefix.md) | Bounded SIMD prefixes and complete bitmap rank on Broadwell/Ice Lake, including same-ISA portable controls and dependent queries |
| [Combined query refactor](../bench/query_refactor.md) | Complete byte/bit queries and root preparation after rank, Elias–Fano and exception outlining changes |
| [Codec arithmetic integration](../bench/query_rounding.md) | Complete queries after explicit unit shifts and additive rounding |
| [Elias–Fano construction / select](../bench/select_compare.md) | Tiled writing, narrowing, validation and scalar/SIMD select candidates |

The selected implementations retain the measured latency tradeoffs: a
qword-first NEON reduction improves independent single-rank throughput but
loses paired and dependent queries; the four-word SIMD select scan loses to
the scalar scan. Width-specialized Elias–Fano construction is faster in the
recorded resident-memory cases, at the cost of about 49 KiB of additional code
in a minimal consumer and higher compile time. These choices are measurable
implementation decisions, not cross-platform performance guarantees.

### Typed byte and bit profiles

Key comparison and longest-common-prefix discovery share one scan. The byte
scanner uses bounded 16-byte NEON or SSE2 loads, followed by word and byte tails;
unrelated bit keys retain a short first-bit mismatch path. Bit copying handles
aligned bodies with `memmove` and shifted bodies in 64-bit chunks, preserving
masked edges, overlapping input and aliased appends. Fixed-width fields and
Golomb framing also use bounded word operations. These are representation-neutral
changes: bit order, counts, padding and encoded bytes remain the same.

The [key/bit comparison](../bench/key_bits.md) separates byte-prefix SIMD from
word-at-a-time bit operations and measures both primitives and complete profile
encoding/decoding. Its oracles cover every bit alignment, protected-page tails,
overlaps, and exact framing bytes, including every truncation of the maximum
129-bit exponential-Golomb code. Native M2 timings and x86-64/Rosetta correctness
checks are reported separately.

The key/bit implementation was reviewed and integrated at `cbd6201`, with the
short mismatch/copy paths at `d027162`. The
[combined blob and pipeline comparison](../bench/blob_pipeline.md) exercises
all three component changes together. With 64-byte shared prefixes on the M2 Max,
base builds improve about 3.4 times, three-link construction 1.6–2.2 times, and
known-window queries 1.3 times for byte profiles and 2.9–3.0 times for bit
profiles. Short byte-key queries have 3–7% slower medians in the same fixture.
Both runs retain all trials and match encoded-output and result checksums;
the integer catalog oracle and a separate combined ASan/UBSan run also pass.

`storage_policy<Unit, Values, GroupSize, BackspaceCode, CodecBlockSize>` carries
the unit, fixed/variable value layout, sampling stride, backspace code and
independent physical block width through
the codec types. `fixed_values<N>` counts
policy units, including the valid width zero. `profile_array<P, Role>` uses
canonical byte varints and policy-selected Golomb or exponential-Golomb bit
backspaces. Other bit counts use order-zero exponential-Golomb. Streams retain
meaningful bit extents and canonical padding. One predecessor-length
checkpoint per physical block supports header-only entry at a selected lane.
The terminal key length handles an end-of-stream predecessor. Common fixed
value width is subtracted from the Elias–Fano residual positions.

`profile_blob<P>` uses ordinary front coding for native and borrowed streams.
It retains two physical offset directories, grouped origin rank, false-borrow
flags and one exact bit-LCP scalar per virtual cut. The LCP relates the cut
boundary to its preceding borrowed key. Reindexing shares the exact native
allocation and recomputes all dependent index metadata. The batch builder
materializes native keys as scratch during reindex.

`profile_query_context<P>` owns its immutable query and carries exact agreement
in bits, key length in policy units and comparison direction. The profile reader
parses controls before the selected lane without reconstructing keys. It then
compares literal suffixes and propagates inherited mismatches. Exact cut LCPs
repair the outgoing borrowed predecessor even when it precedes the window.
Search returns both native values and downstream routing on equality; it does
not resolve same-key arrows. Optional counters expose skipped/visited record
headers and compared literal bits.

`profile_cursor<P, Role>` supplies sequential decoding with borrowed values;
`profile_borrowed_writer<P>` incrementally encodes borrowed keys. Their output
matches the batch encoder across the policy matrix. `sample_cursor<P>` pins
and samples an exact encoded pair without materializing its catalog.
`index_builder<P>` retains bounded sample/decoder state, records each cut's
exact LCP and preserves native allocations. `index_pipeline<P>` feeds samples
directly between stages, supports bounded cursor-event stepping and returns a
chain with exact target pins. Inter-stage samples carry a policy-unit backspace
and suffix; the first sample is literal and later samples refer to that
producer's preceding sample.
Final metadata construction is a separate linear phase. See the
[construction implementation](sampling.md#streaming-construction-pipeline).

Codec fixtures cover 24 combinations: byte/bit × variable/fixed3/fixed0 values
× groups 3/7/15/31. Blob fixtures cover 16 byte/bit × fixed/variable × group
combinations, including repeated equal borrows, partial contexts, cascades and
changed index boundaries. Selected Golomb moduli and exponential-Golomb orders
also exercise profile count encoding, borrowed writers and index pipelines. Count-code
fixtures check known bit patterns, truncation, overflow, unaligned appends,
metadata mismatch and unchanged default encodings. Invalid policy parameters
are rejected at compile time. These test record/prefix behavior, not the full
string-store I/O theorem. Sort-qualified framing, a prefix-free
registry and per-sort hash/category dispatch remain extensions.

### Object envelopes, mappings and type family

`mapped_file` opens regular files read-only and returns lifetime-owning bounded
slices. POSIX mapping and unlink-while-pinned behavior are tested. A native
Windows branch exists but has not been validated on Windows in this checkpoint.

`file<P>` checks a 96-byte little-endian envelope: kind magic, version,
policy metadata, exact lengths and header CRC32C. Only `.kv` and `.index` kinds
exist. By default, opening and `from_slice` read the header and check the exact extent;
they do not read the body or inspect its final padding. The explicit
`file_open_mode::trusted` option additionally skips all header reads and
filename/header kind comparison. It checks only that the mapping can contain
the 96-byte envelope; `body()` slices the remaining physical bytes without
reading them. `header()` returns metadata by value and validates it on explicit
access for trusted handles, without a mutable lazy cache.
`file<P>::scan()` checks the header, whole-body CRC32C and canonical bit padding for recovery
or scrubbing, and `validate_file` retains whole-object validation. Recovery can
select uncertain objects for scanning without scanning every file on restart.
Opening alone does not establish payload integrity; lazy block-level integrity
checking is not implemented. The generic envelope accepts arbitrary bodies;
`sections.h` supplies the portable native/index body format described below.
`encode_file` serializes a complete object; `encode_file_header` emits only its
96-byte envelope from a caller-supplied body CRC. `crc32c(bytes, previous_crc)`
continues a checksum over borrowed chunks without copying them.

`object_writer<P>` streams borrowed chunks through a private file, synchronizes
contents, installs its final name without replacement, then synchronizes the
shard hierarchy and private-name removal. Linux uses `fsync`; macOS uses
`F_FULLFSYNC` file barriers with directory `fsync`. The
[object sealing contract](object-writer.md) covers reserved identities, same-device
root assumptions and receipt scope. It performs no normal-path readback scan.
Injected errors preserve surviving names and stop further writes or publication;
short/interrupted writes are completed, while failed syncs and closes are never
retried. This physical primitive does not implement catalog adoption or recovery.

POSIX tests protect every payload page while exercising checked opening, and
the entire mapping while exercising trusted construction and body slicing.
Other fixtures verify deferred rejection of corrupt headers, wrong policies,
invalid extents and padding, plus retained mappings after unlinking.

CRC32C uses one pinned [Corsix generator](https://github.com/corsix/fast-crc32),
with portable, ARM and x86 implementations checked in alongside the headers.
The [parallel-folding explanation](https://www.corsix.org/content/fast-crc32c-4k)
describes how independent CRC and carry-less-multiply work can share execution
resources. I use generated kernels for this arithmetic. The C++ adaptation gives
them inline linkage, bounded `memcpy` scalar loads, little-endian normalization
and scoped helper macros. Length-based loops avoid forming pointers before the
input. Header and body checks retain the same CRC32C result and file format.

Selection follows the compiler target, with a portable fallback when no
accelerated target is enabled. On ARM, inputs below 128 bytes use scalar CRC;
PMULL handles larger inputs when available, and SHA3/EOR3 fusion takes over at
64 KiB. Those thresholds were measured on an M2 Max. On x86, the eligible
SSE4.2, PCLMUL and AVX512 variants follow the target's feature macros. I do not
add runtime feature detection or export ISA flags to consumers. Source and
installed builds remain header-only and offline. The
[upstream record](../third_party/fast-crc32/UPSTREAM.md) describes regeneration
and the separate MIT-or-zlib license terms.

An independent ASan/UBSan check compared 88,726 buffers against a bit-at-a-time
oracle, exercising the public wrapper and all four host backends, arbitrary
continuation seeds, unaligned spans, and every length through 16 KiB ending at
a protected page. Five x86 target configurations cross-compiled with strict
warnings; x86 execution has not been tested here. The component suite retains
known vectors, independent generated-backend comparisons, loop boundaries,
incremental concatenation and protected-tail cases.

On an Apple M2 Max with AppleClang 21, `-O3 -DNDEBUG`, seven-trial warm medians
were:

| Input | Previous bitwise implementation | Generated wrapper |
| --- | ---: | ---: |
| 96 bytes | 724 ns | 7.93 ns |
| 4 KiB | 32.2 µs | 79.9 ns |
| 1 MiB | 8.30 ms | 17.0 µs |

For comparison, generated serial ARM CRC took 685 ns at 4 KiB; parallel folding
accounts for the improvement beyond simply using the hardware CRC instruction.
The final wrapper processed 4 KiB, 1 MiB and 8 MiB inputs at about 45, 52 and
55 GB/s respectively while rotating through a 128 MiB allocation. Each trial
made complete sweeps of that allocation. These measure resident-memory checksum
work, excluding allocation, page faults and storage reads. `bench/crc32c.cc`
reproduces the comparison; its optional argument selects a named backend such
as `public` or `bitwise`. The observations do not establish tuning for other
processors.

Canonical sharded paths split the current experimental 128-bit opaque object
ID into `ab/cd/<remaining-id>.<extension>`. Cryptographic content-ID calculation
and verification are still pending; CRC32C and the weak world fingerprint are
not substitutes. The intended network path copies received native object bytes
unchanged, then builds receiver-specific fractional indexes as detailed in
[network admission](network-admission.md).

`multiverse<P>` owns an existing object-directory path. It opens objects,
forwards `seal_object` to the same-policy writer, and opens prepared mmap query
chains with `open_query`. It exposes same-policy aliases for `sort`, `blob`,
`file`, `object_writer`, `mapped_native`, `mapped_index`, `mapped_blob` and
`mapped_query_root`, plus forward-declared `world`, `timeline` and `branch_point`
types. `sort<P>` validates one code's packing and unit
alignment; it does not establish prefix freedom of an entire registry. Mapped
files and slices outlive the reader object. There is no SQLite connection or
persistent aggregate implementation behind these forward declarations; the
optional `sqlite_catalog<P>` is a separate owner for saved roots and reservations.

### Portable sections and mapped query chains

`encode_native_sections` and `encode_index_sections` expose borrowed chunks for
the immutable writer. Their fixed directories describe ordinary FC bytes,
Elias–Fano words and samples, packed rank classes/checkpoints, false-borrow
flags and exact cut LCPs. The index records its native identity and the exact
downstream pair. Section positions count physical bytes; the inner FC extent
and residual universe retain the policy's byte/bit units. Fixed-width values
remain subtracted from that inner universe. The encoders do not re-encode keys
or build a second navigation directory.

`mapped_native`, `mapped_index` and `mapped_blob` retain mappings and expose
the same profile/navigation views used by owning blobs. Typed opening reads
only the envelope and fixed directory. `open_mapped_query` follows declared
identities, shares repeated native owners, rejects cycles and missing targets,
and adopts an already-bounded head. The caller's catalog authenticates the
physical object identities; opaque IDs alone are not content digests.

Explicit `scan()` operations verify CRC, canonical section padding, ordinary-FC
framing/order, physical checkpoints and rebuilt EF/rank directories. A mapped
pair's scan also recomputes its interleaving, false-borrow bits and cut LCPs,
then checks borrowed samples against the exact target's augmented stream.
These semantic scans can reconstruct and compare keys; normal queries do not
silently scan untouched data. Successful readback is not evidence that an
earlier failed persistence barrier became durable.

The independent mapped suite covers byte/bit and fixed/variable policies,
independent physical/virtual widths, exact match/source/value oracles,
unaligned slices, malformed descriptors and CRC-valid semantic corruption.
Protected pages distinguish fixed-metadata opening from payload access.
Ownership cases include paused/copied cursors, shared native files, unlinking,
moved-from owners and missing dependencies. The [mapped format](mapped-blobs.md)
records the complete layout and lifetime contract.

### Complete encoded-chain queries

`query_root<P>` prepares an arbitrary exact-linked head for search. If it already
fits in one policy group, the shared pointer is unchanged. Otherwise,
`query_root_builder<P>` constructs an empty-native routing prefix through one
streaming pipeline until its head fits. Preparation visits existing chain
metadata once, samples the original head once and streams diminishing sample
sets. It preserves all existing native/index bytes and exact target pins.

`query_cursor<P>` owns its query and unvisited target suffix. Each step visits
at most the caller's catalog budget and pauses at a native match. Taking the
match returns its owned value, ordinal and exact source pair. Equality continues
to route downstream, and full boundary lengths are retained alongside the
exact query agreement and comparison direction. Independent copies can progress separately. Decoding
failure makes the cursor unusable rather than resuming partial work.

The [query contract](query.md) separates entry/header bounds from string bytes,
preparation and scheduler costs. Shape validation rejects cycles, missing
targets and mismatched sample counts, but does not authenticate manually pushed
sample keys. The existing exact-sampler precondition and immutable-alias contract
remain in force. The shared query machinery retrieves entries from owning
encoded pairs or mapped pairs; it does not evaluate arrows or publish durable
worlds. `adopt_prepared` checks an existing bounded chain without sampling it.

The [whole-query comparison](../bench/query_compare.md) includes query
creation, five- or six-catalog traversal and owned values. On this M2 Max,
ordinary FC with exact cut comparisons reduces median query time by 38–48%
across the six measured cases. Counted backing arrays change by less than 1%;
root preparation has mixed results. Physical width 16 is slower than 15 in
these scalar fixtures. The report records exact revisions, independent
result checks, raw trials and the limits of its storage accounting.

The [shared-view follow-up](../bench/shared_query.md) caught a 4–9% owning-query
regression from repeating checked reader construction. Builder-owned arrays
now create shape-only views; public untrusted readers retain their content
checks. The same six resident-memory fixtures return to timing parity with
the preceding query implementation, with identical counted arrays.

The [small-count decoder](../bench/small_count.md) handles bit counts 0–254 from
one bounded 16-bit field and retains the checked general decoder for longer
codes and short tails. The six whole-query fixtures show 26.0–26.9% less bit
query time; byte timing ranges overlap. Encoded arrays are identical. The
independent count oracle covers every starting alignment, truncation, short
tail and the fast/fallback boundary; the affected profile/query/package
sanitizer checks passed. These are resident-memory M2 Max measurements.

The implementation was reviewed and integrated at `4285e6b`, with moved-from
preparation guards at `79fca75`. The independent query suite checks 20 policies
against native-array oracles for exact source, ordinal, value and match order.
They exercise large and empty roots, zero budgets, pauses, copied cursors,
truncated long boundary contexts, native/borrowed equality across a cut, source
reclamation and malformed chain shapes. The matrix includes physical widths
1, 16 and 64 independently of cascade stride, and both byte and bit policies.

### Incremental native output and ordered merges

`profile_native_writer<P>` accepts one unique sorted key/value pair at a time.
It retains the preceding key, encoded output and one residual offset per
physical block. The common value width is chosen before writing, from the
policy or an explicit per-stream promise. Rejected appends preserve accepted
records; final EF construction transfers the array on success. With the same
value-width choice, bytes and navigation agree with batch encoding.

`profile_blob::adopt_native` moves such an array into a native-only pair and
constructs its zero rank/cut directories without decoding the native keys.
`native_merge_builder<P, Native, Compose>` pins two ordered inputs and resolves
one distinct key per step unit. Equal keys call the supplied chronological
composition; replacement is the default. Sources may be owning arrays or
mapped-native owners. Exact LCP lengths carried between the heads avoid
rechecking their inherited prefixes. Shared frame output keeps only the previous
key's length, leaving two decoded input keys and no third output-key buffer.
The [merge measurements](../bench/native_merge.md) report 12.50–80.31% lower
medians across twelve byte/bit, value-width and prefix fixtures, with exact
payload and EF equivalence. A step failure poisons the continuation while retaining
its inputs. The [native merge guide](native-merges.md) distinguishes the key
budget from string/allocation work and durable checkpointing.

The independent in-memory fixtures check exact wire equivalence, sparse
offsets, partial blocks, unique-key rejection, moves and input lifetimes. The
merge oracle checks replacement and both parenthesizations of an associative,
noncommutative value operation. These builders do not interpret tombstones,
evaluate endpoints or establish a bounded redundant-level schedule.

Borrowed output now reserves its private predecessor buffer before fallible
writes and updates only the actual changed suffix after those writes succeed.
The [construction measurements](../bench/borrowed_prefix.md) report 26–46% less
append/finalize time across six byte/bit prefix fixtures, with identical encoded
bytes, metadata and EF arrays. Allocation-failure injection verifies rollback
and retry without changing the previously accepted stream.

The native writer uses the same predecessor-buffer rule. Its
[measurements](../bench/native_prefix.md) show 18–44% less complete construction
time across twelve byte/bit, fixed/variable-value fixtures. Allocation injection
checks rollback and retry at every reached allocation. Both writers retain
capacity for the largest key seen until finalization or destruction. The mapped
merge tests independently check replacement, concatenation and fixed-width
affine composition, paused/moved continuations, unlinked input mappings, and
sealed-output queries.

### Persistent immutable saves

The optional `sqlite_catalog<P>` records reservations before output creation,
sealing receipts, exact prepared chains, immutable saved roots and durable
reader pins. Its normal registration path reads metadata only; explicit scan
admission verifies the pinned chain before taking the SQL writer lock. All
mutations record exact request and outcome bytes under an operation ID in the
same transaction. Replays compare the complete request, including binary IDs.

The adapter requires SQLite 3.51.3 or later in both headers and the loaded
runtime, a serialized connection, verified WAL/FULL synchronization settings
and foreign keys. Opening checks the required table and trigger definitions,
rejecting unexpected triggers on protected tables. Storage errors and
unacknowledged COMMITs poison the handle. Existing saves, reservations and pins
are never released by this insert-only component.

Focused and independent adversarial tests passed ASan/UBSan with SQLite 3.53.4.
They cover real seal/save/close/reopen/query operations, concurrent connections,
exact replay, binary names, before/after-COMMIT acknowledgment failures, policy
and schema rejection, path aliases and retained input/output ownership. The
adversarial suite also passed Release, and a separate link rejected the older
system SQLite 3.51.0. Relocated `everett::sqlite` consumption and a core consumer
with SQLite discovery disabled both passed. The [component guide](sqlite-catalog.md)
states the distinction between these checks and physical power-loss recovery.
A separate POSIX process-interruption suite passes 20 `SIGKILL` cuts: before
and after actual COMMIT at eight catalog operations, plus four sealed outputs
whose receipts have not been recorded. Fresh connections check the exact
operation prefix, individual pins and targets, old saved queries and replay
without duplicating ownership. No inherited SQLite connection is used.

### World semantics and algebra

- `reference_world` pins immutable sorted runs. Snapshots and forks share them;
  eager reference compaction produces a new run while retained worlds keep the
  old runs.
- `partition_round` holds one immutable base throughout the round, checks
  arbitrary key ownership functions and old values, and accepts disjoint
  batches in any order. Tests cover all six permutations of three batches.
- Identical delivery within a live `partition_round` is replay-safe; reused
  identities with different contents and overlapping writes are rejected.
  Genuine inserts and deletes update the live count.
- Changesets advertise an algebraic delta. Receivers independently recompute
  it before publication; tests reject tampered first delivery and replay.
- Fingerprint policies use addition, subtraction and multiplication, without
  division. Wrapping 64-bit arithmetic and GF(2^8) are exercised policies.
- `save`/`restore` use a versioned **resolved-table reference export**, with
  magic `EVRTREF1`, an explicit value codec and binary-safe keys. This is not
  the intended small manifest of pinned objects and does not establish crash
  durability.

The world layer supplies a semantic oracle for attaching encoded blobs. Its eager
ordered-map resolution is not the intended merge/query algorithm, and it has no
logarithmic active-run-count guarantee. Batch generation may share an immutable
base, while applying batches to one accumulator is serialized.

The export does not persist round identity, accepted batch IDs or claimed keys.
Durable update-round resumption requires additional manifest and replay metadata.
The current hash interface is homogeneous; the sort- and full-key-dependent
potential selection described in [keys.md](keys.md) and [arrows.md](arrows.md)
remains a policy-interface extension.

### Pin ownership

`pin_set` is the actual `reference_world` state owner. Entries hold exact object
identities, immutable pins, additive contributions and optional own-record
fingerprints. The world signature comes from the owner's cached aggregate.
Replacement validates expected identities and contribution preservation before
publishing a new owner; previous owners retain their objects. Object identity
is distinct from its weak fingerprint.

Base entries contribute their table fingerprint, and update entries contribute
validated endpoint deltas. A merge contributes the sum of the entries it
replaces. A tombstone's zero native fingerprint cannot cancel an older value by
itself. Index-only dependencies and staged alternative representations retain
objects without becoming extra logical contributions.

The generic owner does not assign chronology to arbitrary partial replacements;
the caller must preserve semantic precedence. Current reference compaction
replaces the entire set. This reference owner uses process-local IDs and shared
pointers. The separate SQLite catalog uses persistent opaque object IDs for
saved encoded chains; attaching the reference owner to those objects and
implementing disk reclamation remain work.

### Durability and merge resumption

`merge_publication` is a bounded publication/checkpoint state machine with
failure-injection tests. It models verified backend events; it performs no
filesystem writes and supplies no durable save backend. Output, checkpoint,
manifest and old-root retirement have distinct transitions.

Failed synchronization retains the prior recovery root and inputs. Resumption
after a manifest attempt additionally requires explicit selector reconciliation.
Tests model the failed-writeback/successful-retry trap, checkpoint identity and
context checks, and both outcomes of uncertain manifest publication. The
[durability protocol](durability.md) specifies the required future backend.
No physical power-loss or process-restart validation is implied by model tests.

### Abstract Lean model

The independent [proof project](../proof/README.md) pins Lean 4.19.0 and uses
core/Std without mathlib. Its theorems cover dependent per-key
categories, source-checked updates, chronological merging and reassociation,
disjoint-update commutativity, integer endpoint potentials, exact-target pin
closure, snapshot adoption, eligible reclamation and allocation without ID reuse.
`adopt_adjacent_merge` supplies the semantic adoption premise from the proved
history-composition law.

The fractional-index layer models sorted tagged occurrences, stable merging,
every-$K$th sampling, endpoint rank projection, and executable window lookup
equivalent to full predecessor lookup. False-borrow flags certify a matching
native occurrence; native-key uniqueness within a run makes its recovery a
single rank-derived probe, even before the routed window. Mathematical rank
inside the window is derived from a boundary rank and a local scan, without
assuming arbitrary-position constant-time rank. Abstract sampled indexes name
their exact catalog targets and preserve correspondence across extension and
eligible reclamation.

The default `lake --wfail build` checks the proofs, examples and a transitive
axiom audit. The interpreted examples cover duplicate keys across cuts, empty
projections, missing predecessors, and partial tails for $K=3$ and $K=15$.
Keys in the fractional-index layer are natural numbers representing order; it
does not decode string keys or the catalog's physical target graph. Separate
`Prefix` and `Transfer` modules establish the string/algebraic pieces of the
[ordinary-FC design](comparison-fc.md): the exact LCP minimum for three ordered
finite strings and associative content-mismatch transfer under the invariant
that each literal mismatch lies at or beyond its retained prefix. Direction
travels with the selected mismatch; endpoints remain separate.

`Frontier` proves the sorted-merge comparison laws for carried LCP lengths:
unequal lengths decide head order, equal lengths permit a suffix-only comparison,
and either case recovers the exact LCP between the two heads. Equal keys and
proper prefixes remain included; C++ cursor state maintenance is a separate
refinement obligation.

Those theorems do not verify stored cut-LCP scalars, literal comparisons or the
encoded decoder. Compressed rank, Elias–Fano, front coding, full cascade execution, C++ refinement, scheduling
and crash recovery remain outside its scope. The proof README records the assumptions
and the distinctions between endpoint projection, full arrows and finite-key
fingerprint sums.

## Specified extensions

### Aggregate API and key policies

The read-side `multiverse<P>` and associated type family are implemented as
described above. Persistent `world<P>`, `timeline<P>` and `branch_point<P>`
runtimes remain to be attached to the selected SQLite catalog.
`reference_world` provides the executable in-memory semantics.

[Sorts and key policies](keys.md) describes sort-qualified keys, key units,
prefix-free coding and hash selection. The category may depend on the full key,
even when a sort supplies default policies. The typed codecs and homogeneous
replacement oracle do not yet implement a heterogeneous sort registry.

### SQLite catalog and network admission

SQLite is the selected home for logical worlds, immutable representations,
exact pins, contributions, index dependencies and small merge continuations.
The [catalog design](catalog.md) specifies publication, operation identities,
reader/GC synchronization and SQL diagnostics. The [implemented adapter](sqlite-catalog.md)
covers immutable roots and reservations. Mutable timeline publication, ownership
retirement, schema migration and resumable job execution remain extensions.

Direct network adoption keeps compatible native bytes and their sampled offsets
intact. The arbitrary-prefix recurrence bounds borrowed **entries**, without
requiring monotone native file sizes. It does not prove a byte bound for repeated
long sampled keys or a bounded-depth batch scheduler. The
[network analysis](network-admission.md) records both remaining obligations.

### Strong deletes and global rebuilding

[Strong deletion by incremental rebuilding](rebuild.md) adapts the
Overmars–van Leeuwen weak-to-clean transformation, §2, Theorem 1. It specifies
early triggers, the construction-plus-replay work inequality, carried replay
debt, overwrite cleanup and durable continuation. The record-count argument
includes full older coverage and a finite admission cut; byte costs require
additional explicit work budgets.

The global rebuilder is not implemented. Eager reference compaction does not
exercise its schedule, bounded catch-up or durable publication.

### Categorical updates

The [per-key category design](arrows.md) extends the semantics to composable
diffs. It specifies composition, partition independence, exact endpoint deltas,
query costs and dependency retention. `reference_world` resolves replacements
using optional values, and `profile_blob<P>` carries opaque value payloads.
There is no generic arrow executor, category-dependent wire format or general
normalization bound. A second concrete instance should test noncommuting changes
before broadening the executor interface.

## Build and verification

Configure, build and run the standalone component suites with CMake/CTest:

```sh
cmake -S . -B build -DEVERETT_BUILD_TESTS=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

For ASan/UBSan on a supported toolchain, use a separate build directory:

```sh
cmake -S . -B build-sanitize -DEVERETT_BUILD_TESTS=ON -DEVERETT_SANITIZERS=ON
cmake --build build-sanitize --parallel 4
ctest --test-dir build-sanitize --output-on-failure
```

The default component suites cover codecs, native and borrowed writers, index
construction, queries, world semantics, ownership, durability and mapped files.
With SQLite enabled, four more suites cover the catalog, adversarial operations,
forwarded VFS failures and process interruption. Three package consumers check relocated core and
SQLite installations and embedded use. Doxygen is an optional additional check.

Combined verification on 2026-09-15: AppleClang 21, C++20, Release with strict
warnings and ASan/UBSan passed all **32 CTests**, including three package consumers
and Doxygen, in 97 seconds. SQLite headers and runtime were 3.53.4. This run covers
ordinary-FC comparison, complete owning and mapped query chains, portable
unaligned navigation, native construction and incremental merges, immutable
writes, metadata-only opening, and persistent saves and reader pins.

The writer tests cover failure at every syscall position, short/interrupted
writes, disk-full errors, uncertain installation, close failures and retained
real outputs. The SQLite VFS suite injects 114 errors before and after reached
write/sync calls, checking old-root retention and complete-or-absent operation
rows through fresh connections. The 20 process-interruption cases additionally
check actual writer death, including committed-but-unacknowledged operations.
These tests do not establish behavior under physical power loss.

Doxygen checked 29 public headers and 26 real declaration associations, with
clean generation that removes obsolete pages. The proof checkpoint checked
711 Lean declarations with only standard `propext`, `Quot.sound` and
`Classical.choice` axioms.

All seven complete README programs and the native-merge and SQLite guide
examples passed strict warnings and ASan/UBSan, nine executables in total.
The eight core examples have no SQLite linkage; only the catalog example links
it. Installed licenses and generated CRC includes are checked byte for byte
against the source bundle, and the pinned generator reproduced all eight
backends. Windows execution coverage is limited to the recorded rank component
tests. Network transport and durable merge resumption remain separate work.

The optional `EVERETT_BUILD_DOCS` configuration generates Doxygen HTML/XML and
checks all file footers plus representative function/member ownership. A
two-file fixture compares top, bottom and split file documentation across namespaces,
same-name classes and overloaded functions. The license aliases render
SPDX as a code block and remove the unconfigured unknown-command warnings.
See [the documentation check](doxygen.md) for the exact assertions and limits.

## Next implementation assignments

| Work item | Dependencies | Concrete acceptance |
| --- | --- | --- |
| Sort registry | typed byte/bit policies and canonical key contracts | prefix-free framing and order, cross-sort boundaries, domain-separated hashes and stable policy versions |
| Per-key arrow policy and second instance | categorical specification and replacement oracle | noncommuting diffs, heterogeneous keys, source validation, associative semantic composition, disjoint permutations, endpoint deltas, checkpoint observations and explicit work/dependency accounting |
| Comparison block encoding | ordinary FC, exact cut LCP and scalar comparison transfers | transposed count/literal layouts, ordered SIMD transfer scans, bounded tails and independently measured time/space tradeoffs |
| Object identity and integrity | portable sections, mmap queries and immutable writer | cryptographic content addressing, durable catalog publication and lazy block-integrity strategy |
| Attach encoded runs to world semantics | blob reader and query | batch/snapshot/export oracle tests using actual encoded immutable runs |
| COLA scheduler and durable merge continuations | incremental native merge and index builder | byte/work-budgeted continuations, bounded active levels and shared-result adoption under interleaved forks |
| Mutable catalog roots and pin retirement | immutable saved roots, reservations and exact file graph | conditional timeline publication, reader retirement, reclaim only after final pin, schema migration and interruption tests |
| Direct batch adoption | native file reader, prefix index builder and scheduler | preserve received ordinary-FC bytes, bound visible catalogs and work debt, preserve causal order and charge actual key bytes |
| Durable backend and resumable merges | publication protocol and encoded merge continuations | fault injection at write/sync/rename/recovery cuts; failed barriers retain old roots; resume only from verified durable prefixes |
| Durable round resumption | save manifests and update protocol | persist base/round identity, accepted batch identities and claimed keys; restart without double-applying a changeset |
| Live-size rebuilding | scheduler and mutation accounting | replacement ready before half the clean base disappears; repeated overwrites do not grow history-sized active levels; preserve replay debt and explicit byte budgets |

Separate files versus extents in managed blobs, transport and client integration
remain policy choices. The library's correctness contracts must remain explicit
when those implementations are selected.
