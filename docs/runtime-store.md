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
Their cleanup cursor inspects a bounded number of owners per operation and
per insertion; it does not scan all retained snapshots on every write. Expired
cache entries can remain until that cursor reaches them.
Mapped inputs must have been opened by this adapter; an unrelated mapped owner
cannot silently substitute an object under a known identity.

I register a checkpoint's roots together. They often share a long suffix, so
the registration walks and records each distinct pair once. Root identities
are sorted and deduplicated before recording the operation; passing the same
set in a different order replays the same registration. This does not change
which roots and hidden artifacts the checkpoint pins.

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

The default [encoded runtime](cola-runtime.md) uses one pending binary carry
and blocks later admissions until it is serviced. It provides a complete
searchable update path with explicit structural charges, but it does not give
the redundant scheduler's worst-case update guarantee. The runtime-store tests
exercise writes, unchanged-file reuse, snapshot saves, forks, process-equivalent
reopen, interrupted carry restart and competing publication.

Complete redundant frontiers
----------------------------

The storage adapter also accepts the three-slot redundant scheduler. Its fourth
template parameter selects the runtime family; the second selects physical ID
allocation and the third supplies the SQLite fault-injection seam:

```cpp
#include <diet/redundant_checkpoint.h>
#include <diet/runtime_store.h>

using family = diet::redundant_runtime_family<P>;
using storage = diet::runtime_store<P, diet::random_object_ids,
  diet::sqlite_catalog_ops, family>;
```

A redundant checkpoint owns more than its query root. I record every occupied
slot, exact route, ready carrier, hidden completed native or index, pending job
phase, chronological interval, and remaining service obligation. The catalog
pins the additional pair roots and standalone native files in the same
transaction as the visible root and checkpoint. Saving or forking copies these
pins without copying file contents. An operation replay returns the exact
historical pin set as well as its checkpoint.

Reopening interns file identities across all these roots, so two routes to the
same native or index retain the same immutable owner. It checks the scheduler's
slot and dependency invariants before admitting the frontier. A checkpoint
cannot refer to an artifact outside its durable pin closure. None of these
checks scans FC strings or computes a whole-body checksum.

`redundant_runtime::checkpoint()` captures the current full frontier, including
completed artifacts from work since its previous publication. Private partial
builders are restarted after recovery; their unfinished output is not treated
as durable. The restored scheduler admits no new contribution until recovery
service has made its frontier safe. The same capture and mapping path works
for `persistent_engine` through the typed core's runtime family.

Sort-owned native files
-----------------------

`sort_runtime_store` uses the same catalog and checkpoint machinery for KV03
native files. Each registered sort supplies the key and value grammar; integer
keys need not acquire the framing of a front-coded string. The fractional
indexes retain their IX03 representation.

```cpp
#include <diet/connection.h>
#include <cassert>

using family = diet::sort_runtime_family<>;
using engine = diet::typed_engine<diet::string_policy,
  diet::wrapping_fingerprint_algebra, 256, family>;

void update(std::filesystem::path const & existing_directory) {
  auto live = diet::connect<engine>(existing_directory, "earth-616");
  live.put("alpha", "one");
  auto before = live.snapshot();
  live.save("before", before);
  live.put("alpha", "two");
  assert(before.get("alpha") == "one");
}
```

For lower-level frontier work, include `diet/sort_runtime_store.h` and use
`sort_runtime_store<P, Selector>`. The selector defaults to the policy's sort
registry. Both storage paths intern mapped owners across the complete visible
and hidden graph, preserve native identities when only an index changes, and
check the exact durable pin closure on recovery.

The default string schema for this family is
`diet.optional-string/code0/sort-profile-v1`. A custom registry requires its
application schema identity. The physical native format is also checked: an
opaque-profile connection cannot reinterpret KV03 records merely because its
sampling policy happens to match.

Ordinary reopen reads directory and checkpoint metadata. It does not decode
the native FC stream or its borrowed samples. `mapped_sort_cola::scan()` is the
explicit recovery operation: it checks file checksums, native ordering, the
interleave rank metadata, false-borrow flags, and the actual keys selected from
both downstream routes. A `mapped_cola_scan<Mapped>` context can scan several
hidden roots without repeatedly checking their shared suffixes.
