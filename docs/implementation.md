# Everett implementation status

Updated 2026-09-15. Specification: [Everett design](design.md).

Everett currently provides independently testable C++20 foundations for the
intended store. Public headers live in `include/everett/`, in namespace
`everett`. The complete disk store, SQLite catalog runtime and bounded
redundant-level scheduler remain implementation work.

## Ownership and acceptance

The integration owner maintains the main checkout, build/package integration
and combined verification. Component workers use isolated worktrees from a
committed revision, preserve unrelated work, and leave reviewed checkpoints for
integration. These are implementation roles, not runtime components.

| Role | Owned components | Acceptance |
| --- | --- | --- |
| Navigation and recovery | `rank.h`, `rank15.h`, `select15.h`, `durability.h`; `tests/rank.cc`, `tests/durability.cc` | rank and sparse-offset oracles, counter transitions, publication ordering, failure and resumption cases |
| Grouped navigation and object files | `rank_groups.h`, `select_groups.h`, `mapped_file.h`, `file.h`, `object_path.h`; group/mapping/file tests | policy groups, checked binary envelopes, retained mappings and canonical sharded paths |
| Key codecs and blobs | `front.h`, `blob.h`; `tests/front.cc`; [key policies](keys.md) | partial-prefix lookup, false borrows, independent reindexing, conservative boundary contexts and sort contracts |
| Typed profiles and backing reader | `policy.h`, `profile.h`, `profile_blob.h`, `multiverse.h`; profile/blob/multiverse tests | byte/bit and value-layout matrix, LPFC, modified borrowed FC, same-policy aliases and unchanged native allocation on reindex |
| World semantics and ownership | `fingerprint.h`, `pins.h`, `world.h`; `tests/world.cc`, `tests/pins.cc` | disjoint batch permutations, snapshots, old-value validation, contributions, replay and reference export |
| Design documentation | [design](design.md), [arrows](arrows.md), [rebuilding](rebuild.md), [durability](durability.md), this ledger | consistent contracts, cited derivations, implementation limits and independently usable terminology |

Future component changes should update this ledger with their reviewed revision,
actual checks and remaining limits. Host-specific resource coordination belongs
outside this package.

## Implemented foundations

### Rank and sparse offsets

- Rank-only directory with 64-bit epoch counts, 32-bit block counts, and three
  independent ten-bit populations. This backend has no select prerequisite.
- The lane sum uses word-parallel arithmetic, widening ten-bit lanes to eleven
  bits before addition. Tests exercise counter transitions at `2^32` and `2^33`
  source bits without allocating those payloads.
- Separate rank15 codec: four bits per class, a 64-bit prefix checkpoint every
  128 classes, and at most eight packed-word sums per query. Those are 1920
  virtual-entry checkpoints, distinct from the 512/2048-bit rank layout.
- Elias–Fano over sparse residual offsets, with dense/sparse sampled access
  support. Dense select scans at most 4096 high bits; sparse groups store direct
  exception positions. This is bounded pragmatic support, not a tuned succinct
  select directory.
- Views borrow aligned native-endian spans. They validate shapes, not semantic
  directory contents. Portable serialization, untrusted-data validation and mmap
  ownership remain separate work. A full `2^32`-bit payload is not a test fixture.

`rank_groups<K>` and `select_groups<K>` now support policy-sized groups
`K = 2^r - 1`, including 3, 7, 15 and 31. Packed class widths are respectively
2, 3, 4 and 5 bits. The generic rank directory reads at most 127 classes after
a checkpoint; it does not yet use the fixed-15 packed-word summation fast path.
Offsets and fixed strides share the caller's byte/bit unit. Shape validation
is not a substitute for complete validation of a serialized rank/select section.
The [sampling analysis](sampling.md) distinguishes local correctness from
per-level capacity and whole-chain storage bounds.

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

Those are the original byte-only `front.h`/`blob.h` primitives, retained as an
independent baseline. They are owned encoded streams and borrowed views, not a
finalized on-disk ABI.

### Typed byte and bit profiles

