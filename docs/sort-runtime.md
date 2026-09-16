Sort-owned records in the redundant runtime
==========================================

I can use the same redundant COLA scheduler with sort-owned native records:

```cpp
#include <diet/sort_runtime.h>
#include <diet/typed_scan.h>

using policy = diet::string_policy;
using family = diet::sort_runtime_family<policy>;
using engine = diet::typed_engine<policy, diet::wrapping_fingerprint_algebra,
                                  256, family>;

engine active;
auto before = active.snapshot();
auto after = active.contribute(engine::put("config/theme", "dark"));
assert(!before.get("config/theme"));
assert(after.get("config/theme") == "dark");

for (auto rows = diet::scan(after); auto row = rows.next();) {
  // row->key and row->value own their contents.
}
```

`tap<engine>` provides the same serialized mutable commands and asynchronous
receipts as it does for other engine families. Snapshot contributions keep
exact old-value validation; the engine's static command factories apply to the
current state. Disjoint contributions from one base retain the same composite
signature in either order.

This family is an explicit choice. `typed_engine<>` and the existing opaque
binary and redundant families retain their current formats. The default string
schema for this family is `diet.optional-string/code0/sort-profile-v1`, distinct
from the opaque transport's `diet.optional-string/code0/v1`. A custom registry
or selector needs an explicit application schema identity.

What enters a native file
------------------------

A typed command owns a logical key and an already encoded arrow. The logical
key consists of the selector code followed by the leaf's order bits. Its length
is separate: string keys need neither escaped zero bytes nor a terminator.
Proper prefixes compare before their extensions. A raw integer key contributes
its known number of bits.

On admission, `sort_runtime_storage` writes those logical bits using the
[sort profile](sort-profiles.md). It copies the encoded value once; the value
codec supplies its framing. There is no outer variable-value length. Completed
merges use `sort_profile_merge_builder`, which retains inherited key prefixes
as spans into its pinned inputs and writes only each output suffix.

When every registered sort declares replacement semantics, the typed engine
uses the newer encoded arrow directly on a collision. Such a merge does not
need to reconstruct the key or decode either value. A mixed or categorical
registry dispatches collisions to the sort's composition operation. That path
materializes a collision key when the operation needs it; noncolliding records
retain their encoded values and borrowed prefix spans.

The sort still owns semantic hashing. Selector bits affect navigation and
physical identity, but never enter the composite table signature.

One scheduler, associated storage
---------------------------------

`redundant_runtime<P, Compose, Storage>` uses `Storage` for these operations:

- `native_type`: an immutable owner with `view`, `size`, `owned` and `mapped`.
- `mapped_pair_type`: the exact mapped native/index pair.
- `empty()` and `singleton(record)`: new native owners.
- `merge_type<Compose>`: an incremental native merger.
- `make_merge` and `finish_merge`: concrete construction and publication of native outputs.
- `encode_native(array)`: the borrowing encoder used to seal a completed array.

`profile_runtime_storage<P>` supplies the ordinary opaque profile.
`sort_runtime_storage<P, Selector>` supplies KV03 native records, existing IX03
fractional indexes and prefix-preserving native merging. The scheduler's slots,
work charges, shadow objects, publication rules and restart recipes are shared.
It does not have a second implementation for this format.

The associated `redundant_runtime_family` exposes the concrete node, native,
snapshot, frontier, object, route, slot, level and job-recipe types. Its default
storage parameter preserves the ordinary profile API. `sort_runtime_family`
adds the typed key transport used by command encoding, point queries, scans and
semantic composition. Each of those consumers therefore agrees on the exact
logical key representation.

Mapped owners retain KV03 and IX03 allocations and their dependencies. A
restored runtime can resume with mapped inputs and produce new owned sort
profiles. The tests restore a frontier with a completed hidden native merge,
remove the source paths, finish its indexing work, then admit new updates.
Catalog identity allocation and durable publication belong to the persistence
adapter. The [streaming context](sort-runtime-context.md) attaches a catalog and
file output factory to the same scheduler.

Selectors and schema dispatch
-----------------------------

`Selector::select(spigot, callback)` consumes a prefix-free sort code and calls
the typed callback with the remaining spigot. `Selector::write<S>(spigot)` emits
the matching code. `registry_selector<Registry>` is the default implementation;
a custom generated selector can supply a lookup table or switch.

The family is concrete once selected. It can be instantiated inside an outer
`policy.with_schema(user_version, callback)` dispatch without making the COLA
scheduler interpret version numbers. Application schema identities need not be
consecutive numbers. Physical KV03/IX03 revisions remain a different concern.
The outer version-to-schema dispatch and whole-store migration are separate
work; this adapter does not infer a migration from a matching C++ type.

A custom navigable key codec supplies `sort_profile_key<Codec>::order` and its
inverse `decode_order` as well as physical frame/header operations. Typed reads
and scans use that inverse, rather than the opaque transport's `read_ordered`.
A codec's finite order bits must preserve its logical order and have a unique
inverse. The supplied string, bit-string and unsigned-integer adapters satisfy
that contract.

Costs and boundaries
-------------------

The paid scheduler counts structural work, not key bytes, callback time or hard
latency. This adapter keeps completed native arrays in memory before sealing;
large merges still need memory for their output. The explicit
[streaming family](sort-runtime-context.md) selects a file-backed native writer
through an owning execution context. Its native payload buffering is bounded,
while sparse navigation metadata remains in memory. Fractional-index literals
stream through the same associated storage context.

The profile reader caches its selected leaf parser across records and physical
blocks. That avoids rerunning the selector for every record, but still makes an
indirect parser call per record. The writer also selects a leaf for each merged
frame. Moving these operations into a typed loop around an entire sort run is
further performance work. I do not impose inline attributes or other compiler
attributes on user selectors or codecs.

The focused tests cover proper-prefix strings, embedded zero bytes, partial-bit
keys, fixed three-bit values, a custom non-tree selector, mixed integer and
string sorts, noncommutative arrows, scans, independent fingerprint oracles,
disjoint updates, tap command ordering, stale input rejection, historical
snapshots and mapped pending-work restoration. Existing opaque typed, scan,
profile and redundant-runtime tests exercise the unchanged default family.
