# Durable publication and merge resumption

The [file lifecycle](file-lifecycle.md) covers immutable `.kv` and `.index`
objects, checked envelopes and mapping lifetime. The [SQLite catalog](catalog.md)
covers world representations, pins, publication outcomes and merge continuations.
A save needs both: durable bytes and a durable way to find them.

Status: design and executable protocol model, 2026-09-15. We can check ordering
and retention decisions with injected backend outcomes using
[`durability.h`](../include/everett/durability.h). That model performs no filesystem
operations and does not yet make the store's saves durable across process exit
or power loss.

## Contract

A merge changes a world's representation. Until both the replacement and the
manifest selecting it are durable, its inputs remain our recovery source:

```text
retain old recovery root and exact input dependencies
  -> build immutable output generation
  -> verify and durably seal output and its dependencies
  -> commit the replacement representation and head in SQLite
  -> retire this merge's old durable pins
  -> reclaim only objects with no remaining owner
```

By **manifest**, I mean the immutable set of representation rows and exact
dependencies in SQLite. **Root selection** is a catalog transaction updating a
timeline or retained owner. SQLite's transaction and recovery machinery handles
this selection, without a separate custom selector file. Mapping makes bytes
readable; it does not establish when they become durable.

The initial backend uses separate immutable files. A later managed-extent
allocator must preserve the same durable identity and retention contract.

We retain the manifest's exact fractional-index dependencies as well as its
native inputs. A merge may finish before dependent indexes are rebuilt. Each
dependent publishes its own new index and adopts the shared merged data when
ready; we cannot release everybody's retention when the first dependent switches.

## Why failure changes the protocol

