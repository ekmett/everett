Adaptive native and fractional-index output
==========================================

I give the redundant scheduler an execution-owned storage context that keeps
small completed merges and fractional indexes in memory, and streams larger
outputs into `.kv` and `.index` files. A short-lived intermediate result can
therefore disappear without ever acquiring a file. Publication seals the
completed objects still needed by the exact frontier.

The context owns its SQLite connection, concrete file operations and shared
retained-output allowance. It survives moves and rebases of the active engine;
immutable snapshots retain their inputs directly. Each unfinished job also pins
its context until its writer and scratch resources have been destroyed, even
if the last external storage handle is released.

```cpp
#include <diet/sort_runtime_context.h>
#include <diet/connection.h>

using policy = diet::string_policy;
using family = diet::streaming_sort_runtime_family<policy>;
using engine = diet::typed_engine<policy, diet::wrapping_fingerprint_algebra,
                                  256, family>;

auto live = diet::connect<engine>(existing_directory, "settings");
live.put("config/theme", "dark");
auto before = live.snapshot();
live.put("config/theme", "light");
assert(before.get("config/theme") == "dark");
```

The named connection opens the context after opening or creating the catalog.
The ordinary `sort_runtime_family` keeps owned outputs and remains available.
Both choices write the same KV03 records and use the same application schema;
changing the output sink is not a key-format migration.

Reservations, seals and publication
-----------------------------------

A native merge starts with the same 64 KiB payload buffer as the file writer.
Its first physical flush reserves an object ID and attempt ID before creating
the file, then writes the already encoded prefix. The encoder continues from
that position; it does not repeat comparisons or composition. A conservative
metadata bound can choose streaming at the outset. Finalization also spills
when the completed output exceeds its per-object ceiling or the shared
allowance is exhausted.

An output that fits remains an immutable owned profile. Otherwise the writer
seals its envelope and performs its file and directory barriers. Only after
`record_sealed` acknowledges its transaction does the context construct a mapped
native owner carrying an immutable `native_seal` descriptor.

Fractional indexes make the same choice using their main and secondary payload
buffers. Their jobs retain exact source owners from construction, but defer
resolving file bindings and reserving an index attempt until the first spill.
An owned dependency is sealed then; an already bound suffix ends that walk.
A small completed index needs neither a destination file nor a secondary spool.

For a fractional index, `seal_pair` acknowledges the completed index and its
exact pair registration in one catalog transaction. It checks the receipt's
path, attempt, extent, checksum field and barrier, and checks the native and
secondary envelopes against their sealed rows. Its prepared mapping stays
pinned through the transaction and becomes the returned index owner. The
registered main suffix is checked by its immutable row, without reopening its
files. Exact operation replay revalidates these envelopes but does not repeat
row changes. A failed or uncertain commit installs no owner binding.

The descriptor records the catalog identity and exact seal receipt. Its private
factory opens the expected object path itself. A caller cannot give an arbitrary
mapping a trusted receipt. When the persistence adapter encounters this owner,
it verifies the descriptor against its own catalog and the file envelope, then
reuses the already sealed native file. It does not rewrite its payload or scan
its checksum on ordinary admission.

Sealing an object does not publish a logical update. The persistence adapter
first seals any owned objects reachable from the complete frontier, including
hidden completed outputs. It reuses acknowledged bindings and omits discarded owned
intermediates. Only then do the named root and checkpoint advance in the
publication transaction. The returned durable snapshot retains mapped owners. If a file
operation or catalog transaction has an uncertain outcome, the runtime and its
context become unusable. No receipt is fabricated by reading back cached bytes.
The previous acknowledged publication remains the recovery point; reservations
and surviving output names remain available to recovery. Automatic reclamation
of abandoned attempts belongs to catalog lifecycle management.

The context has its own catalog connection, not a pointer into a movable store.
One serialized worker operates it. There are no context locks or type-erased
callbacks in the record loop. The first spill can perform reservation and
dependency-sealing operations inside an append; later appends reuse that attempt.
Shared dependency owners retain the exact inputs while a merge is suspended.

Concrete storage hooks
----------------------

`redundant_runtime<P, Compose, Storage>` owns a concrete `Storage` value. In
addition to the native/profile traits, a storage family supplies:

