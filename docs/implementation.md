# Diet implementation status

Updated 2026-09-15. Specification: [Diet design](design.md).

This ledger records what works, what the tests establish, and what remains to
be built. The C++20 foundations live in `include/diet/`, in
namespace `diet`. Immutable files, mmap queries and saved catalog roots work;
the encoded mutable runtime now executes real carries, including a three-slot
redundant-level scheduler with explicit service obligations.

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
| Sort registry | `registry.h`, `policy.h`; `tests/registry.cc`, `tests/registry_compat.cc` | typed discriminator dispatch, width/unit inference, stable-code extension, file-local framing and catalog reopen under broader defaults |
| Mutable tap | `tap.h`; `tests/tap.cc` | serialized immutable publication, bounded accepted input, readiness backpressure, cancellation, shutdown, exact logical identity and worker failure |
| Named typed connection | `connection.h`; `tests/sqlite_catalog_connection.cc` | mutable and asynchronous commands, mapped snapshots, exact saves/forks, restart, stale publishers and healthy input rejection |
| Encoded runtime | `cola_runtime.h`; `tests/cola_runtime.cc` | chronological runs, real native/index/carrier work, immutable publication, budget partition, mmap restoration and failed continuation isolation |
| Redundant runtime | `redundant_runtime.h`; redundant and typed-redundant tests | three-slot ownership, overlapping main/secondary jobs, charged admission, exact frontier checkpoints and recovery gates |
| Runtime persistence | `runtime_store.h`, `runtime_checkpoint.h`, `redundant_checkpoint.h`; runtime and redundant catalog tests | exact graph sealing, hidden completed artifacts, atomic auxiliary pins, saved frontiers, mapped reopening and interrupted-stage restart |
| Typed updates | `typed_cola.h`; `tests/typed_cola.cc` | replacement reads, chronological arrows, per-sort dispatch and hashes, validated deletes, disjoint contributions, mutable commands and snapshot metadata |
| Sort-owned record codec | `sort_codec.h`; `tests/sort_codec.cc` | heterogeneous FC/raw/integer grammars, optional/niche/no-payload values, typed stream anchors, control parsing and borrowed-role output |
| Sort-owned physical profiles | `sort_profile.h`, `sort_profile_file.h`, `sort_profile_merge.h`; `tests/sort_profile.cc` | KV03 native framing, shared selector seeds, mapped cascading queries, prefix-preserving merges and protected-page entry |
| Sort-owned runtime and persistence | `sort_runtime.h`, `sort_runtime_store.h`; sort-runtime and catalog tests | direct heterogeneous records, chronological composition, complete redundant frontiers and metadata-only mapped recovery |
| Replacement rebuilding | `replacement_rebuild.h`; replacement and durable-rebuild tests | paid physical scans, FIFO replay, carried generation debt, active saves/forks and gated recovery after interruption |
| Resolved scans | `typed_scan.h`; typed and mapped scan tests | ordered rows, newest replacements, chronological arrows, tombstone elision, bounded traversal and snapshot ownership |
| Typed profiles and backing reader | `policy.h`, `profile.h`, `profile_blob.h`, `fridge.h`; profile/blob/fridge tests | byte/bit and value-layout matrix, ordinary FC, exact cut LCP, same-policy aliases and unchanged native allocation on reindex |
| Complete encoded-chain queries | `query.h`; `tests/query.cc` | bounded root preparation, exact target traversal, all native matches, partial contexts, cursor budgets and ownership |
| Native construction and merging | `native_writer.h`, `native_merge.h`; native writer/merge tests | streaming record acceptance, preserved FC/EF bytes, chronological composition, input pins and failure state |
| Persistent catalog | `sqlite_catalog.h`; focused, adversarial, VFS and process-interruption tests; optional package consumer | reserved IDs, exact prepared graphs, close/reopen saves, binary operation replay, uncertain commits and conservative pins |
| Cola semantics and ownership | `fingerprint.h`, `pins.h`, `cola.h`; `tests/cola.cc`, `tests/pins.cc` | disjoint batch permutations, snapshots, old-value validation, contributions, replay and reference export |
| Design documentation | [usage](usage.md), [design](design.md), [arrows](arrows.md), [rebuilding](rebuild.md), [durability](durability.md), this ledger | working examples, consistent contracts, cited derivations and implementation limits |

As components change, we update this ledger with the reviewed revision, actual
checks and remaining limits. Host-specific resource coordination stays outside
this package.

## Implemented foundations

### Active runtime and named frontiers

