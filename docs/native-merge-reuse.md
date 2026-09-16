Reusing completed native merges
==============================

Two forks can need the same native merge even when their fractional indexes
point into different layouts. I reuse the completed `.kv` output and let each
fork finish its own indexes. Its old index dependencies remain pinned until
that fork switches to its new layout.

The ordinary named string connection passes its exact application schema to the
storage context. Reuse covers the built-in bit-string registry and its known
replacement kernels, including conservative tombstone literals. A custom registry, selector or
composition function follows the ordinary merge path. I do not infer semantic
equality from C++ type names or from a user callback having no data members.

Exact identity
--------------

A hint names the older native file, the newer native file, and a domain containing:

- The exact application schema bytes.
- The physical policy descriptor.
- The library's operation identity: `everett.KV03.right-biased-native-merge/1`
  for raw replacement, or `everett.KV03.conservative-tombstone-merge/1` for the
  typed string merge that preserves canceled literals.

The two operations have separate cache entries even when their output contents
agree. A normally compressed tombstone cannot stand in for a conservative
literal donor merely because the table signatures match. Passing the typed
composer by reference retains its operation identity.

Input order matters. Table signatures, record counts and equal-looking payloads
are not cache keys. Independently constructed equal files can miss. Catalog
identity and canonical local directory scope the input bindings, so a copied
catalog UUID does not authorize mappings from the original directory.

At the scheduler's `native_start` step, a hit supplies a completed immutable
native. The requesting job assigns its own chronological interval and still
builds its destination and carrier indexes. A hit charges lookup/adoption work
instead of scanning or encoding records; `native_reuses` counts it separately.
Admission grants remain unchanged. Work credits are structural accounting,
not a measurement of SQLite search time or file latency.

Adaptive output stays adaptive
------------------------------

A native without an acknowledged binding causes an immediate miss, before any
SQL or file work. I never seal inputs just to discover a cache key. Both inputs
must already have bindings when construction begins for a completed merge to
install a hint.

Only naturally streamed outputs register hints in this version. A small owned
output retains the existing shared retention allowance and acquires no durable
identity for caching. Publication can later seal it for the frontier, but that
does not retroactively register a merge recipe. Reuse is checked once at the
start of a job; a concurrent completion does not interrupt an already running
merge.

Catalog ownership and failure
-----------------------------

The first naturally streamed completion installs an optional, exactly validated
STRICT `completed_native_merges` table. Ordinary catalog open and cache misses
do not create it. It is an advisory extension to core schema 4: the heads,
checkpoints and existing reader-pin representation do not change. Older core
readers can ignore the table. New readers check its exact table definition and
immutable triggers when it is present.

`record_native_merge` acknowledges the hint and a reader owner's standalone
output pin together. It accepts a trusted semantic attestation: the caller must
have run the stated replacement merge. A file receipt proves durable bytes;
it cannot prove which computation produced them. The runtime supplies this
attestation only for the supported built-in kernel and registry.

`acquire_native_merge` checks the selected output's sealed row and envelope,
adds a separate reader pin, and returns its receipt after acknowledgment. The
context then maps that exact native under its own directory. No full payload
scan is part of lookup. Recovery scans remain explicit.

The first acknowledged hint wins. Two valid producers can finish concurrently;
the later producer keeps its valid output without replacing the hint or
poisoning its context. Exact operation replay cannot change inputs, domain,
output receipt or owner. Replaying a registration rechecks its supplied output
envelope.

A file can be sealed before hint registration fails. That failure returns no
usable completed owner from the context, and an uncertain commit poisons it.
Reopening observes whichever hint committed. Failure before registration merely
leaves a sealed output without a hint; recomputation remains correct. An
uncertain acquisition likewise returns no owner, even if its pin committed.
The last acknowledged logical publication remains the recovery point.

These pins and successful-acquisition operation records are permanent under the
current append-only catalog policy. There is no hint eviction or pin-release
implementation here. This feature therefore adds retained metadata and can
retain output files; it does not claim garbage collection or bounded cache
storage.

Custom storage
--------------

`sort_runtime_family::open_storage(root, schema)` uses a storage type's
`open_for_schema(root, schema)` when it returns that exact storage type. Otherwise
it preserves the existing `open(root)` path. An inherited base helper cannot
silently bypass a custom storage constructor. A schema-empty context disables
reuse, and attaching a schema-configured context to a different typed schema is
rejected before starting runtime work.

The focused tests cover exact ordered and schema matches, custom-composition
misses, adaptive no-I/O behavior, copied-catalog namespaces, first-winner races,
independent fork indexes, hidden reused-native checkpoints, replay collisions,
unacknowledged receipts, malformed advisory schemas, corrupt envelopes, and
pre/post-COMMIT failures during registration and acquisition.
