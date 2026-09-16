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

Publication walks the completed graph in dependency order. New owned nodes
receive reserved identities, sealed files and registered receipts; nodes already
sealed by the execution context reuse their acknowledged bindings. Only objects
reachable from the complete checkpoint need publication, so discarded owned
intermediates do not acquire files. The adapter then commits the root and
checkpoint atomically. Unchanged native owners reuse
their existing `.kv` identity when an index changes. A wholly unchanged graph
needs only a new catalog generation. Each shared owner carries its acknowledged
file identity and mapped representation. The first sealing walks its immediate
dependencies; an already bound suffix stops that walk. These records disappear
with their owners, so there is no weak-owner table to sweep.

Each adapter also retains its latest restored frontier in a local identity
registry. Acquiring an owner already present there increments its local count;
we do not revisit its native, main or secondary dependencies. A newly live owner
acquires those immediate dependencies once, and the final local release retires
them. Normal publication bookkeeping therefore follows its roots and the edges
that enter or leave the retained graph, rather than its unchanged suffix. The
registry holds no older frontiers and releases its entries when the adapter is
destroyed. Other snapshots keep their own immutable owners independently.

New roots are acquired before old roots are released. Allocation failures and
conflicting facade identities roll back the acquisitions. Before checkpoint
decoding, the registry contains exactly the acknowledged head and auxiliary
roots' closure: a previous larger frontier cannot supply a missing durable pin.
Distinct owners for one physical identity, even deep inside a new prefix, take
the full canonical mapping path. That exceptional import and the first open
still inspect the complete incoming graph. Ordinary roots, checkpoint encoding
and scheduler metadata still require work; this does not make publication
constant time. The entry count follows the current retained graph; spare hash
buckets shrink geometrically when allocation succeeds.

Bindings distinguish both catalog identity and canonical local directory. A
copied catalog can share its identity with the original while naming a different
set of local files. Concurrent adapters sharing the same owner serialize only
its first sealing into a given store. Lookup and merge record loops do not use
these locks. Import checks the completed receipt and current envelope; an
unrelated mapped owner cannot silently substitute an object under a known name.
Reopening reconstructs bindings from the exact catalog graph.

Each newly sealed pair registers over already registered immediate targets.
Registration validates this pair's metadata and the main target's stored
counts without reopening the suffix. The separate bulk registration API
interns a set of imported roots together and canonicalizes their order for
operation replay. Both paths preserve the complete checkpoint's exact pins.

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

The low-level `runtime_store<P>` default,
[the binary encoded runtime](cola-runtime.md), uses one pending binary carry
and blocks later admissions until it is serviced. It provides a complete
searchable update path with explicit structural charges, but it does not give
the redundant scheduler's worst-case update guarantee. The runtime-store tests
exercise writes, unchanged-file reuse, snapshot saves, forks, process-equivalent
reopen, interrupted carry restart and competing publication. Ordinary
`connect()` instead selects the streamed redundant family described below.

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

Adaptive native and index output
---------------------------------

The ordinary named string connection can also
[reuse completed native merges](native-merge-reuse.md) from another fork. Exact
ordered input identities, application schema, physical policy and the known
replacement kernel select the result. The requesting fork retains its old
dependencies while constructing its own indexes. Reuse covers naturally
streamed completions; it never forces small adaptive outputs to acquire files.

`streaming_sort_runtime_family` keeps small native merges and fractional indexes
owned until publication, and streams larger outputs into KV03 and IX03 files.
This is the ordinary bit-profile connection backend. The typed core carries one
concrete storage context through equivalent snapshot replacements:

```cpp
#include <diet/connection.h>
#include <diet/sort_runtime_context.h>

using family = diet::streaming_sort_runtime_family<>;
using engine = diet::typed_engine<diet::string_policy,
  diet::wrapping_fingerprint_algebra, 256, family>;

auto live = diet::connect<engine>(existing_directory, "earth-616");
live.put("alpha", "one");
```

The native encoder starts in its existing 64 KiB buffer; an index uses its two
64 KiB borrowed-stream buffers. A first spill reserves an identity and attempt
before creating the destination, writes the encoded prefix and continues the
same encoder. The index resolves exact dependency bindings at that point, so a
small private index needs neither file identities nor a secondary spool. Large
metadata or an unavailable retention allowance also selects streaming, including
at finalization.

The context shares an 8 MiB retained-output allowance between natives and indexes,
with a 128 KiB ceiling per completed object. Charges follow immutable owners and
cover the object, lease and actual section-vector capacities. Construction
buffers, source owners, encoder metadata, composition workspace and allocator
overhead lie outside that allowance. Setting either output limit to zero forces
eager streaming; see [the context options](sort-runtime-context.md).

A completed owned output remains part of the scheduler's ordinary charged work.
Publication seals it only if the full frontier still retains it. A hidden
completed merge is retained too, even before an index points to it. Discarded owned
intermediate outputs release their allowance without performing file or catalog
work. The adapter returns a mapped durable snapshot after the graph and checkpoint
are committed.

After a streamed native file is sealed and SQLite acknowledges the seal, the
context opens that exact file and attaches an immutable receipt to its owner.
The receipt names the backing catalog, object, attempt, size, checksum and completed barrier. A store
accepts this owner only after checking the catalog identity, exact seal row,
object path and file envelope. This check reads metadata; it does not repeat
the merge, copy the native file or calculate its whole-body checksum. An
arbitrary mapped owner without this receipt still cannot introduce a file.

Completed hidden natives receive or reuse their exact identities in the next
full frontier checkpoint, including when no new index was produced in that step.
Private unfinished file writers restart after recovery. Published native files
are reused when the typed core rebases onto its mapped snapshot; its concrete
merge context survives the rebase.

An uncertain seal transaction never returns a sealed native owner. A failure
during publication, catalog comparison or rebase disables the persistent
engine and its active merge context, retaining the last durable snapshot for
the caller. Reopening discovers whichever catalog generation committed. A
standalone `runtime_store` only owns its own adapter: rejecting an immutable
snapshot disables that adapter without mutating an unrelated source engine.