`fridge<>::create(path).connect(name)` opens a default bit-profile string table.
The [connection](connection.md) serializes mutable commands, publishes durable
mapped results and services merge work in the background. Its synchronous
`persistent_engine` is also available to caller-owned scheduling loops. Tests
cover concurrent same-key commands, noncommutative arrows across reopen, saved
generations, forks, rejected absent deletes and publication failures. The
installed SQLite consumer exercises the README workflow.

`cola_runtime<P, Compose>` admits encoded records, creates a real private binary
carry queue, and publishes completed equivalent layouts. Its immutable snapshots
retain chronological admission intervals independently of duplicate collapse.
`admission_ready`, `try_contribute`, `admission_cost` and `next_service_cost`
separate new admission from existing debt. The next admission waits for the
current carry; this is conservative backpressure, not the three-slot redundant
schedule's worst-case update bound. Structural charges cover executed stages;
codec finalization remains atomic and byte/callback costs remain separate.

`redundant_runtime` executes the main/secondary/shadow schedule with actual
native, destination-index and carrier jobs. The typed family offers logarithmic
service after each admitted record and waits only for admission readiness.
Tests include simultaneous unsafe levels, every-stage checkpoint/restart,
visibility and chronological-coverage oracles, failed-publication isolation,
and K=3/K=15 budget partitions. Full-frontier metadata includes hidden completed
outputs. The persistent adapter seals and pins that complete closure in the same
transaction as the query root and semantic metadata. The
[runtime guide](redundant-runtime.md) distinguishes these checks from a universal
proof and from byte or latency guarantees.

`runtime_store<P>` seals that exact graph, reuses known native owners during
reindexing, and publishes its root plus small checkpoint through schema 4.
Reopening maps the published files and validates frontier metadata without
decoding payloads. Unfinished private work restarts from the published inputs.
The focused ASan/UBSan suites check equivalent layouts, noncommutative merges,
protected-payload restoration, historical snapshots, reopened carries and
competing durable publishers. Redundant checkpoints retain every completed
artifact through each merge stage; deliberately omitted auxiliary pins are
rejected on restoration. The [runtime persistence guide](runtime-store.md)
states the current retention and identity-allocation boundaries.

`tap<Engine>` provides serialized mutable publication and bounded accepted
inputs. Optional readiness blocks new claims behind prior engine debt. An
engine can certify a preflight rejection left state unchanged; only that ticket
fails. Uncertain or partial execution failures stop the worker. Focused tests
exercise both paths, shutdown during required service and old snapshot ownership.

`typed_engine<>` supplies the default bit-profile optional-string table. Its
registry reserves code one for extension and assigns code zero to the current
sort. Static command factories support ordinary mutable writes; snapshot
factories validate each touched old value, allowing disjoint contributions from
one base in either order. Sorts supply chronological composition and hashing;
dispatch bits never enter the signature. Tests include a noncommutative append
sort, different sort-code layouts with matching signatures, and byte/bit map
oracles. The default backend transports canonical ordered keys and arrow
payloads through the ordinary FC profile. The opt-in `sort_runtime_family`
uses each sort's key and value grammar directly in KV03 files, with the same
redundant scheduler, cascading queries and prefix-preserving merges.
`sort_runtime_store` persists and restores its complete frontier, including
hidden completed outputs. Its normal open validates metadata; explicit scans
check the payload and cross-file samples.

`sort_profile_file_writer` and `sort_profile_file_merge` stream native payloads
through a 64 KiB buffer. Sparse offsets, block seeds and the file's sort
dictionary remain in memory until finalization. Tests compare complete bytes
with the owning encoder, bound allocation for large values and unary controls,
and preserve mapped inputs while builders pause or their filenames are unlinked.
Connecting these file writers to runtime execution is separate from the
completed owning-runtime and durable-publication path.

### COLA main and secondary indexes

`cola_index<P>` and its incremental builder construct two borrowed streams over
an unchanged native array. Main samples the next augmented main catalog;
secondary samples a terminal native array. Two packed rank directories refer
to one three-way virtual order. Each borrowed stream has its own FC/EF, flags
and cut LCPs. `cola_query_root` includes both level-zero arrays and the cursor
preserves matching native contributions from both roles.

