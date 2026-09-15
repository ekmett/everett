# Everett implementation status

Updated 2026-09-15. Specification: [Everett design](design.md).

This ledger records what works, what the tests establish, and what remains to
be built. The C++20 foundations live in `include/everett/`, in
namespace `everett`. The complete disk store, SQLite catalog runtime and
bounded redundant-level scheduler remain implementation work.

## Ownership and acceptance

One integration owner handles the main checkout, build/package integration and
combined verification. Component work happens in isolated worktrees from
a committed revision, preserving unrelated work and leaving reviewed checkpoints
for integration. These are development responsibilities.

| Role | Owned components | Acceptance |
| --- | --- | --- |
| Navigation and recovery | `rank.h`, `rank15.h`, `select15.h`, `durability.h`; `tests/rank.cc`, `tests/durability.cc` | rank and sparse-offset oracles, counter transitions, publication ordering, failure and resumption cases |
| Grouped navigation and object files | `rank_groups.h`, `select_groups.h`, `mapped_file.h`, `file.h`, `object_path.h`; group/mapping/file tests | policy groups, checked binary envelopes, retained mappings and canonical sharded paths |
| Checksums | `crc32c.h`, generated backends, pinned generator and package notices; `tests/crc32c.cc` | independent CRC oracle, bounded loads, reproducible generation, target guards and multi-translation-unit installed consumption |
| Key codecs and blobs | `key_detail.h`, `front.h`, `blob.h`; `tests/front.cc`; [key policies](keys.md) | bounded comparison, partial-prefix lookup, false borrows, independent reindexing, conservative boundary contexts and sort contracts |
| Typed profiles and backing reader | `policy.h`, `profile.h`, `profile_blob.h`, `multiverse.h`; profile/blob/multiverse tests | byte/bit and value-layout matrix, LPFC, modified borrowed FC, same-policy aliases and unchanged native allocation on reindex |
| World semantics and ownership | `fingerprint.h`, `pins.h`, `world.h`; `tests/world.cc`, `tests/pins.cc` | disjoint batch permutations, snapshots, old-value validation, contributions, replay and reference export |
| Design documentation | [design](design.md), [arrows](arrows.md), [rebuilding](rebuild.md), [durability](durability.md), this ledger | consistent contracts, cited derivations, implementation limits and independently usable terminology |

As components change, we update this ledger with the reviewed revision, actual
checks and remaining limits. Host-specific resource coordination stays outside
this package.

## Implemented foundations

### Rank and sparse offsets

- Rank-only directory with 64-bit epoch counts, 32-bit block counts, and three
  independent ten-bit populations. This backend has no select prerequisite.
- The lane sum uses word-parallel arithmetic, widening ten-bit lanes to eleven
  bits before addition. Tests exercise counter transitions at `2^32` and `2^33`
  source bits without allocating those payloads.
- Separate rank15 codec: four bits per class and a 64-bit prefix checkpoint
  every 128 classes. A query accumulates at most eight packed words before one
  horizontal sum. Those are 1920 virtual-entry checkpoints, distinct from the
  512/2048-bit rank layout.
- Elias–Fano over sparse residual offsets, with dense/sparse sampled access
  support. Dense select scans at most 4096 high bits; sparse groups store direct
  exception positions. This is bounded pragmatic support, not a tuned succinct
  select directory.
- Views borrow aligned native-endian spans. They validate shapes, not semantic
  directory contents. Portable serialization, untrusted-data validation and mmap
  ownership remain separate work. A full `2^32`-bit payload is not a test fixture.

