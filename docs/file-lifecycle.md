# Files, mappings and publication

Updated 2026-09-15. Everett uses SQLite for catalog metadata and two custom file
kinds for bulk data: `.kv` and `.index`. Read-only mapping, checked object
envelopes and typed codecs are implemented foundations. Serialized codec
sections, the SQLite adapter, writers and recovery executor remain work. See
[implementation status](implementation.md), the [catalog design](catalog.md)
and the [failure and resumption protocol](durability.md).

## Objects and names

| Extension | Intended encoded contents | Exact dependencies recorded in the catalog |
| --- | --- | --- |
| `.kv` | Immutable native LPFC keys, values and native sampled offsets | Any objects required by the value/arrow representation |
| `.index` | Modified front-coded borrowed keys, borrowed sampled offsets, origin ranks and false-borrow flags | Its native source and exact downstream blob/index versions |

A fractional index is an independent object; several index versions can share
one unchanged native file. SQLite's immutable representation rows select the
compatible objects, their precedence and additive contributions. A logical
world can have several representations, and a saved branch point retains one
exact representation. Jobs and checkpoints also live in SQLite; their owners
retain unfinished work without adding its fingerprint a second time.

Thus **manifest** below means a representation recorded in the catalog, and
**root publication** means an atomic catalog transaction selecting it. Everett
does not add a custom manifest file, root-selector file or metadata journal.
SQLite manages its own database and journaling files.

### Path spelling and content identity

Within an object directory, a canonical 128-bit physical identity is written
as 32 lowercase hexadecimal digits and split without repeating its prefix:

```text
objects/ab/cd/ef0123456789abcdef0123456789ab.kv
objects/ab/cd/ef0123456789abcdef0123456789ab.index
```

The example shows the implemented path spelling for two object kinds; related
objects need not receive the same identity. The current codec accepts an opaque
128-bit ID. It does not compute a content hash or prove uniqueness. Allocation
must prevent ID reuse, check collisions and distribute the shard prefixes.

Content-addressed native files are the intended sharing contract: the address
identifies verified encoded bytes under a specified hash algorithm and format,
not the weak fingerprint of their resolved logical contents. Different merge or
codec layouts can represent the same world and have different content addresses.
A private construction ID is reserved before the final content is known; sealing
establishes its final content descriptor and name before publication. Reusing
an existing candidate requires verified byte identity and a retained owner.

The content-ID algorithm, digest width and canonical hash input are still to be
chosen and implemented. The preimage must bind kind, interpretation metadata,
meaningful bit extent and payload, with an explicit rule for any derived fields.
Do not introduce a self-referential hash by including the address itself. A
future digest may require a wider path format than today's 128-bit primitive.
Neither CRC32C nor the algebraic world signature supplies this content identity.

Create shard directories lazily. Sharding bounds the entries in a directory;
it does not reduce the total inode count. An eventual managed-extent allocator
can retain the same immutable-object interface while packing physical objects.

## Object envelope

The initial portable envelope has a 96-byte explicitly little-endian header.
Never serialize a C++ struct by copying its memory. The header binds kind,
format version, length, policy and integrity metadata. Kind-specific magic is:

| Kind | Eight magic bytes |
| --- | --- |
| Native | `EVRT.KV` followed by zero |
| Fractional index | `EVRT.IX` followed by zero |

The extension helps people; the header establishes the object's actual kind.
The reader checks magic, version, reserved fields, policy unit, group size,
fixed-width descriptor, exact extent and CRC32C for the header and body before
exposing a validated object. CRC32C detects accidental corruption; it is not an
authentication mechanism and is separate from the algebraic world fingerprint.

The current `file<P>::open` and `from_slice` compute the whole body's CRC32C.
Opening therefore costs O(physical body bytes) and can fault every mapped page.
Mapping preserves direct byte access after validation, but this reader does not
yet provide logarithmic or lazy opening. A future page/block integrity format
must make checked partial reads explicit; simply skipping validation is not that
format.

`file<P>` must reject persisted metadata inconsistent with `P`. Byte profiles
measure their body extent in bytes. Bit profiles measure meaningful bits, with
MSB-first bytes and zero trailing padding. The physical mapping still has a byte
length. Conversion and overflow checks occur at that boundary; padding does not
increase the logical bit universe.