`encode_cola_sections`, `mapped_cola_index`, `mapped_cola_blob` and
`open_mapped_cola_query` implement IX03 encoding and metadata-only mapped
opening with exact main/secondary pins. Explicit `scan` validates payloads and
target samples. The [COLA guide](cola-indexes.md) describes the layout and API.
`mapped_cola_index_builder` constructs those routes directly from retained
mapped inputs. Its four-policy tests check exact IX03 equality with owning
construction, paused and moved builders, unchanged native bytes and addresses,
and continued construction after the source files are unlinked.
The [complete persistence example](cola-store.md) writes native files, saves
both input and combined roots, publishes a merged timeline, then reopens and
queries all three representations.
Terminal COLA construction reads only the admitted native count and emits zero
navigation. A protected-page test hides the entire native mapping through
construction, moves, finalization and IX03 serialization, then compares the
result with the ordinary key-walk encoding. The nonterminal builder reuses its
exact carried LCP for borrowed FC output.
The sampler and index builder also retain exact three-way comparison frontiers;
the builder needs only the preceding key's length. The
[frontier benchmark](../bench/cola_frontier.md) records complete-build measurements
and byte-identical output across all compared variants. Lean's ordered-walk
theorem establishes the adjacent-LCP minimum law used by the comparison argument.
The independent eight-policy core and mapped suites passed strict O3
ASan/UBSan. The mapped suite independently assembles IX03 bytes, checks exact
source/ordinal/value query results, mutates valid-CRC files, verifies target
identities and mapping lifetimes, and protects both borrowed payloads during
metadata-only opening.
The focused sampler tests check exact endpoint comparisons against original
bit strings, including redundant front coding and LP restarts in every stream.
The route-stability tests independently encode FC and EF, verifying unchanged
route bytes while the other target, local natives, ranks and cuts change.
Five COLA suites also passed strict Linux Clang 20 ASan/UBSan and leak checks;
the [Linux report](../bench/cola_linux.md) records the exact scope and sources.

The separate [scheduler model](cola-scheduling.md) executes fixed-admission
main/secondary/shadow transitions with immutable identities and explicit work
counts. At `b11957d`, strict O3 ASan/UBSan and Release checks covered 131,072
admissions, 131,047 completed merges and 32,743 hidden merge inputs. Independent
event replay checked intermediate roots, chronological coverage, slot reuse,
snapshot unions and budget splitting. This is an executable count model;
production scheduling, byte/I/O service and durable continuation remain work.

`cola_local_merge_job<P, Compose>` executes one owning native/index/carrier job
with exact main or secondary destination plans. `step` counts work in the
current stage; `finish_stage` explicitly performs finalization and advances to
the next stage. It retains source and plan owners on failure and exposes only a
completed result. Six-policy tests cover noncommutative composition, both plans,
pauses and moves, exact batch index bytes, retained old roots and full queries.
Separate allocation tests inject 32 first/last failures in stage finalization
and index/carrier growth, checking poison, rejected reuse and retained pins.
The [local merge guide](cola-merges.md) demonstrates the API. Slot assignment,
root publication and durable continuation remain separate.

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
| [Absolute block framing](../bench/query_absolute.md) | Complete queries, preparation and navigation work after removing predecessor-length reads |
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
Subview formation checks the requested range once and preserves the containing
view's validated storage bounds. Exhaustive small-span tests cover nested
slices, empty endpoints and invalid ranges.

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

`storage_policy<Registry, GroupSize, BackspaceCode, CodecBlockSize>` derives
units and the common fixed-value-width hint from its occupied sorts. Each sort
exposes an `encoding` with `unit` and optional `fixed_value_bits`.
`byte_encoding<fixed_values<N>>` counts bytes; `bit_encoding<fixed_values<N>>`
counts bits, including the valid width zero. `profile_array<P, Role>` uses
canonical byte varints and policy-selected Golomb or exponential-Golomb bit
backspaces. Other bit counts use order-zero exponential-Golomb. Streams retain
meaningful bit extents and canonical padding. Each physical block's first record
stores an absolute retained-prefix count; later records use relative backspaces.
Header-only entry needs no read of the preceding block. Profile metadata,
native KV02 directories and single-route IX02 directories use version 2;
two-route IX03 directories use version 3. The terminal key length supports
sequential checks and explicit endpoint length access. Common fixed
value width is subtracted from the Elias–Fano residual positions.

`profile_blob<P>` uses ordinary front coding for native and borrowed streams.
It retains two physical offset directories, grouped origin rank, false-borrow
flags and one exact bit-LCP scalar per virtual cut. The LCP relates the cut
boundary to its preceding borrowed key. Reindexing shares the exact native
allocation and recomputes all dependent index metadata. The batch builder
materializes native keys as scratch during reindex.

`profile_query_context<P>` owns its immutable query and carries exact agreement
in bits, an optional key length in policy units and comparison direction. The profile reader
parses controls before the selected lane without reconstructing keys. It then
compares literal suffixes and propagates inherited mismatches. Exact cut LCPs
repair the outgoing borrowed predecessor even when it precedes the window.
The known order establishes equality from the recovered LCP without reading
that predecessor's length or replaying its physical block.
Search returns both native values and downstream routing on equality; it does
not resolve same-key arrows. Optional counters expose skipped/visited record
headers and compared literal bits.

