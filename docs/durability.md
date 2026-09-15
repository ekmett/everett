# Durable publication and merge resumption

The [file lifecycle](file-lifecycle.md) specifies immutable `.kv` and `.index`
objects, checked envelopes and mapping lifetime. The [SQLite catalog](catalog.md)
owns world representations, pins, publication outcomes and merge continuations.

Status: design and executable protocol model, 2026-09-15. The implementation in
[`durability.h`](../include/everett/durability.h) checks ordering and retention
decisions using injected backend outcomes. It performs no filesystem operations
and does not yet make the store's saves durable across process exit or power loss.

## Contract

A merge replaces representations of a logical world. Its inputs remain the
recovery source until the replacement and the manifest selecting it are durable:

```text
retain old recovery root and exact input dependencies
  -> build immutable output generation
  -> verify and durably seal output and its dependencies
  -> commit the replacement representation and head in SQLite
  -> retire this merge's old durable pins
  -> reclaim only objects with no remaining owner
```

Here a **manifest** is the immutable set of representation rows and exact
dependencies in SQLite. **Root selection** is a catalog transaction updating a
timeline or retained owner; it is not a separate custom selector file. The
catalog uses SQLite's transaction and recovery machinery. Mmap is a reading
mechanism and does not establish publication order.

The initial backend uses separate immutable files. A future managed-extent
allocator must preserve the same durable identity and retention contract.

The retained manifest includes its exact fractional-index dependencies. A merge
may finish before dependent indexes are rebuilt. Each dependent publishes its
own new index and adopts the shared merged data when ready. Retention is not
released globally when the first dependent switches.

## Why failure changes the protocol