The policy's fixed-value descriptor and a stream's actual common width are
different metadata. A borrowed-key stream has no value payload, even when the
shared policy says native values have fixed width. For a native stream with
common width `v`, sampled residual positions remove `ordinal * v`; add the same
stride back when locating the record. A width constant only within each sort
does not suffice for a single shared stride. The LPFC restart rule can still
change key redundancy when record widths change: only the direct fixed-payload
contribution is removed from the Elias–Fano universe.

The envelope currently accepts an opaque body. It is not yet a serialized
`profile_blob<P>`. Codec sections still need checked offsets, versions and exact
dependency descriptors that agree with the catalog. Metadata such as world
manifests and merge continuations belongs in SQLite rows and versioned BLOBs.

## Mapped lifetime

Published files are immutable and fixed in length. Map them read-only. A
`mapped_slice` keeps its mapping alive, so a view cannot accidentally outlive the
mapped object merely because the opener closed its handle. Checked subranges
use lengths and relative offsets, not unchecked pointer arithmetic. Empty
files need no zero-length mapping. Never truncate, overwrite or recycle a file
while a reader can still reach it.

Keep navigable LPFC/FC and rank/select sections directly readable from mappings.
Opaque whole-file compression would require another decompressed allocation
before those structures can be used. LevelDB similarly avoids a second cached
copy when an uncompressed block already resides in stable mapped memory; its
compressed block paths allocate decoded storage.
[LevelDB block reader](https://github.com/google/leveldb/blob/main/table/format.cc)

LevelDB's POSIX environment limits mappings and open descriptors and can fall
back to positional reads. Its process-local lock table supplements operating
system locking. Everett likewise needs explicit mapping/descriptor budgets.
With SQLite, short catalog transactions arbitrate publication and reclamation;
workers build private generations outside those transactions.
Any implementation using an additional coordinator must prevent duplicate
ownership, including repeated opens within one process.
[LevelDB POSIX environment](https://github.com/google/leveldb/blob/main/util/env_posix.cc)

A mapping's lifetime and a catalog pin have different jobs. The former keeps
already mapped bytes addressable; the latter prevents removal before a later
dependency is opened. Acquire the exact representation and its dependency
closure through the catalog's reader-owner transaction before opening files,
and retain that owner until all views and deferred opens are finished. A SQLite
read transaction alone does not pin external objects. Acquisition and GC claims
must serialize; an object already claimed for deletion cannot be reacquired.
See [reader acquisition and liveness](catalog.md#6-reader-acquisition-and-garbage-collection).

## What to retain from LevelDB's lifecycle

LevelDB separates immutable tables from the metadata selecting them. Table
construction finishes and synchronizes the file before offering it to the
version update. Metadata installation appends and synchronizes a version edit;
a newly created manifest also needs root selection before the in-memory
version is installed. These are useful publication boundaries for Everett,
independent of the choice of compaction schedule.
[Table builder](https://github.com/google/leveldb/blob/main/db/builder.cc),
[version installation](https://github.com/google/leveldb/blob/main/db/version_set.cc)

LevelDB's obsolete-file collector includes pending outputs and all retained
versions in its live set. It stops collection after a background error, because
publication may be uncertain. Everett needs the same protection for builders,
plus its explicit saves, old fractional targets and shared merge candidates.
Completing a merge does not release somebody else's old index dependency.
[Live files and pending outputs](https://github.com/google/leveldb/blob/main/db/db_impl.cc)

For Everett, the catalog records exact dependency edges and explicit owners.
Runtime reference counts can cache that graph but cannot substitute for
reconstructing its durable roots after restart. These LevelDB examples motivate
publication boundaries and conservative retention; SQLite supplies our metadata
transaction and recovery machinery.

## Publication protocol

For a new generation:

1. Commit a job/attempt identity, reserved private outputs and ownership of the
   exact input graph. An independent recovery owner must retain the old
   representation and candidate outputs before the selecting transaction.
2. Write private outputs. Finish and verify their encoded sections, extents,
   checksums and dependencies.
3. Persist the output contents and the directory entries needed to find them.
   The final content address, when used, is established and verified here. A
   checkpoint's sealed prefixes must remain physically immutable.
4. Register the sealed object graph, then atomically select its representation
   in SQLite. Recheck the expected head revision, exact inputs and operation ID;
   commit the head, owners, contributions and operation outcome together.
5. Acknowledge successful publication. Retire the auxiliary recovery/job owners
   in a later transaction, after the outcome is established. Other branch,
   reader, cache and dependent-index owners remain independent.
6. Claim unreachable objects for GC in a catalog write transaction. Unlink
   outside it, persist the namespace change, then record deletion's outcome.
   Claimed identities cannot be reused or acquired during this interval.

Do not put the recovery-owner release in step 4: it preserves both possible
outcomes if the selecting commit becomes uncertain. Long writes, checksums and
file synchronization occur outside the catalog write transaction. Exact SQL
relationships, retry identities and transitions are in [catalog.md](catalog.md).

File synchronization and directory-entry persistence are separate obligations.
SQLite's commit cannot flush Everett's external outputs for us. The backend
must specify its file, directory and device barriers, including initial catalog
creation. Platform details and failed-`fsync` handling are in
[durability.md](durability.md).

An error after publication is attempted does not prove rollback. Preserve old
roots and candidates, stop retirement/reclamation in the affected domain, and
reconcile the catalog operation outcome and external bytes. A later successful
flush alone does not prove that failed writeback data has been regenerated.

## Acknowledgment, checkpoints and recovery

An update is acknowledged as durable only after its complete immutable payload
and the catalog transaction selecting it are durable. SQLite journals the
metadata; Everett does not add a second custom update log. An implementation
cannot acknowledge in-memory-only payloads under this durability contract.

Small versioned merge/index continuations live in SQLite BLOBs and refer to
durable sealed ranges of `.kv` or `.index` outputs. Store exact P, byte/bit
address units, group size K, record counts, input identities and sufficient
prefix contexts. Counts of records are not byte or bit offsets. Commit a
checkpoint only after the ranges it names are durable, and preserve the prior
checkpoint until the new one has committed. Full key contexts can be large;
charge their storage and encoding work. See
[resumable checkpoints](durability.md#resumable-merge-checkpoints).

Recovery first opens the catalog under its configured, verified SQLite policy.
It reconciles stable operation IDs and uncertain attempts, validates exact
dependencies and restores owners before GC. A missing sealed object required by
a readable representation or selected checkpoint is corruption. A reserved
output may legitimately not have been created yet; a GC intent may explain an
unlinked, unrooted object. Interpret these cases through the catalog lifecycle
state, not by silently dropping missing dependencies. Neither a directory listing
nor a weak world signature establishes reachability or content integrity.

Reader-owner cleanup requires reliable session-liveness evidence. Heartbeats
alone are not deletion authority: a paused live process may still access its mapping or
open an indexed dependency later. Retire abandoned readers only after the exact
session's death is established, or conservatively retain them. Durable saves
and recovery owners do not expire with a process.

Accepted update identities and their replay horizon are catalog state. A retry
must discover the original operation's result, not apply its delta again.
Merge checkpoints recover representation work and cannot replace update
admission records. A catalog-only backup likewise omits the external objects;
a complete backup must retain and transfer a consistent dependency closure.

## Verification still required for the writer

- Short writes, interruptions, disk-full errors, close errors and failure at
  each synchronization/rename boundary.
- Process termination at publication cuts, uncertain catalog commit outcomes,
  operation retry and recovery from both possible outcomes of a failed barrier.
- Missing dependencies, corrupted bodies, invalid policy metadata, stale merge
  cursors and noncanonical bit padding.
- Reader lifetime and reclamation races, abandoned builders, retained snapshots
  and adoption of somebody else's completed merge; catalog acquisition must
  beat a GC claim or reject cleanly.
- SQLite transaction errors, concurrent checkpointing, reader-session death,
  stale liveness observations and directory synchronization failures.
- Persistent recovery compared with an independent logical-world oracle.

Parser and mapping tests are useful now. Protocol-model tests establish ordering
decisions. Neither is a substitute for the future filesystem fault-injection and
power-loss validation of the chosen backend.