`profile_encoded_cursor<P, Role>` traverses encoded frames with borrowed literal
and value spans, without key allocation or payload reads. It checks framing and
terminal metadata; sortedness is a separate content property.
`profile_cursor<P, Role>` supplies sequential decoding with borrowed values;
`profile_borrowed_writer<P>` incrementally encodes borrowed keys. Their output
matches the batch encoder across the policy matrix. Ordinary advancement and
advancement with an adjacent-key comparison select separate compile-time modes.
The [cursor comparison](../bench/sample_advance.md) records the smaller ordinary
body, consumer-dependent code size and broadly flat pipeline timings.
`sample_cursor<P>` pins
and samples an exact encoded pair without materializing its catalog.
`index_builder<P>` retains bounded sample/decoder state, records each cut's
exact LCP and preserves native allocations. `index_pipeline<P>` feeds samples
directly between stages, supports bounded cursor-event stepping and returns a
chain with exact target pins. Inter-stage samples carry a policy-unit backspace
and suffix; the first sample is literal and later samples refer to that
producer's preceding sample.
The sample encoder obtains sortedness and retained-prefix length from one
comparison. Its [direct pipeline measurement](../bench/sample_frontier.md)
has 3.2–4.6% lower medians with 4 KiB shared prefixes and roughly unchanged
short-prefix results; all observed trial ranges overlap. Exact occurrence,
encoded-output and work-counter checks accompany the timings.
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
string-store I/O theorem. Semantic pair framing and active per-sort hash/category dispatch remain
extensions; the prefix-free registry and typed discriminator dispatch are implemented.

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

`object_stream<P>` retains an exclusive private attempt while accepting body
chunks across calls. It reserves the envelope and optionally a fixed body
prefix, then backpatches those private regions at finalization. CRC32C combination
updates the checksum for that prefix replacement without rereading the suffix.
Metadata rejection before final writes is retryable; I/O errors poison the
stream and leave surviving names available for reconciliation. Tests cover
prefix-only and mixed bodies, bit tails, partial writes, no-clobber installation,
operation ownership and failures at each sealing step. The CRC combination
oracle separately covers every bit of a 64-bit suffix length.

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
and verification are still pending; CRC32C and the weak cola fingerprint are
not substitutes. The intended network path copies received native object bytes
unchanged, then builds receiver-specific fractional indexes as detailed in
[network admission](network-admission.md).

`fridge<P>` owns an existing object-directory path. It opens objects,
forwards `seal_object` to the same-policy writer, and opens prepared mmap query
chains with `open_query`. It exposes same-policy aliases for `sort`, `blob`,
`file`, `object_writer`, `mapped_native`, `mapped_index`, `mapped_blob` and
`mapped_query_root`, plus forward-declared `cola`, `timeline` and `branch_point`
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

`index_builder<P, Native>` accepts an owning array or pinned mapped native
object, and `sample_cursor<P, Target>` can sample either owning or mapped exact
pairs. `finish_index` returns an independent `profile_index<P>` for serialization
against the unchanged native identity. Its native bytes and offsets are never
copied or recoded. The artifact does not own dependency pins; its builder,
sampler and publishing caller retain them. `profile_index::native_only` builds
terminal zero directories from the admitted count without visiting native keys.

`file_index_builder<P, Native>` supplies a borrowed-stream file sink to that
same builder. `profile_file_output` shares count framing, bounded payload
buffering, residual offsets and section emission with native file construction.
The index sink fixes value width to zero even under a fixed native-value policy.
It emits the same nine IX02 sections as the owning index encoder. Structural
metadata checks precede final writes; sample provenance and exact LCPs come
from the builder. A complete semantic scan remains an explicit readback operation.

`file_index_pipeline<P>` shares the owning pipeline's bounded handoff loop.
Its source sampler pins the exact mapped target, and each stage pins an
unchanged mapped native file. Front-coded samples pass directly between
builders as their borrowed payloads stream to private files. `seal_next`
finalizes one stage in dependency order and retains its receipt; later failures
leave earlier receipts and all surviving names available for reconciliation.
The caller records receipts and publishes the completed graph through its
catalog. These construction APIs provide no partial-output restart operation.

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
to route downstream, carrying exact query agreement, comparison direction and
an optional full boundary length. Independent copies can progress separately. Decoding
failure makes the cursor unusable rather than resuming partial work.

