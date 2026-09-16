Streamed native and fractional-index output
==========================================

I can give the redundant scheduler an execution-owned storage context so a
native merge writes directly to its reserved `.kv` file, and fractional indexes
stream into reserved `.index` files. The context owns its
SQLite connection and concrete file operations. It survives moves and rebases
of the active engine; immutable snapshots retain their mapped inputs directly.

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

Before starting a native merge, the context reserves an object ID and attempt
ID in the catalog. The file writer fills the object through a bounded payload
buffer, seals its envelope, and performs its file and directory barriers. Only
after `record_sealed` acknowledges its transaction does the context construct a
mapped native owner carrying an immutable `native_seal` descriptor.

The descriptor records the catalog identity and exact seal receipt. Its private
factory opens the expected object path itself. A caller cannot give an arbitrary
mapping a trusted receipt. When the persistence adapter encounters this owner,
it verifies the descriptor against its own catalog and the file envelope, then
reuses the already sealed native file. It does not rewrite its payload or scan
its checksum on ordinary admission.

Sealing an object does not publish a logical update. The named root and its
checkpoint still advance in the persistence adapter's transaction. If a file
operation or catalog transaction has an uncertain outcome, the runtime and its
context become unusable. No receipt is fabricated by reading back cached bytes.
The previous acknowledged publication remains the recovery point; reservations
and surviving output names remain available to recovery. Automatic reclamation
of abandoned attempts belongs to catalog lifecycle management.

The context has its own catalog connection, not a pointer into a movable store.
One serialized worker operates it. There are no context locks or type-erased
callbacks in the record loop. Shared dependency owners retain the exact input
mappings while a merge is suspended.

Concrete storage hooks
----------------------

`redundant_runtime<P, Compose, Storage>` owns a concrete `Storage` value. In
addition to the native/profile traits, a storage family supplies:

- `make_merge<Compose>(older, newer, compose)` to start an incremental merger.
- `finish_merge(merge)` to produce its immutable native owner.
- `make_index<Node>(native, main, secondary)` and `finish_index<Node>(index)`
  to build an index over exact pinned dependencies.
- An optional `poison() noexcept` hook for failures during incremental work.

`sort_file_runtime_storage` holds a shared lifetime handle to
`sort_runtime_context`. Empty and singleton admissions are small owned profiles;
merge and index outputs are streamed and mapped. The context shares one empty
native owner across rebases. Before an index can name an owned dependency, the
graph sealer installs its catalog binding. An already bound owner ends that
walk: its mapped dependency tail is retained directly. A default-constructed
storage can make an empty seed snapshot, but it needs `open(root)` before
starting a merge or index.

For a direct engine, I can attach an opened storage with
`engine::from_snapshot(snapshot, family::storage_type::open(root))`.
`engine.rebase(equivalent_snapshot)` preserves this storage handle while replacing
a settled layout. Its metadata and admission-count checks are consistency
checks: the caller remains responsible for supplying an equivalent layout.
The named connection uses this operation after persisting a settled result.

Memory and work
---------------

Each active native writer has a 64 KiB payload buffer. Large source value spans
are consumed synchronously; merging replacement values does not allocate a
full output record or decode and re-encode the value. Sparse entry offsets,
selector seeds, the file-local selector dictionary and final Elias–Fano metadata
still occupy memory proportional to their size. Composition callbacks may also
allocate their result.

A fractional index uses two 64 KiB borrowed-payload buffers and a private
secondary-stream spool. Preserving canonical IX03 sections adds one sequential
scratch write and read of the secondary payload; the final copy has a 64 KiB
transfer buffer. [The index writer](cola-file-index.md) keeps rank directories,
false-borrow flags, cuts and Elias–Fano offsets in memory, but streams FC literals.

The scheduler can have several active merges and indexes, so these allowances
are per writer, not per database. This bounds encoded payload buffering; it
does not claim constant memory for the whole runtime or bounded wall-clock
latency. Structural work credits do not measure bytes or flush time.

Tests compare streamed updates and snapshots with the owned runtime, verify
mapped sealed owners and historical reads, exercise moves and settled rebases,
restore all hidden job phases, and inject write, file-sync and pre/post-commit
failures through both sealing and index registration. The file writer's
separate tests compare complete output bytes and cap allocations during large
value and long-control encodings.