Rebello et al. observed filesystems marking failed writeback pages clean. A
later `fsync` can succeed without writing the missing data; cached reads can
still show the desired contents. Their experiments also found materially
different failure behavior across filesystems. A successful retry is therefore
insufficient evidence of recovery. The experiments do not describe every current
backend, but they rule out relying on that inference in a general protocol. See
[Can Applications Recover from fsync Failures?, ATC 2020, §§2–3 and §5](https://www.usenix.org/system/files/atc20-rebello.pdf).

**An error leaves the attempted publication uncertain.** The last acknowledged
durable root and its input retention remain our recovery basis. The candidate
may nevertheless have reached storage. We must discover that outcome through
recovery; an error or lost acknowledgment does not establish the catalog's
durable commit outcome.

After an output, checkpoint, allocator or catalog persistence error, we follow
these steps:

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

If we attempted root selection, we must also reconcile the catalog commit before
restarting from the old inputs. We resolve the stable operation/attempt identity
against recovered catalog state, then either verify and accept the published
candidate, or establish the old head and reconcile candidate reachability while
preserving uncertain outputs. Our recovery transaction must respect head
revisions and later committed operations; it cannot blindly overwrite a newer
head. Verifying only old inputs or a checkpoint does not resolve the head's
state. Starting a fresh generation never authorizes us to reclaim the failed
generation's extents.

Before attempting the head change, independently commit a recovery owner that
retains both the old representation and candidate outputs. We leave
that owner in place during the selecting transaction and release it only in a
later, established retirement transaction. This keeps old-input retention from
depending on the very commit whose outcome may be uncertain.

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

For the file backend, we finish and verify the external outputs, persist their
contents and discoverable names, then commit the SQLite representation
transaction. We must never overwrite an existing sealed object when installing
a name. If a verified content-identical object already exists, we acquire its
ownership through the same catalog protocol before reuse. The weak world
fingerprint does not establish that identity. SQLite cannot synchronize external
files on our behalf; [catalog.md](catalog.md) specifies its own durability
settings, supported release and transaction handling.

For managed extents, allocation and generation records take the place of file
names. Do not recycle a candidate extent after uncertain publication: a durable
root might already name it. Recovery must reconcile root reachability before
the allocator considers it free. Never overwrite already sealed checkpoint
bytes, even when appending a neighboring logical record would touch the same
physical update unit.

For macOS, we must choose and verify the power-loss barrier explicitly. Apple's
archived documentation distinguishes `fsync` from `F_FULLFSYNC`, which asks the
drive to flush its own buffered data. The Linux syscall recipe cannot be assumed
to transfer unchanged to APFS or to managed extents.
[Apple fsync manual](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fsync.2.html)

## Resumable merge checkpoints

A merge recipe needs its own identity. The world signature cannot identify its
physical inputs or encoding choices. Bind the recipe identity to:

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

A small checkpoint descriptor lives in a versioned SQLite BLOB, with relational
links to its inputs and sealed output ranges. This needs no custom checkpoint
file type. We persist the output ranges first, then commit the descriptor and
its retention in SQLite. We keep the old checkpoint reachable until the new one
is durable. Subsequent construction must preserve sealed extents physically;
promising to leave their logical prefix unchanged is insufficient. We need a
format that supports this even before final headers or sparse-index widths are
known. A final file envelope cannot be repeatedly rewritten over a sealed prefix.

If continuation context is large, budget it explicitly or reference sufficient
context in retained sealed outputs. Encoding full predecessor keys in a BLOB
does not make their storage constant-sized. External continuation data, if
needed, belongs to documented sections of the same two file kinds and follows
the same sealing/retention protocol.

On resume, we validate the recipe and input identities before trusting cursors,
and validate the selected checkpoint's entire dependency set through the
backend's recovery path. We discard or quarantine the uncheckpointed tail and
resume into fresh storage. We can reuse sealed prefix extents after
re-establishing their immutability, identities and durability. Otherwise, we
regenerate from the pinned inputs.

### Bounded first implementation

The first model checkpoints at complete-record boundaries, saving full
predecessor keys for each input and the previous output key. This supplies
the context ordinary front coding needs and keeps the initial recovery protocol
straightforward. The metadata continuation is opaque versioned data; actual
rank/select/merge codec serialization and its SQLite representation are still
to be implemented and validated. The model's byte-vector fields are not an
implementation of typed byte/bit checkpoint encoding.

Pausing inside an enormous key will require an additional continuation: decoded
prefix extent, current prefix/suffix lengths, comparison state, pending value
units, output record progress and checksum state. Until that exists, a complete
record is the minimum checkpoint unit; this first model does not promise a fixed
checkpoint latency independent of key length.

## Accounting and backpressure

Recovery consumes work beyond the prepaid merge budget. We track output
bytes, prefix reconstruction, index rebuilding, checksum work and descriptor/spool
serialization. Reconstructing after an I/O failure costs additional work; a
failed attempt does not restore the credit already spent on it.

We must reserve enough space for retained inputs and snapshots, the working
output, sealed checkpoints, pending metadata, root records and a recovery
generation. We keep charging uncertain candidates until recovery proves them
unreachable. Immutable prefix sharing can reduce duplicate bytes, but not
retention lifetime. Since failure can prevent compaction from freeing space, we
must throttle admission before consuming the reserved recovery headroom.

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

The generic pin layer's immutable `replace(exact_inputs, outputs)` operation
supplies the in-memory ownership boundary. We obtain output handles before constructing
the replacement, persist the candidate root while keeping the old owner alive,
and retire only after durable publication. Existing saves and readers retain
their own owners. Index dependencies, checkpoint ranges and staged candidates
add retention without another logical contribution to the world's sum. During
recovery, we reconstruct durable reachability before garbage collection; reader
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

Our tests inject failure at each publication boundary and reproduce a fake
writeback failure that clears dirty state while leaving new cache bytes. They
also check context-preserving checkpoint resumption, stale identities, backward
cursors, size overflow and uncertain manifest reconciliation. Filesystem,
power-cut, process-restart, allocator and actual mmap durability tests remain
backend work. That backend must durably serialize recovery facts and reconstruct
the state machine after process restart.