The [query contract](query.md) separates entry/header bounds from string bytes,
preparation and scheduler costs. Shape validation rejects cycles, missing
targets and mismatched sample counts, but does not authenticate manually pushed
sample keys. The existing exact-sampler precondition and immutable-alias contract
remain in force. The shared query machinery retrieves entries from owning
encoded pairs or mapped pairs; it does not evaluate arrows or publish durable
colas. `adopt_prepared` checks an existing bounded chain without sampling it.

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
key's length. Replacement and two-argument value-only composition forward input
literals directly, retaining prefix spans to validate strict source order.
Three-argument key-aware callbacks retain reconstructed input keys. No path
needs a third output-key buffer. Redundant/LP inputs and deep fragment chains
are checked independently; span metadata can exceed contiguous key storage.
Each span now stores a source bit offset and cumulative logical endpoint in
16 bytes. The [compact span measurements](../bench/native_compact.md) show
31.4–58.0% lower peak requested allocation bytes in the fragmented-key fixtures
than the 40-byte descriptor. Those peaks still exceed materialized key buffers;
all runtime trial ranges overlap, so this is a measured space choice.
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

`native_file_writer<P>` streams the same framing into an immutable object
attempt. It retains a reusable predecessor key, 64 KiB of payload buffering,
bounded count scratch and residual block offsets. `native_file_merge` feeds
that sink through the ordinary merge builder's policy-checked output interface;
there is one merge algorithm for arrays and files. Finishing builds EF before
final writes, emits the directory sections and seals through `object_stream`.
The independent suite checks complete portable bytes across byte/bit policies,
fixed/variable values and different K/W, long unary controls, large borrowed
values, protected tails, geometric key growth, allocation retry and I/O poison.
Mapped input tests retain owners after unlinking, pause between steps, and
exercise both callback forms and malformed later records. These are live-process
continuations; they do not yet restore partial output after process restart.

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

### Persistent saves and timeline generations

The optional `sqlite_catalog<P>` records reservations before output creation,
sealing receipts, exact prepared chains, immutable saved roots, timeline
generations and durable reader pins. Its normal registration path reads metadata only; explicit scan
admission verifies the pinned chain before taking the SQL writer lock. All
mutations record exact request and outcome bytes under an operation ID in the
same transaction. Replays compare the complete request, including binary IDs.

`create` selects schema 2; `create_cola` selects schema 3, which admits both
linear IX02 and dual-route IX03 graphs. COLA registration records the main pair
and terminal secondary native edge with separate counts and requires every
seal receipt. Schema 3 tests cover exact replay, missing-secondary rollback,
mixed layouts, saves, timelines, reopen and registration COMMIT failures.
`create_timeline`, `fork_timeline` and
`publish_timeline` append immutable generations under the SQLite writer lock.
Publication compares the complete expected name, generation, head and owner;
stale requests record their observed head as a stable replay outcome. Forks
select the exact supplied historical generation. Every generation retains its
own root pin. Schema-1 catalogs keep the earlier save/reservation APIs; timeline
methods reject them without an automatic migration. Catalog schema, object
envelope and inner section versions are independent.

`create_taps` selects schema 4. Named taps publish an opaque runtime checkpoint
and prepared root pin atomically. Exact CAS and replay cover both; ordinary
timeline publication cannot bypass the checkpoint. Historical `fork_tap` and
`save_tap` retain the exact checkpoint with the root. The tap-catalog suite covers
reopen, stale and competing publications, binary metadata, historical replay,
save stability, schema protection and injected errors before/after COMMIT.
It does not itself interpret runtime metadata or resume private file writes.

The timeline suite checks binary names, competing connections, historical forks,
reopen/replay, malformed outcomes, generation limits, eight COMMIT-failure cases
and eight process-kill cuts. These establish the tested API/SQLite behavior,
not physical power-loss recovery or pin retirement.

The streamed publication suite adds 26 process-kill cuts across byte and
partial-bit profiles. It merges actual mapped inputs, interrupts private output
writes and sealing, then exercises receipt recording, index registration and
timeline publication. Fresh connections verify complete-or-absent publication,
exact operation outcomes and retained source, reader, historical and output
pins. An old save remains queryable throughout. These tests cover process
interruption; the partial merge still restarts from its inputs.

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
system SQLite 3.51.0. Relocated `diet::sqlite` consumption and a core consumer
with SQLite discovery disabled both passed. The [component guide](sqlite-catalog.md)
states the distinction between these checks and physical power-loss recovery.
A separate POSIX process-interruption suite passes 20 `SIGKILL` cuts: before
and after actual COMMIT at eight catalog operations, plus four sealed outputs
whose receipts have not been recorded. Fresh connections check the exact
operation prefix, individual pins and targets, old saved queries and replay
without duplicating ownership. No inherited SQLite connection is used.