`storage_policy<Unit, Values, GroupSize>` carries the unit, fixed/variable value
layout and sampling group size through the codec types. `fixed_values<N>` counts
policy units, including the valid width zero. `profile_array<P, Role>` uses
canonical byte varints or bit-level exponential-Golomb0 counts, actual backspace
counts, meaningful bit extents and canonical padding. One predecessor-length
checkpoint per physical group supports surrogate-anchor decoding. Common fixed
value width is subtracted from the Elias–Fano residual positions.

`profile_blob<P>` supplies native LPFC with default factor 18 and separate
borrowed front coding constrained at shared group boundaries. It retains two
sampled offset structures, one grouped origin rank and false-borrow flags.
Reindexing shares the exact native allocation. Search returns both a native
value and downstream routing on equality; it does not resolve same-key arrows.
The builder still materializes decoded native keys as scratch during reindex.

Codec fixtures cover 24 combinations: byte/bit × variable/fixed3/fixed0 values
× groups 3/7/15/31. Blob fixtures cover 16 byte/bit × fixed/variable × group
combinations, including repeated equal borrows, partial contexts, cascades and
changed index boundaries. These test record/prefix behavior, not the full
string-store I/O theorem. Sixteen-bit units, sort-qualified framing, a prefix-free
registry and per-sort hash/category dispatch remain extensions.

### Object envelopes, mappings and type family

`mapped_file` opens regular files read-only and returns lifetime-owning bounded
slices. POSIX mapping and unlink-while-pinned behavior are tested. A native
Windows branch exists but has not been validated on Windows in this checkpoint.

`file<P>` validates a 96-byte little-endian envelope: kind magic, version,
policy metadata, exact lengths, canonical padding, and header/body CRC32C.
Only `.kv` and `.index` kinds exist. Validation scans the whole body on open;
lazy block-level integrity checking is not implemented. The body is presently
opaque: portable rank/select/profile section serialization remains work.
`encode_file` is pure serialization, not a durable object writer.

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

The world layer is a semantic oracle for attaching encoded blobs. Its eager
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

## Specified extensions

### Aggregate API and key policies

The read-side `multiverse<P>` and associated type family are implemented as
described above. Persistent `world<P>`, `timeline<P>` and `branch_point<P>`
runtimes remain to be attached to the selected SQLite catalog.
`reference_world` remains the semantic prototype.

[Sorts and key policies](keys.md) describes sort-qualified keys, key units,
prefix-free coding and hash selection. The category may depend on the full key,
even when a sort supplies default policies. The typed codecs and homogeneous
replacement oracle do not yet implement a heterogeneous sort registry.

### SQLite catalog and network admission

SQLite is the selected backend for logical worlds, immutable representations,
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

The [per-key category design](arrows.md) extends the intended semantics to
composable diffs. It specifies composition, partition independence, exact
endpoint deltas, query costs and dependency retention. `reference_world` still
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

The eleven component suites are `rank`, `groups`, `front`, `profile`,
`profile_blob`, `world`, `pins`, `durability`, `mapped_file`, `files` and
`multiverse`. Two additional CTests validate relocated installation and embedded
CMake consumption, including typed headers. Combined verification is recorded
by the integration owner after these commands run.

Combined verification on 2026-09-15: AppleClang 21, C++20, Release with strict
warnings and ASan/UBSan passed **14/14 CTests**, including both package consumers
and the optional Doxygen check. Installed license notices were checked byte for
byte against the source bundle.
The README reference example also compiled and ran with the same warnings and
sanitizers. The initial build could not write the host's default ccache directory;
using a cache inside the build tree resolved that environmental failure.
No Windows execution, persistent SQLite backend, network transport, filesystem
writer fault injection or physical power-loss test is claimed.

The optional `EVERETT_BUILD_DOCS` configuration generates Doxygen HTML/XML and
checks all file footers plus representative function/member ownership. A
two-file fixture compares top, bottom and split file documentation across namespaces,
same-name classes and overloaded functions. The original `ein` aliases render
SPDX as a code block and remove the unconfigured unknown-command warnings.
See [the documentation check](doxygen.md) for the exact assertions and limits.

## Next implementation assignments

| Work item | Dependencies | Concrete acceptance |
| --- | --- | --- |
| Sort registry and remaining units | typed byte/bit policies and canonical key contracts | prefix-free framing and order, cross-sort boundaries, domain-separated hashes, stable policy versions and eventual 16-bit-word codec |
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
