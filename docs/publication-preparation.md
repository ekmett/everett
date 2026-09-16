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
both seals and the pair. Individually, this takes two catalog commits instead
of the separate native and index paths' four; ready units can also share the
reservation as described below. The final named session compare-and-swap remains a
separate transaction.

This saving applies to an eligible pair, not every file produced by a write.
Already bound natives, streamed native receipts, and hidden native-only outputs
keep their separate seal acknowledgment paths. Ready index-only or native-only
units may still join a reservation group. Adaptive output selection and all
merge/index work remain unchanged.

Ready reservation batches
-------------------------

Before the ordinary dependency walk, I collect one group of at most 16 ready
units. A unit is a native/index pair with acknowledged targets, an index over
an acknowledged native, or a hidden native-only output. Each unit can finish
without producing another dependency. An unbound native belonging to any
unbound pair stays available for joint preparation, even when that pair's
targets are not ready yet.

The group shares one reservation transaction. Each unit still writes and
flushes its own files and acknowledges its seals separately; the final session
compare-and-swap remains separate too. For $b$ selected units, this removes
$b-1$ reservation commits. Once that group finishes, the ordinary walk prepares
the rest of the graph. The cap bounds selected units, not dependency discovery
or total publication work. We do not repeatedly rescan the graph looking for
more groups.

Planning captures and validates existing dependency bindings and prepares all
producer slots before acquiring any claim. Claims use nonblocking locks and
deduplicate shared native owners. A busy slot or a binding installed since
planning falls back to the ordinary walk. While claims are held, the group
neither recursively resolves dependencies nor waits for another producer lock.
This lets concurrent branches make progress without reversing the dependency
lock order.

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
pair's shape and target counts. The main dependency must already be a registered
pair, and the secondary native must already have an acknowledged seal. An
own-native secondary alias therefore uses the graph sealer's index-only fallback,
not this direct method. Its transaction performs reservation checks, two seal-row
updates, and pair insertion using that prepared evidence. The apply path performs
no explicit filesystem operations.

An exact retry revalidates the current file evidence before returning the recorded
outcome. A fresh request rejects wrong-kind, unreserved or already sealed
objects. The existing `seal_pair` method remains the index-only operation over
an acknowledged native.

Failure and recovery
--------------------

An uncertain shared reservation installs no bindings. Its exact operation can
be reconciled after reopening, as with an individual reservation. If a later
unit fails, earlier acknowledged units keep their installed bindings; healthy
retries can reuse them. The unacknowledged units remain unavailable to readers.

A pre-commit failure acknowledges neither seal nor the pair. An error reported
after COMMIT may mean that all three rows are durable; the catalog handle is
poisoned and the sealer exposes neither binding. Reopen and reconcile the exact
operation with the same two receipts and pair identity.

Mapping or binding allocation can still fail after acknowledgment. The durable
rows remain valid. An installed native binding can survive a later pair-binding
failure, so a healthy retry can reuse that native and prepare another index.
Preparation does not change the named session's logical head. Losing a later session
compare-and-swap can leave prepared objects; an uncertain publication can also
leave a committed new generation, which must be reconciled after reopening.
This protocol does not reclaim retained attempts or historical graphs.

Focused validation
------------------

`tests/sqlite_catalog_native_pair.cc` exercises both opaque and sort-owned native
formats. It checks two-row/pair atomicity, before/after-COMMIT reconciliation,
exact replay, receipt and envelope rejection, source-native aliases with both
target roles, concurrent branches sharing a native, hidden-only preparation,
an actual index installation collision after native sealing, and an injected
post-acknowledgment mapper allocation failure. Queries verify
that separate branch indexes retain their own target values.

`tests/sqlite_catalog_ready_reservations.cc` checks the 16-unit bound, aliases,
unready dependencies, concurrent publishers, and before/after-COMMIT failures
at reservation and acknowledgment. A real 64-row frontier compares grouped
and ordinary preparation: 17 reservations become 11, while all 24 files,
hidden checkpoint owners, mapped queries, scans and signatures are preserved.
