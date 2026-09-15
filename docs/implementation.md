# Everett implementation status

Updated 2026-09-15. Specification: [Everett design](design.md).

Everett currently provides independently testable C++20 foundations for the
intended store. Public headers live in `include/everett/`, in namespace
`everett`. The production disk store, bounded redundant-level scheduler and
persistent object manifests remain implementation work.

## Ownership and acceptance

The integration owner maintains the main checkout, build/package integration
and combined verification. Component workers use isolated worktrees from a
committed revision, preserve unrelated work, and leave reviewed checkpoints for
integration. These are implementation roles, not runtime components.

| Role | Owned components | Acceptance |
| --- | --- | --- |
| Navigation and recovery | `rank.h`, `rank15.h`, `select15.h`, `durability.h`; `tests/rank.cc`, `tests/durability.cc` | rank and sparse-offset oracles, counter transitions, publication ordering, failure and resumption cases |
| Key codecs and blobs | `front.h`, `blob.h`; `tests/front.cc`; [key policies](keys.md) | partial-prefix lookup, false borrows, independent reindexing, conservative boundary contexts and sort contracts |
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

These are owned encoded streams and borrowed views, not a finalized on-disk ABI.
The tested keys use unsigned-byte lexical order. Bit keys, 16-bit-word keys,
sort-qualified framing and per-sort policy dispatch are specified in
[keys.md](keys.md), not implemented by this codec.

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

The intended public aggregates are `multiverse` for backing storage, `world` for
a logical state, `timeline` for an ordered progression, and `branch_point` for a
retained point. These are design names; no such aggregate disk API is implemented.
`reference_world` remains the concrete prototype.

[Sorts and key policies](keys.md) describes sort-qualified keys, key units,
prefix-free coding and hash selection. The category may depend on the full key,
even when a sort supplies default policies. Neither the existing byte codec nor
the homogeneous replacement API implements heterogeneous sorts yet.

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

The suites are `rank`, `front`, `world`, `pins` and `durability`. Standalone
extraction verification is recorded by the integration owner after these
commands run. Also validate installation and consumption from a separate CMake
project; successful in-tree tests alone do not validate the installed package.

## Next implementation assignments

| Work item | Dependencies | Concrete acceptance |
| --- | --- | --- |
| Sort policy interface | canonical key contracts and byte-codec baseline | bits/bytes/16-bit-word keys, framing and order, cross-sort boundaries, domain-separated hashes and stable policy versions |
| Per-key arrow policy and second instance | categorical specification and replacement oracle | noncommuting diffs, heterogeneous keys, source validation, associative semantic composition, disjoint permutations, endpoint deltas, checkpoint observations and explicit work/dependency accounting |
| Complete multi-catalog query | front/rank primitives | oracle-equivalent root-to-leaf queries; both frontier contexts; equality at cuts; recorded bounds on entries and bytes visited |
| Conservative fractional-index codec tuning | native LPFC and tested shared-cut policy | streaming reindex against changed downstream layout without changing native bytes; measured replayed-prefix bytes; empty projected streams and scratch-space costs |
| Portable blob reader/writer | agreed codec/context layout | versioned sections, mapped borrowed views, round trips and malformed/tail checks; no full offset per key |
| Attach encoded runs to world semantics | blob reader and query | batch/snapshot/export oracle tests using actual encoded immutable runs |
| COLA scheduler and incremental string merge | correct run merge and index builder | byte/work-budgeted continuations, bounded active levels and shared-result adoption under interleaved forks |
| Persistent pins and save manifests | object store and scheduler publication | reopen saves without re-encoding contents; retain exact dependency closure; reclaim only after final pin; interruption tests |
| Durable backend and resumable merges | publication protocol and encoded merge continuations | fault injection at write/sync/rename/recovery cuts; failed barriers retain old roots; resume only from verified durable prefixes |
| Durable round resumption | save manifests and update protocol | persist base/round identity, accepted batch identities and claimed keys; restart without double-applying a changeset |
| Live-size rebuilding | scheduler and mutation accounting | replacement ready before half the clean base disappears; repeated overwrites do not grow history-sized active levels; preserve replay debt and explicit byte budgets |

Separate files versus extents in managed blobs, transport and client integration
remain policy choices. The library's correctness contracts must remain explicit
when those implementations are selected.
