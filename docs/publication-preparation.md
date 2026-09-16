Publication preparation
=======================

I keep file durability separate from catalog acknowledgment. A native `.kv` and
its fractional `.index` each complete the existing write, protection, file-sync,
installation, directory-sync and private-name cleanup protocol. The catalog then
records which exact bytes belong to the immutable pair. None of those file
barriers runs while the pair acknowledgment holds SQLite's writer lock.

When publication first encounters a pair whose native still lives in an owned
buffer and has no acknowledged catalog binding, we can prepare both outputs
locally. One reservation names both objects, and one transaction acknowledges
both seals and the pair. This takes two catalog commits instead of the separate
native and index paths' four. The final named tap compare-and-swap remains a
separate transaction.

This saving applies to an eligible pair, not every file produced by a write.
Already bound natives, streamed native receipts, and hidden native-only outputs
keep their existing paths. Adaptive output selection and all merge/index work
remain unchanged. In particular, this is not a graph-wide staging transaction.

Ordering and shared owners
--------------------------

The graph sealer resolves the main pair and secondary native before entering
the pair's own native binding producer. If a dependency uses the same native,
that dependency supplies its acknowledged binding first. The new index then
uses the ordinary index-only path.

For two concurrent pairs sharing one unbound native, the native owner's existing
binding slot elects one producer. That producer can reserve and acknowledge its
native and index together. The other pair uses the resulting native binding and
prepares its own index against its own targets. The producer does not acquire
another dependency binding while holding the native slot.

Bindings retain the exact catalog identity and canonical root namespace. No
binding becomes visible before its acknowledgment succeeds. Current envelopes
are still checked when an acknowledged binding is imported into another backend.

Catalog API
-----------

`sqlite_catalog<P>::seal_native_pair<Mapped>(operation, identity,
native_receipt, index_receipt)` accepts completed barrier receipts and returns
the pinned native and index mappings after acknowledgment. Its replay request
records both complete receipts in native/index order, followed by the exact pair
identities, targets and counts.

Before the write transaction, the method checks canonical receipt paths, opens
and pins both mappings, validates their envelopes and formats, and checks the
pair's shape and target counts. The main dependency must already be a registered pair, and the secondary
native must already have an acknowledged seal. An own-native secondary alias
therefore uses the graph sealer's index-only fallback, not this direct method. Its transaction performs reservation checks, two seal-row updates,
and pair insertion using that prepared evidence. The apply path performs no
explicit filesystem operations.

An exact retry revalidates the current file evidence before returning the recorded
outcome. A fresh request rejects wrong-kind, unreserved or already sealed
objects. The existing `seal_pair` method remains the index-only operation over
an acknowledged native.

Failure and recovery
--------------------

A pre-commit failure acknowledges neither seal nor the pair. An error reported
after COMMIT may mean that all three rows are durable; the catalog handle is
poisoned and the sealer exposes neither binding. Reopen and reconcile the exact
operation with the same two receipts and pair identity.

Mapping or binding allocation can still fail after acknowledgment. The durable
rows remain valid. An installed native binding can survive a later pair-binding
failure, so a healthy retry can reuse that native and prepare another index.
The persistent publication adapter retains its previous acknowledged logical
head on failure. Likewise, losing a later tap compare-and-swap can leave prepared
objects. This protocol does not reclaim retained attempts or historical graphs.

Focused validation
------------------

`tests/sqlite_catalog_native_pair.cc` exercises both opaque and sort-owned native
formats. It checks two-row/pair atomicity, before/after-COMMIT reconciliation,
exact replay, receipt and envelope rejection, source-native aliases with both
target roles, concurrent branches sharing a native, hidden-only preparation,
an actual index installation collision after native sealing, and an injected
post-acknowledgment mapper allocation failure. Queries verify
that separate branch indexes retain their own target values.