Rebello et al. observed filesystems marking failed writeback pages clean. A later
`fsync` can succeed without writing the missing data; cached reads can still show
the desired contents. Their experiments also found materially different failure
behavior across filesystems. This is evidence against inferring recovery from a
successful retry, rather than a universal description of every current backend.
See [Can Applications Recover from fsync Failures?, ATC 2020, §§2–3 and §5](https://www.usenix.org/system/files/atc20-rebello.pdf).

**An error leaves the attempted publication uncertain.** The last acknowledged
durable root and its input retention remain our recovery basis. The candidate
may nevertheless have reached storage. We must discover that outcome through
recovery; an error or lost acknowledgment does not establish the catalog's
durable commit outcome.

After an output, checkpoint, allocator or catalog persistence error:

1. Stop publication and retirement for that attempt; preserve the error and all
   relevant object identities. Broaden the quarantine if the backend reports a
   failure affecting a shared container or allocation domain.
2. Keep the original immutable inputs, previous durable checkpoints, old root
   records and possibly published candidates discoverable. Do not rely solely
   on an in-memory reference count to preserve them after a crash.
3. Establish which stored state can be trusted. An ordinary reread through the
   same cache, reopening the file, or an unchanged-descriptor `fsync` retry is
   insufficient as general recovery evidence.
4. Regenerate uncertain bytes into a fresh output generation from verified
   retained inputs, or use a backend-specific recovery protocol that establishes
   the candidate's persistent contents and naming. If that cannot be established,
   retain the old roots and report the storage failure; no progress guarantee is
   possible on unavailable or damaged storage.

Regeneration means writing the necessary bytes again, not merely retrying a
flush. It still requires a functioning storage path and successful verification
and persistence of the new generation. Copying cached uncertain output into a
new file is not regeneration from the immutable inputs.

If root selection was attempted, restarting from the old inputs also requires
reconciling the catalog commit. Resolve the stable operation/attempt identity
against recovered catalog state. Either verify and accept the published
candidate, or establish the old head and reconcile candidate reachability while
preserving uncertain outputs. A recovery transaction must respect head revisions
and later committed operations; it cannot blindly overwrite a newer head.
Verifying only old inputs or a checkpoint does not resolve the head's state.
A fresh generation never authorizes reclaiming the failed generation's extents.

Before attempting the head change, commit an independent recovery owner that
retains both the old representation and candidate outputs. The selecting
transaction leaves that owner in place. Only a later, established retirement
transaction releases it. This avoids making old-input retention depend on the
very commit whose outcome may be uncertain.

## Backend publication obligations

| Phase | Required fact before advancing | What remains retained |
| --- | --- | --- |
| Build | Output generation is private; writes cannot mutate a published input or checkpoint extent | Old manifest, inputs, saved checkpoints |
| Seal output | Full data/index sections, framing, lengths, checksums, allocation metadata and exact dependency names are verified and durable | All old retention plus output |
| Prepare manifest | Candidate representation names only sealed output and exact targets; old/candidate recovery ownership was already committed | Old root and candidate objects |
| Publish manifest | One catalog transaction commits representation, selected head, owner links, contributions and operation outcome; external names were already durable | Independent recovery ownership until explicit retirement |
| Retire | Durable ownership removes only this superseded root/checkpoint/merge's references | Every other save, branch, reader, builder and dependent index |
| Reclaim | No durable or live owner can still reach the object; allocator retirement is crash-consistent | Unrelated owners and conservative quarantine |

For a file backend, Linux documents that syncing a file alone does not ensure
its directory entry is durable; the directory needs its own synchronization.
[`fsync(2)`](https://man7.org/linux/man-pages/man2/fsync.2.html)

Linux `rename` provides atomic replacement visibility. When used to give a
sealed object its final name, that visibility does not substitute for persisting
its contents and directory changes.
[`rename(2)`](https://man7.org/linux/man-pages/man2/rename.2.html)

For the selected file backend, finish and verify the external outputs, persist
their contents and discoverable names, then commit the SQLite representation
transaction. Never overwrite an existing sealed object when installing a name.
If a verified content-identical object already exists, acquire its ownership
through the same catalog protocol before reuse. The weak world fingerprint
does not establish that identity. SQLite cannot synchronize external files on
our behalf; its own durability settings, supported release and transaction
handling are specified in [catalog.md](catalog.md).

For managed extents, allocation and generation records take the place of file
names. Do not recycle a candidate extent after uncertain publication: a durable
root might already name it. Recovery must reconcile root reachability before
the allocator considers it free. Never overwrite already sealed checkpoint
bytes, even when appending a neighboring logical record would touch the same
physical update unit.

The macOS backend must choose and verify its power-loss barrier explicitly.
Apple's archived documentation distinguishes `fsync` from `F_FULLFSYNC`, which
asks the drive to flush its own buffered data. This document does not assume
that the Linux syscall recipe transfers unchanged to APFS or to managed extents.
[Apple fsync manual](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fsync.2.html)

## Resumable merge checkpoints

The merge recipe has an identity separate from the world signature. It binds:

- Exact immutable input versions and their order/precedence, comparator and
  encoding versions, sort/schema and hash policies, resolver rules, and
  tombstone-elision context.
- Job, attempt, output and checkpoint generations, and all exact index
  dependencies. IDs are stable and never reused for different work.
- Canonical policy P, byte/bit address unit, group size K and continuation
  encoding version. Fixed value widths are measured in P's units; the borrowed
  stream has zero-width values under the same P.
- Input record ordinals and encoded positions in P's units, together with
  decoding context at each input cursor. Physical file ranges are separately
  expressed in bytes. For bit profiles, preserve meaningful bit lengths and
  reject noncanonical padding or ambiguous byte/bit conversions.
- Previous output key and encoder context. Resuming after front coding requires
  those units or an explicitly sufficient anchored representation, including
  actual predecessor lengths used by grouped backspace framing.
- Output stream boundaries: completed records, sealed physical ranges, group
  offsets, native/borrowed counts, false-borrow flags, and partial `rank_groups<K>`
  and `select_groups<K>` builder state. These group counts measure entries, not
  address units. Elias–Fano's final width may still depend on the final extent;
  the continuation must preserve enough information to finish its sparse
  offsets without a hidden unrecorded spool.
- Merge-selection state, value/tombstone resolution state and algebraic
  accumulator state, plus performed-work counters.
- Byte-integrity digests and exact lengths covering every reused extent and the
  checkpoint descriptor itself. The algebraic world signature is not a byte
  checksum and cannot establish physical checkpoint identity.

A small checkpoint descriptor is a versioned SQLite BLOB with relational links
to its inputs and sealed output ranges. It adds no custom checkpoint file type.
Persist the output ranges first, then commit the descriptor and its retention
in SQLite. The old checkpoint remains reachable until the new one is durable.
Subsequent construction must preserve sealed extents physically, not merely
promise that the application will not change their logical prefix. The format
must accommodate this even when final headers or sparse-index widths are not
yet known; a final file envelope cannot be repeatedly rewritten over a sealed
prefix.

If continuation context is large, budget it explicitly or reference sufficient
context in retained sealed outputs. Encoding full predecessor keys in a BLOB
does not make their storage constant-sized. External continuation data, if
needed, belongs to documented sections of the same two file kinds and follows
the same sealing/retention protocol.

On resume, validate recipe and input identities before trusting cursors. Validate
the entire selected checkpoint's dependency set through the backend's recovery
path. Discard or quarantine the uncheckpointed tail; resume into fresh storage.
Reuse of sealed prefix extents is allowed if their immutability, identities and
durability were re-established. Otherwise regenerate from the pinned inputs.

### Bounded first implementation

The current model checkpoints only at complete-record boundaries and saves full
predecessor keys for each input plus the previous output key. This is sufficient
context for ordinary front coding and keeps the first recovery protocol clear.
The metadata continuation is opaque versioned data; actual rank/select/merge
codec serialization and its SQLite representation are still to be implemented
and validated. The model's byte-vector fields are not an implementation of
typed byte/bit checkpoint encoding.

Pausing inside an enormous key will require an additional continuation: decoded
prefix extent, current prefix/suffix lengths, comparison state, pending value
units, output record progress and checksum state. Until that exists, a complete
record is the minimum checkpoint unit; this first model does not promise a fixed
checkpoint latency independent of key length.

## Accounting and backpressure

The prepaid merge budget is distinct from recovery work. Track output bytes,
prefix reconstruction, index rebuilding, checksum work and descriptor/spool
serialization. Reconstructing after an I/O failure costs additional work; failed
attempts do not magically regain successful-work credit.

Reserve enough space for retained inputs and snapshots, the working output,
sealed checkpoints, pending metadata, root records and a recovery generation.
Uncertain candidates remain charged until recovery proves them unreachable.
Immutable prefix sharing can reduce duplicate bytes, but not retention lifetime.
Failure can prevent compaction from freeing space, so admission must throttle new
work before reserved recovery headroom is consumed.

Checkpointing full prefix contexts costs their actual stored bytes, even when
the codec counts bits. Amortize descriptor writes by checkpointing at useful
byte/work intervals; do not write one complete checkpoint per item. SQLite BLOBs
and journals count against that budget. A managed container must account for
padding or fresh physical units needed to preserve checkpoint immutability.

## Pins integration and implementation boundary

Durable round replay is a separate obligation from merge resumption. A round's
catalog state must bind its base world/round identity, partition assignments,
accepted update identities and completion/deduplication information. A failed save may
leave its acknowledgment uncertain even when the round reached storage. Recovery
must reconcile that outcome before accepting a duplicate update or advancing
the next round. Merge checkpoints only recover representation work; they cannot
serve as admission records for accepted participant updates. SQLite stores those
records and their replay horizon. The adapter and durable replay executor remain
future backend work.

The generic pin layer's immutable `replace(exact_inputs, outputs)` operation is
the in-memory ownership seam. Obtain output handles before constructing the
replacement. Persist the candidate root while keeping the old owner alive;
retire only after durable publication. Existing saves and readers retain their
own owners. Index dependencies, checkpoint ranges and staged candidates add
retention without adding another logical contribution to the world's sum.
Recovery reconstructs durable reachability before garbage collection, and reader
acquisition must serialize with deletion claims as specified in the catalog.

`merge_publication` is an executable ordering model. Its `durable_verified` and
`durable_state_verified` events are backend assertions, not security tokens and
not automatically inferred from syscall results. It tracks the latest
checkpoint, requires fresh generations after failure, and permits explicit
reconciliation when recovery proves that an uncertain manifest actually
published. Resumption after an attempted manifest additionally requires explicit
old-root selector recovery evidence; input/checkpoint verification alone is
rejected. With the chosen SQLite architecture, that abstract selector evidence
means reconciliation of the committed head and operation outcome, not recovery
of a custom selector file. The current model has no SQLite adapter. Releasing
its old-pin flag grants no authority to delete references held by another owner.

The tests inject failure at each publication boundary and reproduce a fake
writeback failure that clears dirty state while leaving new cache bytes. They
also check context-preserving checkpoint resumption, stale identities, backward
cursors, size overflow and uncertain manifest reconciliation. No filesystem,
power-cut, process-restart, allocator or actual mmap durability test is claimed.
Durable serialization of recovery facts and reconstruction of the state machine
after process restart belong to the future backend.
