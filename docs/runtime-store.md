Persisting a runtime frontier
============================

`runtime_store<P>` connects the encoded runtime to the SQLite catalog. I persist
the exact searchable graph and its admission intervals together, so reopening
can restore a named tap without decoding all its keys. The native files and
fractional indexes remain immutable.

The directory must already exist and have durable ancestors. `create` creates
a version-4 catalog; `open` checks an existing one. Link with `diet::sqlite`.

```cpp
#include <diet/runtime_store.h>

using P = diet::storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<>>>>;

void initialize(std::filesystem::path const & directory) {
  auto storage = diet::runtime_store<P>::create(directory);
  diet::cola_runtime<P> runtime;
  auto current = storage.create_tap("earth-616", runtime.snapshot());
  auto record = diet::profile_record{
    diet::bit_string::from_bytes("alpha"),
    diet::bit_string::from_bytes("one")};
  auto next = runtime.contribute(record);
  current = storage.publish(current.head, next);
  storage.save("initial", current.head);
}
```

`find(name)` returns the exact catalog generation, a mapped runtime snapshot,
and opaque semantic metadata. A typed layer uses that metadata for its live
count, signature and schema identity. `find_save(name)` returns an immutable
historical selection; `fork(name, source)` starts a new named progression from
one. Neither operation copies native records.

Publication walks new graph nodes in dependency order, reserves their identities,
seals their files, records the receipts and registers the complete graph. It
then commits the root and checkpoint atomically. Unchanged native owners reuse
their existing `.kv` identity when an index changes. A wholly unchanged graph
needs only a new catalog generation. The weak caches retain neither native
data nor dead snapshots, and reopening reconstructs those caches from metadata.
Mapped inputs must have been opened by this adapter; an unrelated mapped owner
cannot silently substitute an object under a known identity.

The default physical identity allocator uses OS randomness. These are opaque
reserved names, separate from semantic signatures; this adapter does not yet
deduplicate independently constructed equal files by content. Exclusive file
installation and catalog reservations reject collisions. A caller can supply
a concrete identity allocator as the second template parameter.

The checkpoint contains oldest-first admission intervals and a versioned
semantic byte string. Opening validates the interval structure against the
exact mapped graph. It reads envelope and directory metadata, not native or
borrowed payloads. Full scans remain explicit. A pending carry resumes from its
published inputs; private partial output is restarted. I do not treat partially
written files as completed merges.

The adapter is single-owner. A mutation error disables it, retaining the last
operation identity for diagnosis; an uncertain publication requires reopening.
The last committed generation remains discoverable, including when publication
succeeded but constructing the returned mapping failed. Competing writers use
the catalog's exact generation comparison; a stale writer cannot replace the
winner. Current catalog generations and reservations remain pinned, so this is
not yet a garbage-collection policy.

The [encoded runtime](cola-runtime.md) currently uses one pending binary carry
and blocks later admissions until it is serviced. It provides a complete
searchable update path with explicit structural charges, but it does not give
the redundant scheduler's worst-case update guarantee. The runtime-store tests
exercise writes, unchanged-file reuse, snapshot saves, forks, process-equivalent
reopen, interrupted carry restart and competing publication.