`rank_groups<K>` and `select_groups<K>` now support policy-sized groups
`K = 2^r - 1`, including 3, 7, 15 and 31. Packed class widths are respectively
2, 3, 4 and 5 bits. Each checkpoint covers 128 classes. Groups of three sum
two-bit fields with scalar packed arithmetic. Groups of seven and thirty-one
use weighted bit-plane populations, with bounded NEON reductions on
little-endian AArch64 and portable word reductions elsewhere. Short final
checkpoints use only their readable words. Other group sizes retain the generic
loop over at most 127 classes. The `K=15` view shares the packed rank15 implementation.
The helpers in `rank15.h` and `select15.h` fix their interval to fifteen; the
policy-backed blob uses `rank_groups<P::group_size>` and
`select_groups<P::group_size>`.
Offsets and fixed strides share the caller's byte/bit unit. Shape validation
is not a substitute for complete validation of a serialized rank/select section.
The [sampling analysis](sampling.md) distinguishes local correctness from
per-level capacity and whole-chain storage bounds.

On little-endian AArch64, rank15 loads a complete checkpoint with four NEON
vectors, masks the low and high nybbles at the requested boundary, and combines
the byte lanes before one widening horizontal reduction. AVX2 uses two 32-byte
loads; AVX-512F plus AVX-512BW uses one 64-byte load. Both x86 paths compare the
even and odd nybble positions with the boundary and retain one pair sum per byte.
The word shift used to extract high nybbles is masked again to remove bits from
the neighboring byte.

Each byte holds a sum of at most 30. Reducing the eight 64-bit lanes first
would leave eight byte sums of at most 240, without carries between them. We
could then finish in a scalar register with adjacent-byte sums and a multiply.
I measured that variant against widening with `VPSADBW` before the final lane
reduction. The latter avoids the dependent scalar fold and had lower independent
rank and pair medians on both tested x86 processors, so it is the selected
implementation. Dependent queries do not establish a universal winner. A complete
checkpoint sums to at most 1920; the largest queried prefix has 127 classes and
sums to 1905. The implementation selects an ISA from the compiler target; it adds
neither runtime dispatch nor exported ISA flags.

A bounded scalar path handles short final checkpoints and other targets. All
vector paths check for 64 readable bytes before loading. The view caches its group
count, adding eight bytes to the view without changing the stored classes or
checkpoints. Both blob APIs project a window with one rank query and the group's
population: the upper rank is the lower rank plus that population.

The query checks cover maximum populations, every prefix cut, isolated nybbles
and their complements, short final checkpoints, random packed words and every
eight-byte alignment within a cache line. POSIX fixtures put complete and short
checkpoints immediately before an inaccessible page, including nonzero padding.
Typed `K=15` views retain the same encoded layout and use the same query
implementation.