- `make_merge<Compose>(older, newer, compose)` to start an incremental merger.
- An optional `reuse_merge<Compose>(older, newer)` to acquire an already completed
  native before starting its replacement merger.
- `finish_merge(merge)` to produce its immutable native owner.
- `make_index<Node>(native, main, secondary)` and `finish_index<Node>(index)`
  to build an index over exact pinned dependencies.
- An optional `poison() noexcept` hook for failures during incremental work.

`sort_file_runtime_storage` holds a shared lifetime handle to
`sort_runtime_context`. Empty and singleton admissions are small owned profiles;
merge and index outputs can be owned or mapped. The context shares one empty
native owner across rebases. After spilling, an index job retains its acknowledged
native/main/secondary bindings until completion, so finishing it does not repeat
their catalog and header checks. Catalog admission still validates the completed
index against the exact native and target metadata. A default-constructed storage
can make an empty seed snapshot, but it needs `open(root)` before starting a
merge or index.

For a direct engine, I can attach an opened storage with
`engine::from_snapshot(snapshot, family::storage_type::open(root))`.
`engine.rebase(equivalent_snapshot)` preserves this storage handle while replacing
a settled layout. Its metadata and admission-count checks are consistency
checks: the caller remains responsible for supplying an equivalent layout.
The named connection uses this operation after persisting a settled result.

The named connection also passes its application schema into the storage
context. For the built-in string replacement sort, this enables
[completed native merge reuse](native-merge-reuse.md) across forks with identical
ordered input files. Each fork still builds its own fractional indexes. A
direct engine can opt in with `family::open_storage(root, schema)`; an empty
storage schema disables reuse. Naturally retained small outputs keep their
existing no-I/O path. Durable hints and acquisition pins currently remain in
the append-only catalog.

Memory and work
---------------

`runtime_output_options` defaults to an 8 MiB shared retained-output allowance
and a 128 KiB ceiling for each completed output. Native profiles and fractional
indexes draw from the same allowance. Either limit set to zero forces eager
streaming:

```cpp
auto storage = family::storage_type::open(existing_directory,
  {}, {}, {}, {}, diet::runtime_output_options{0, 0});
```

I charge the completed object, its lifetime lease and actual vector capacities:
encoded payload, Elias–Fano directories, selectors and dictionaries for natives;
both borrowed streams, rank directories, false-borrow flags and cuts for indexes.
The charge follows the immutable owner and returns only after its last reference
is destroyed, even on another thread. These owners keep the allowance alive
without retaining the execution context. `retained_output_bytes()` reports the
current charge and `output_limit()` reports the shared ceiling.

This is a retained-output allowance, not a bound on total process memory. It
excludes allocator/control-block overhead, source owners, singleton admissions,
active encoder metadata, cursors and composition workspace. Each active native
writer still has one fixed 64 KiB payload buffer; each index has two. These are
the existing construction buffers, not additional retained copies. Filling one
can trigger streaming before the per-output ceiling is reached. Finalization
can also spill an index whose metadata is large even when its payload never
filled either buffer.

Large source value spans are consumed synchronously; merging replacement values
does not allocate a full output record or decode and re-encode the value.
Sparse entry offsets, selector seeds, the file-local selector dictionary and
final Elias–Fano metadata still occupy memory proportional to their size.
Composition callbacks may allocate their result.

A spilled fractional index uses a private secondary-stream spool. Preserving
canonical IX03 sections adds one sequential scratch write and read of the
secondary payload; the final copy has a 64 KiB transfer buffer.
[The index writer](cola-file-index.md) keeps navigation metadata in memory while
streaming FC literals. Owned and streamed finalization share the same framing
and produce the same KV03 and IX03 bytes when serialized.

Several jobs can be active at once, so construction workspace remains per writer.
Structural work credits do not measure encoded bytes or flush latency. The
scheduler charges the same construction work whether the completed output stays
owned or spills.

Tests compare adaptive and eager outputs with the owned runtime, including
complete file bytes and partial-byte tails. They cover exhausted shared budgets,
last-owner release across threads, lazy dependency binding, moves and settled
rebases, hidden job recovery, and write, file-sync and pre/post-commit failures.
Named publication tests keep ordinary small outputs adaptive; seal-specific
fault fixtures explicitly choose eager streaming.