### Cola semantics and algebra

- `reference_cola` pins immutable sorted runs. Snapshots and forks share them;
  eager reference compaction produces a new run while retained colas keep the
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
- `export_rc`/`import_rc` provide a **debug resolved-table dump**,
  conventionally a `.rc` file. This is not an intended access pattern; normal persisted access
  uses catalog object roots. The dump has
  magic `DIET.RC` with a terminating zero (eight bytes), then little-endian
  64-bit fields for format version 1, the value codec tag and record count.
  The fixed header is 32 bytes. Each live entry then contributes its key byte
  length, full binary-safe key and codec value in sorted order, without front
  coding. `import_rc` constructs a fresh reference table; callers own dump
  durability.

`snapshot()` shares the reference model's existing state and run owners without
materializing the resolved table. Catalog saves retain exact encoded roots;
reopening those roots does not import a debug dump.

The cola layer supplies a semantic oracle for attaching encoded blobs. Its eager
ordered-map resolution is not the intended merge/query algorithm, and it has no
logarithmic active-run-count guarantee. Batch generation may share an immutable
base, while applying batches to one accumulator is serialized.

The debug dump does not persist round identity, accepted batch IDs or claimed keys.
Durable update-round resumption requires additional manifest and replay metadata.
The current hash interface is homogeneous; the sort- and full-key-dependent
potential selection described in [keys.md](keys.md) and [arrows.md](arrows.md)
remains a policy-interface extension.

### Pin ownership

`pin_set` is the actual `reference_cola` state owner. Entries hold exact object
identities, immutable pins, additive contributions and optional own-record
fingerprints. The cola signature comes from the owner's cached aggregate.
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
refinement obligation. `Framing` proves that retained prefixes bounded by the
physical start, followed by bounded controls and payloads, keep reconstructed
key lengths within the admitted physical extent. This supplies the arithmetic
argument for checking absolute prefixes once per block; it does not prove the
C++ parser's extraction of those fields.

Those theorems do not verify stored cut-LCP scalars, literal comparisons or the
encoded decoder. Compressed rank, Elias–Fano, front coding, full cascade execution, C++ refinement, scheduling
and crash recovery remain outside its scope. The proof README records the assumptions
and the distinctions between endpoint projection, full arrows and finite-key
fingerprint sums.

## Specified extensions

### Aggregate API and key policies

The read-side `fridge<P>` and associated type family are implemented as
described above. Persistent `cola<P>`, `timeline<P>` and `branch_point<P>`
runtimes remain to be attached to the selected SQLite catalog.
`reference_cola` provides the executable in-memory semantics.

[Sorts and key policies](keys.md) describes sort-qualified keys, key units,
prefix-free coding and hash selection. The category may depend on the full key,
even when a sort supplies default policies. `registry.h` implements `tip<S>`,
`bin<L,R>`, `sort_list<S...>`, `unsorted<T>` and `sort_undefined`, with inferred
physical units/width hints, duplicate-sort rejection, typed visitor dispatch
and additive-extension checks. `storage_policy<>` defaults to
`unsorted<std::optional<std::string>>`; `value_encoding<T>` supplies encoding
requirements for tagless value types. It does not persist semantic schema
identity or supply leaf hash/update laws. `registry.cc` exercises code consumption, holes,
unaligned dispatch, return types and inference. `registry_compat.cc` checks old
fixed-width files and catalogs against broader registry defaults.

The registry, compatibility and benchmark-adapter workers were integrated from
`fe40f75`, `ac1acd9`, `e467c33` and `bb713d9`. Combined verification passed all
54 CTests, with strict O3 ASan/UBSan component builds, installed/embedded/SQLite
package consumers and Doxygen checks. Eight invalid registry/policy compile
probes rejected correctly. README and persistent-usage examples passed separate
sanitized build, reopen and replay checks. Historical benchmark fixtures still
compile against their pinned pre-registry headers; their measured data remains
unchanged.