I compared the packed queries with the full bitvector directory and a CPU/NEON
translation of [my Poppy shader](https://github.com/ekmett/vr/blob/master/shaders/poppy.glsl).
That translation keeps the 2048/512-bit directory and loads all four vectors of
the selected run, masks at the query position, and reduces their populations.
That comparison predates the bounded NEON path now used by `rank_view`.
Every variant sees the same bitmap and queries at the same fifteen-entry cuts.

For a 253,440-bit universe on an M2 Max, AppleClang 21 `-O3 -DNDEBUG`, the
five-trial medians were:

| Rank implementation | Data plus directory | Independent random rank | Dependent random rank |
| --- | ---: | ---: | ---: |
| Full bitvector, scalar `rank_view` | 32,680 B | 14.86 ns | 23.26 ns |
| Full bitvector, 512-bit NEON translation | 32,680 B | 4.89 ns | 18.77 ns |
| Previous packed rank15 | 9,504 B | 15.78 ns | 19.96 ns |
| Previous generic `rank_groups<15>` | 9,504 B | 43.78 ns | 45.53 ns |
| Packed rank15, NEON | 9,504 B | 5.38 ns | 17.12 ns |

The packed representation uses about 29% of the full representation's space
including their directories. For the adjacent endpoint pair used by window
projection, two queries through the previous generic path took 78.67 ns; one
SIMD rank plus the class took 6.56 ns. The full-bitvector NEON translation's
corresponding rank-plus-fifteen-bit-popcount pair took 7.34 ns. These are rank
primitive timings, including a common indirect call, not complete string lookups.
The dependent stream derives its next position from the preceding answer and
includes that address-generation cost.

Both data arrays were 64-byte aligned in this comparison. Queries were generated
before timing, independently checked, and shared across variants; allocation,
construction and the oracle are excluded. Larger cases include about 96 MiB of
packed storage versus 330 MiB of full storage for the same universe. Their
trial ranges overlap substantially, especially for dependent queries, so these
measurements do not establish a large-working-set winner. The reproducible
benchmark ([method](../bench/rank_compare.md), [runner](../bench/rank_compare.sh), [source](../bench/rank_compare.cc),
[measurements](../bench/results/rank_compare_m2max.csv)) retains sizes,
alignments, checksums and minimum/median/maximum times.
These are resident-memory measurements on one processor, excluding page faults
and file I/O.

The SIMD and typed-view changes were reviewed at `d55fefba` and `b06798d`, and
the comparison at `ef26e25`, in isolated component work. Combined ASan/UBSan
verification passed all 18 CTests, including the blob projections, package
consumers and Doxygen. Component checks also exercised the forced-portable rank
and grouped-rank paths. That checkpoint introduced the packed `K=15` SIMD path;
the other rank paths are measured separately below.

The native x86 comparison uses a newer scalar baseline and the same packed
input for both reductions. These are complete public rank calls, including
checkpoint lookup and a common indirect call. The packed arrays occupy 32,760 B;
the query array occupies 256 KiB. Entries are median nanoseconds per operation.

| Processor / compiler target | Scalar rank | SAD rank | Qword-first rank | SAD rank + class | Qword-first rank + class |
| --- | ---: | ---: | ---: | ---: | ---: |
| Core i9-12900K / Clang 20 AVX2 | 9.212 | 2.896 | 3.235 | 3.234 | 3.694 |
| Ryzen 9 7950X3D / MSVC 19.44 AVX2 | 8.961 | 4.289 | 4.939 | 5.272 | 5.809 |
| Ryzen 9 7950X3D / MSVC 19.44 AVX512 | 8.850 | 3.673 | 4.344 | 4.709 | 4.928 |

The [Linux report](../bench/rank_compare_quartus.md) includes a larger case
where the scalar dependent query beats both SIMD variants. The
[Windows report](../bench/rank_compare_windows.md) retains a dependent AVX512
case favoring qword-first and substantial scheduling outliers. Both reports
pin their source snapshots, record CPU affinity and usable ISA features, and
provide the complete CSVs and reconstruction instructions. They exclude view
construction, page faults and file I/O; these are not complete catalog searches.

I also compared the NEON reductions against the actual Cult CPU rank header,
using an external include. That directory stores twelve bytes per 512 source
bits, or 18.75% metadata. It is distinct from the earlier 2048/512 Poppy
translation. For the same 253,440-bit universe, the new M2 Max comparison gives:

| Implementation | Encoded arrays + endpoint count | Independent rank | Rank + class | Dependent rank |
| --- | ---: | ---: | ---: | ---: |
| Current NEON | 9,512 B | 6.802 ns | 8.030 ns | 17.350 ns |
| Qword-first NEON | 9,512 B | 5.731 ns | 9.583 ns | 20.192 ns |
| Cult bitmap rank | 37,624 B | 7.368 ns | 10.611 ns | 18.612 ns |

I kept the existing NEON widening reduction: qword-first improves independent
single-rank throughput, but loses the paired and dependent hot queries. The
larger comparison has mixed results too: Cult leads the independent pair while
packed rank has smaller dependent medians. The
[NEON/Cult report](../bench/neon_cult_rank.md) records exact storage, alignment,
source hashes, timing ranges and reproduction commands. Its sizes include the
terminal count, which the earlier Poppy comparison table excludes. The Cult
header stays external and unchanged.

The x86 kernels and guarded-tail tests were independently reviewed; the selected
SAD kernels at `d7cbb09` match the native-tested bodies. Both public rank and
grouped-rank suites passed for SAD and qword-first on Windows under AVX2 and
AVX512. The selected header also passed both suites under Rosetta AVX2 in
Release and ASan/UBSan, including the guarded-page fixtures. The NEON/Cult
benchmark passed independent oracles and hot ASan/UBSan checks and was reviewed
separately before integration at `1b7e25c`. Combined ASan/UBSan verification
passed all 18 CTests, including installed and embedded package consumers,
policy-group tests, blob projections and Doxygen.

Rank construction now separates complete 2048-bit blocks from the bounded tail.
A full block uses four 512-bit popcounts; AArch64 NEON sums four byte-popcount
vectors with a widened final reduction, and other targets use eight portable
word popcounts per run. Stored counts and directory layout are unchanged.
The tests cover all 513 populations, word-aligned SIMD offsets and every
partial-block length. On an M2 Max with AppleClang 21 `-O3`, owning builds over
32 KiB, 8 MiB and 64 MiB inputs measured about 1.7–2.0 times faster than the
previous builder. These are medians of seven warm-input rounds, including
allocation and copying but excluding I/O; they are not cross-platform results.

Elias–Fano construction packs low fields in width-specialized tiles of
64/gcd(width,64) values and assigns each high word once. All encoded words,
select samples and sparse exceptions remain identical. M2 Max / AppleClang 21
`-O3 -DNDEBUG` measurements against the earlier writer show 2.01–3.03 times
faster complete construction; 64 MiB source arrays at widths 8 and 12 retain
2.35 and 2.01 times improvement. The all-width specializations add about 49 KiB
of code in a minimal consumer and increased its median compile/link time from
0.50 to 1.57 seconds. Those measurements cover the portable tiled writer.

The current sparse-offset builders share that writer, including `select15`.
On little-endian AArch64, complete width-eight and width-sixteen tiles use NEON
narrowing; the remaining widths keep the constant-shift implementation. Both
builders use bounded NEON adjacent comparisons to validate monotone input on
AArch64. High-word accumulation and the encoded low/high arrays, samples and
exceptions are unchanged.

Select still scans scalar words within its bounded dense group. After finding
the word, a broadword byte-prefix calculation locates the selected bit without
clearing each preceding one. An x86-64 compiler target with BMI2 uses `PDEP`
instead. The measured four-word SIMD scan lost to the scalar scan, and
width-thirty-two narrowing lost to the existing tile. I kept those candidates
in the [select comparison](../bench/select_compare.md), with the measured
construction and query results. Sharing the writer also means a `select15`-only
consumer instantiates the existing width dispatcher.

The full bitmap `rank_view` now masks and popcounts a complete readable 512-bit
run with NEON on AArch64; bounded portable work handles short tails and other
targets. Queries at 512-bit boundaries return the directory count without
touching the bitmap. Guarded-page tests cover both short tails and a completely
inaccessible bitmap at directory-only boundaries.

The [other-rank comparison](../bench/other_rank.md) records independent rank,
dependent rank and rank-plus-class projection for groups 3, 7 and 31, alongside
the full bitmap query. I selected the scalar two-bit sum for `K=3` because it
has lower dependent latency, although NEON has higher independent throughput.
The wider classes favor NEON for hot projection throughput; `K=31` retains a
small dependent-latency tradeoff. Larger inputs give mixed results, so these
measurements do not establish a universal winner outside the measured workloads.

The navigation changes were reviewed and integrated at `bc24f44` (select) and
`5817e02` (grouped and bitmap rank). Their component checks include independent
population/offset oracles, forced-portable paths, protected pages, Rosetta AVX2
with and without BMI2 where applicable, and package consumption. The selected
native AArch64 timings and translated x86 correctness checks are distinguished
in the linked reports. These changes preserve the stored layouts and add no
runtime dispatch or exported ISA flags.

### Front-coded blobs

- Encoded byte-string arrays with configurable native LPFC, partial-key decoding,
  sparse residual offsets, and fixed nine-byte optional-u64 slots.
- Separate native and borrowed streams, two select15 indexes, one rank15, and
  packed false-borrow flags, including equality spanning multiple group cuts.
- `reindex` shares the exact immutable native allocation and repairs only the
  borrowed/index data. The builder currently materializes native keys as scratch.
- A caller-supplied virtual window projects to at most fifteen records. This is
  a local lookup primitive, not a complete root-to-leaf catalog-search executor.
- A two-sided borrowed-prefix policy reconstructs outgoing context with one
  additional predecessor probe. It emits at most twice ordinary borrowed FC's
  suffix bytes; framing and access metadata are separate costs.
- The default policy constrains prefixes at actual shared fifteen-entry cuts.
  Records unaffected by a cut keep ordinary front coding. Tests cover moving
  cuts on reindex, multiple applicable cuts and suffix-byte ordering against
  the two-sided reference.
- A three-level fixture checks 920 queries, including replacement precedence
  and tombstones. All three borrowed-prefix policies have cascade-oracle tests;
  ordinary mode uses an explicit slow fallback when predecessor context is
  missing. Native LPFC remains independent of changing index cuts.

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

`storage_policy<Unit, Values, GroupSize, BackspaceCode>` carries the unit,
fixed/variable value layout, sampling group size and bit-backspace code through
the codec types. `fixed_values<N>` counts
policy units, including the valid width zero. `profile_array<P, Role>` uses
canonical byte varints and policy-selected Golomb or exponential-Golomb bit
backspaces. Other bit counts use order-zero exponential-Golomb. Streams retain
meaningful bit extents and canonical padding. One predecessor-length
checkpoint per physical group supports surrogate-anchor decoding. Common fixed
value width is subtracted from the Elias–Fano residual positions.

`profile_blob<P>` supplies native LPFC with default factor 18 and separate
borrowed front coding constrained at shared group boundaries. It retains two
sampled offset structures, one grouped origin rank and false-borrow flags.
Reindexing shares the exact native allocation. Search returns both a native
value and downstream routing on equality; it does not resolve same-key arrows.
The builder still materializes decoded native keys as scratch during reindex.
`profile_cursor<P, Role>` supplies sequential decoding with borrowed values;
`profile_borrowed_writer<P>` incrementally encodes borrowed keys. Their output
matches the batch encoder across the full policy matrix. `sample_cursor<P>`
pins and samples an exact encoded pair without materializing its catalog.
`index_builder<P>` retains one incoming/outgoing sample, delays the preceding
borrowed record until its shared-cut ceiling is known, and preserves native
allocations. `index_pipeline<P>` feeds samples directly between those stages,
supports bounded cursor-event stepping and returns a chain with exact target
pins. Inter-stage samples carry a policy-unit backspace and suffix; the first
sample is literal and later samples refer to that producer's preceding sample.
Final metadata construction is a separate linear phase. See the
[construction implementation](sampling.md#streaming-construction-pipeline).

Codec fixtures cover 24 combinations: byte/bit × variable/fixed3/fixed0 values
× groups 3/7/15/31. Blob fixtures cover 16 byte/bit × fixed/variable × group
combinations, including repeated equal borrows, partial contexts, cascades and
changed index boundaries. Selected Golomb moduli and exponential-Golomb orders
also exercise native LPFC, borrowed writers and index pipelines. Count-code
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
checking is not implemented. The body is presently opaque: portable
rank/select/profile section serialization remains work.
`encode_file` is pure serialization, not a durable object writer.

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

`multiverse<P>` is a working read-side object-directory owner. It exposes
`sort = everett::sort<P>`, `blob`, `file` and forward-declared `world`, `timeline`
and `branch_point` types. `sort<P>` validates one code's packing and unit
alignment; it does not establish prefix freedom of an entire registry. Mapped
files and slices outlive the reader object. There is no SQLite connection or
persistent aggregate implementation behind these forward declarations.

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
replaces the entire set. IDs are process-local and lifetime tests use shared
pointers. Durable object IDs and disk reclamation remain implementation work.

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
Keys in this proof layer are natural numbers representing order; it does not
decode string keys or the catalog's physical target graph. Compressed rank,
Elias–Fano, front coding, full cascade execution, C++ refinement, scheduling
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
reader/GC synchronization and SQL diagnostics. No SQLite schema migration or
C++ catalog runtime is implemented yet.

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
query costs and dependency retention. `reference_world` still
resolves replacements, and `blob` still uses its fixed value/tombstone payload.
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

The fifteen component suites are `rank`, `groups`, `front`, `profile`,
`profile_blob`, `sampling`, `index_builder`, `index_pipeline`, `world`, `pins`,
`durability`, `mapped_file`, `files`, `multiverse` and `crc32c`. Two additional CTests
validate relocated installation and embedded CMake consumption, including
typed headers and CRC calls across translation units. We record combined
verification here after these commands run.

Combined verification on 2026-09-15: AppleClang 21, C++20, Release with strict
warnings and ASan/UBSan passed all **18 CTests**, including both package consumers
and the optional Doxygen check, with the key/bit, grouped/bitmap rank and
Elias–Fano changes integrated. The tested public headers match `d027162`.
The documentation includes the benchmark method and bundles its runner, source
and measurements. Installed licenses and generated CRC includes were checked
byte for byte against the source bundle; regenerating from the pinned generator
also reproduced all eight backends.
All five complete README examples also compiled and ran with strict warnings
and ASan/UBSan. Local Markdown links were checked, including heading anchors.
Windows execution coverage is limited to the recorded rank component tests.
A persistent SQLite backend, network transport, filesystem writer fault injection
and physical power loss remain outside these checks.

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
| Complete multi-catalog query | front/rank primitives | oracle-equivalent root-to-leaf queries; both frontier contexts; equality at cuts; recorded bounds on entries and bytes visited |
| Conservative fractional-index codec tuning | native LPFC and tested shared-cut policy | streaming reindex against changed downstream layout without changing native bytes; measured replayed-prefix bytes; empty projected streams and scratch-space costs |
| Portable blob sections and writer | checked envelope, typed codecs and retained mappings | serialize/validate codec sections, content addressing and durable publication, lazy block-integrity strategy; no full offset per key |
| Attach encoded runs to world semantics | blob reader and query | batch/snapshot/export oracle tests using actual encoded immutable runs |
| COLA scheduler and incremental string merge | correct run merge and index builder | byte/work-budgeted continuations, bounded active levels and shared-result adoption under interleaved forks |
| SQLite catalog and persistent pins | object store and scheduler publication | reopen saves without re-encoding contents; retain exact dependency closure; query metadata with existing SQL tools; reclaim only after final pin; interruption tests |
| Direct batch adoption | native file reader, prefix index builder and scheduler | preserve received LPFC bytes, bound visible catalogs and work debt, preserve causal order and charge actual key bytes |
| Durable backend and resumable merges | publication protocol and encoded merge continuations | fault injection at write/sync/rename/recovery cuts; failed barriers retain old roots; resume only from verified durable prefixes |
| Durable round resumption | save manifests and update protocol | persist base/round identity, accepted batch identities and claimed keys; restart without double-applying a changeset |
| Live-size rebuilding | scheduler and mutation accounting | replacement ready before half the clean base disappears; repeated overwrites do not grow history-sized active levels; preserve replay debt and explicit byte budgets |

Separate files versus extents in managed blobs, transport and client integration
remain policy choices. The library's correctness contracts must remain explicit
when those implementations are selected.