File readers use the width encoded in each stream rather than demanding the
current registry's global width hint. Physical unit, sampling, block size and
count-code checks remain exact. `typed_engine` connects the registry to reads,
hash accounting, conditional updates and merge composition. Its current
default transport uses canonical ordered keys and encoded arrows in the ordinary
profile grammar. `sort_runtime_family` instead uses leaf-owned key/value packing,
including fixed-width integer keys, through mapped KV03 files and the complete
redundant runtime. Both transports use the same per-sort semantics.
The [schema-history extension](keys.md#schema-histories-and-migration) describes
multiple historical registries and forward migration without claiming that
runtime exists.

### SQLite catalog and network admission

SQLite is the selected home for logical colas, immutable representations,
exact pins, contributions, index dependencies and small merge continuations.
The [catalog design](catalog.md) specifies publication, operation identities,
reader/GC synchronization and SQL diagnostics. The [implemented adapter](sqlite-catalog.md)
covers immutable roots, reservations and conditional timeline publication.
Ownership retirement, schema migration and resumable job execution remain
extensions.

Direct network adoption keeps compatible native bytes and their sampled offsets
intact. The arbitrary-prefix recurrence bounds borrowed **entries**, without
requiring monotone native file sizes. The intended three-indexes-per-level cap,
doubled during rebuilding, bounds repeated first-key storage by $O(T\log(N+1))$.
The [network analysis](network-admission.md) also bounds all borrowed literals
by the index count times the native key union's ordinary-FC literal size, and
separates these per-snapshot peaks from cumulative string work and historical
pins. Enforcing occupancy and admission work budgets remains scheduler work.

### Strong deletes and global rebuilding

[Strong deletion by incremental rebuilding](rebuild.md) adapts the
Overmars–van Leeuwen weak-to-clean transformation, §2, Theorem 1. It specifies
early triggers, the construction-plus-replay work inequality, carried replay
debt, overwrite cleanup and durable continuation. The record-count argument
includes full older coverage and a finite admission cut; byte costs require
additional explicit work budgets.

`replacement_rebuild_engine` implements this transformation for one occupied
replacement sort. It funds a physical scan, feeds resolved live rows into a
real candidate runtime, then replays intervening mutations in FIFO order.
The candidate must settle and match the foreground's logical metadata before
handoff. Replay debt remains in the next generation's mutation count. The
[replacement guide](replacement-rebuild.md) derives the structural reservations
and states the separate byte-cost boundary.

The typed connection can persist this executor's clean-base count, mutation
count and active-rebuild marker. If private scan or replay progress is lost,
reopening funds a fresh cleanup of the latest acknowledged state and gates new
admissions until it finishes. Tests cover active saves and forks, loss of a
queued overwrite and deletion, repeated reopen and process interruption.
Private partial candidate output is not resumed. Multiple replacement sorts,
general arrows and byte-bounded rebuilding remain extensions.

### Categorical updates

The [per-key category design](arrows.md) extends the semantics to composable
diffs. It specifies composition, partition independence, exact endpoint deltas,
query costs and dependency retention. `reference_cola` resolves replacements
using optional values, and `profile_blob<P>` carries opaque value payloads.
`typed_engine` executes per-sort arrows and composes them in chronological
order during native merges. A noncommutative append sort exercises that order,
including hash accounting and mapped snapshot restoration. Sort-specific stream
grammars also run directly in the opt-in KV03 runtime family. General arrow
normalization bounds remain a separate concern.

## Build and verification

Configure, build and run the standalone component suites with CMake/CTest:

```sh
cmake -S . -B build -DDIET_BUILD_TESTS=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

For ASan/UBSan on a supported toolchain, use a separate build directory:

```sh
cmake -S . -B build-sanitize -DDIET_BUILD_TESTS=ON -DDIET_SANITIZERS=ON
cmake --build build-sanitize --parallel 4
ctest --test-dir build-sanitize --output-on-failure
```

The default component suites cover codecs, native and borrowed writers, index
construction, queries, cola semantics, ownership, durability and mapped files.
With SQLite enabled, seven more suites cover the catalog, adversarial operations,
forwarded VFS failures, process interruption, timeline publication, streamed
merge publication and COLA graph registration. Three package consumers check relocated core and
SQLite installations and embedded use. Doxygen is an optional additional check.

Verification through `09903de` on 2026-09-15: AppleClang 21, C++20, Release
with strict warnings and ASan/UBSan passed all **67 component/package CTests**,
including Doxygen and the installed named-connection example. The complete run
found one legacy catalog fixture missing its original WAL setting; the corrected
timeline suite passed separately. ThreadSanitizer also passed the tap and named
connection suites. Linux GCC passed the earlier tap, typed update, runtime,
sort-codec and fridge suites with ASan/UBSan.

Earlier combined verification through `4872d25` on 2026-09-15: AppleClang 21, C++20,
Release with strict warnings and ASan/UBSan passed all **51 component/package
CTests**, including three package consumers. The final Doxygen check passed
separately. SQLite headers and runtime were 3.53.4. The RC suite and revised
README snapshot example were rebuilt and passed after the dump-operation rename.
This run covers ordinary-FC comparison, owning and mapped single/two-route
queries, carried sampling/build frontiers, portable unaligned navigation,
native construction, staged local merges and allocation failures, immutable
writes, metadata-only opening, and persistent saves, timelines and reader pins.

The writer tests cover failure at every syscall position, short/interrupted
writes, disk-full errors, uncertain installation, close failures and retained
real outputs. The SQLite VFS suite injects 114 errors before and after reached
write/sync calls, checking old-root retention and complete-or-absent operation
rows through fresh connections. The 20 process-interruption cases additionally
check actual writer death, including committed-but-unacknowledged operations.
These tests do not establish behavior under physical power loss.

Doxygen checked 42 public headers and 41 real declaration associations, with
combined leading file metadata, six rejected metadata fixtures, and clean
generation that removes obsolete pages. Metadata moves preserved code bodies;
the generator reproduced all eight CRC backends. The proof checkpoint checked
858 Lean declarations with only standard `propext`, `Quot.sound` and
`Classical.choice` axioms.

All twelve complete programs from the README and the native-merge, SQLite,
COLA index, mapped save/merge and local-job guides passed strict warnings and
ASan/UBSan. The mapped save example
reopens two saved roots and the current timeline after publication. Installed
licenses and generated CRC includes are checked byte for byte
against the source bundle, and the pinned generator reproduced all eight
backends. Windows execution coverage is limited to the recorded rank component
tests. Network transport and durable merge resumption remain separate work.

Object envelopes use the eight-byte `DIET.KV`/`DIET.IX` signatures with a
terminating zero; debug resolved-table dumps use the same eight-byte
shape, `DIET.RC` plus a terminating zero, with a separate format version.
Independent golden checks cover these bytes, the complete 32-byte empty
reference header and the envelope CRC. Header validation and
`import_rc` reject incompatible signatures even with otherwise valid
fields and checksums.

The optional `DIET_BUILD_DOCS` configuration generates Doxygen HTML/XML and
checks leading file metadata plus representative function/member ownership. A
two-file fixture checks namespaces, same-name classes and overloaded functions;
negative cases reject malformed or misplaced file metadata. The license aliases render
SPDX as a code block and remove the unconfigured unknown-command warnings.
See [the documentation check](doxygen.md) for the exact assertions and limits.

## Next implementation assignments

| Work item | Dependencies | Concrete acceptance |
| --- | --- | --- |
| Streamed active runtime | direct sort-owned runtime, KV03 file writer and durable complete-frontier adapter | runtime-owned file construction context, sealed mapped merge outputs and bounded native-payload memory during ordinary writes |
| General arrow policy coverage | replacement and noncommutative append instances, source validation and per-sort endpoint deltas | additional categories, bounded composition dependencies, observation costs and persisted schema migration |
| Comparison block encoding | ordinary FC, exact cut LCP and scalar comparison transfers | transposed count/literal layouts, ordered SIMD transfer scans, bounded tails and independently measured time/space tradeoffs |
| Object identity and integrity | portable sections, mmap queries and immutable writer | cryptographic content addressing, durable catalog publication and lazy block-integrity strategy |
| Shared completed merges | redundant main/secondary scheduler, paid structural service and durable full-frontier restoration | reuse completed native merges across forks while each dependent rebuilds its own exact fractional indexes |
| Catalog pin retirement | conditional timeline publication, immutable saves, reservations and exact file graph | reader/generation retirement, reclaim only after final pin, schema migration and interruption tests |
| Direct batch adoption | native file reader, prefix index builder and scheduler | preserve received ordinary-FC bytes, bound visible catalogs and work debt, preserve causal order and charge actual key bytes |
| Durable backend and resumable merges | publication protocol and encoded merge continuations | fault injection at write/sync/rename/recovery cuts; failed barriers retain old roots; resume only from verified durable prefixes |
| Durable round resumption | save manifests and update protocol | persist base/round identity, accepted batch identities and claimed keys; restart without double-applying a changeset |
| Generalized live-size rebuilding | single-sort replacement executor and durable recovery gate | multiple replacement sorts, explicit byte budgets and durable partial-progress continuation without erasing replay debt |

Separate files versus extents in managed blobs, transport and client integration
remain policy choices. The library's correctness contracts must remain explicit
when those implementations are selected.

### Optional experiments

Key-only self-cancelling toggles remain an ambitious TODO, not a required second
arrow instance or a completion criterion for the store. Their hash deltas depend
on prior-state polarity, so source-state validation and replay need a convincing
contract before implementation is worthwhile. I may omit them entirely.
