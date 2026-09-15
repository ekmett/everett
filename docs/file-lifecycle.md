# Files, mappings and publication

Updated 2026-09-15. SQLite manages catalog metadata; bulk data lives in two
custom file kinds, `.kv` and `.index`. We have read-only mapping, checked object
envelopes, typed codecs and an immutable object writer as implemented foundations.
Portable codec sections, exact mapped chains and an optional SQLite catalog
support saves and conditional timeline publication. The recovery executor
remains work. See
[implementation status](implementation.md), the [catalog design](catalog.md)
and the [failure and resumption protocol](durability.md).

## Objects and names

| Extension | Intended encoded contents | Exact dependencies recorded in the catalog |
| --- | --- | --- |
| `.kv` | Immutable ordinary-FC native keys and values, absolute retained counts at W-spaced block starts, final key length and native sampled offsets | Any objects required by the value/arrow representation |
| `.index` | Ordinary-FC borrowed keys with the same block framing, sampled offsets, K-spaced origin ranks, exact bit-LCP counts and false-borrow flags | Its native source and exact downstream blob/index versions |

Separating a fractional index from its native file lets several index versions
share the same native bytes. We select compatible objects, their
precedence and additive contributions through SQLite's immutable representation
rows. A logical cola can have several representations, and a saved branch point
retains one exact representation. Jobs and checkpoints also live in SQLite;
their owners retain unfinished work without adding its fingerprint a second time.

Here **manifest** means a representation recorded in the catalog, and **root
publication** means an atomic catalog transaction selecting it. SQLite owns the
database and journaling files; Diet adds no custom manifest file, root-selector
file or metadata journal.

### Path spelling and content identity

Within an object directory, a canonical 128-bit physical identity is spelled as
32 lowercase hexadecimal digits, split without repeating its prefix:

```text
objects/ab/cd/ef0123456789abcdef0123456789ab.kv
objects/ab/cd/ef0123456789abcdef0123456789ab.index
```

The example shows the implemented path spelling for two object kinds; related
objects need not receive the same identity. The current codec accepts an opaque
128-bit ID. It does not compute a content hash or prove uniqueness. Allocation
must prevent ID reuse, check collisions and distribute the shard prefixes.

The sharing contract calls for content-addressed native files. Such an address
must identify verified encoded bytes under a specified hash algorithm and
format; the weak fingerprint of the resolved logical contents cannot do that
job. Different merge or codec layouts can represent the same cola and have
different content addresses. We reserve a private construction ID before knowing
the final content, then establish its final content descriptor and name when
sealing it, before publication. To reuse an existing candidate, we must verify
byte identity and retain an owner.

I have not yet chosen or implemented the content-ID algorithm, digest width and
canonical hash input. We must bind kind, interpretation metadata, meaningful bit
extent and payload in the preimage, with an explicit rule for derived fields.
Including the address itself would make the hash self-referential. A future
digest may require a wider path format than today's 128-bit primitive. Neither
CRC32C nor the algebraic cola signature supplies this content identity.

Create shard directories as needed. Sharding bounds the entries in each
directory, while leaving the total inode count unchanged. We can eventually pack
physical objects with a managed-extent allocator while retaining the same
immutable-object interface.

## Object envelope

The portable envelope has a 96-byte explicitly little-endian header.
We encode its fields individually, so the representation does not depend on a
C++ struct's memory layout. The header binds kind, format version, length, policy and
integrity metadata. Kind-specific magic is:

| Kind | Eight magic bytes |
| --- | --- |
| Native | `DIET.KV` followed by zero |
| Fractional index | `DIET.IX` followed by zero |

Only these signatures identify Diet objects; checked readers reject every
other magic value, even when the rest of the header and its CRC are valid.

The extension helps people; the header establishes the object's actual kind.
By default, `file<P>::open` and `from_slice` check the 96-byte header and exact file extent:
magic, version, reserved fields, policy unit, K, W, fixed-width descriptor,
backspace code and parameter, and header CRC32C. They do not read body pages or
inspect the final padding byte.
CRC32C detects accidental corruption; it is not authentication and is separate
from the algebraic cola fingerprint.

When an object is already trusted, `file_open_mode::trusted` skips all header
reads during `open` or `from_slice`. It also skips the filename/header kind
comparison. The only envelope check at this point is that the physical mapping
has at least 96 bytes, so slicing off the header remains well-defined.
`fridge<P>::open_object` forwards the same option without inspecting the
header. This is useful when the caller already knows the type and policy and
wants to avoid faulting in the header page on every open.

`body()` obtains the physical slice after the header without reading either
region. For bit profiles this includes the final storage byte and its padding.
An explicit `header()` call returns metadata by value; a trusted handle then
reads and validates the header and exact extent. There is no mutable lazy cache
shared between readers. `scan()` validates the header, extent and entire body,
including for trusted handles. It validates the object bytes rather than its
external filename.

Opening should not scan a large immutable file just to make its bytes addressable.
An explicit `file<P>::scan()` checks the header, body's CRC32C and canonical bit padding,
with O(physical body bytes) work. `validate_file` remains the whole-object
validation helper. Recovery selects the objects that need such a scan, for
example uncertain outputs; it does not automatically scan the whole database on
every restart. A scrub can request the same operation.

The header check establishes interpretation and bounds, not payload integrity.
`object_writer<P>` computes the checksum while streaming borrowed chunks,
without reopening them for a second pass. The [sealing protocol](object-writer.md)
specifies exclusive construction, no-clobber installation and OS barriers.
Checked partial reads will need a separate page/block integrity format, which
is not implemented yet.

`file<P>` must reject persisted metadata inconsistent with `P`. Byte profiles
measure their body extent in bytes. Bit profiles measure meaningful bits, with
MSB-first bytes and zero trailing padding. The physical mapping still has a byte
length. Conversion and overflow checks occur at that boundary; padding does not
increase the logical bit universe.

The backspace descriptor uses header byte 18 for the code and bytes 88–95 for
its little-endian parameter: code 0 selects exponential-Golomb with an order
from 0 through 63; code 1 selects Golomb with a positive modulus. Byte profiles
require descriptor `(0, 0)` and use varints. The reader checks the descriptor
against `P` before interpreting the body. Byte 19 remains reserved zero. Bytes
20–23 encode physical block width W as an unsigned little-endian 32-bit count;
zero is invalid, and the reader checks it against `P::codec_block_size`. Virtual
sampling interval K is checked separately. Both counts describe records, not
byte or bit lengths.

The policy's fixed-value descriptor and a stream's actual common width are
different metadata. A borrowed-key stream has no value payload, even when the
shared policy says native values have fixed width. For a native stream with
common width `v`, sampled residual positions remove `ordinal * v`; add the same
stride back when locating the record. A width constant only within each sort
does not suffice for a single shared stride. Ordinary FC selects key prefixes
from adjacent keys, independently of value widths. The residual universe still
includes variable key data and physical block framing.

The generic envelope accepts arbitrary bodies. The [mapped blob format](mapped-blobs.md)
encodes native FC, borrowed FC, rank, Elias–Fano, false-borrow flags and exact
cut LCPs in checked, versioned sections. The IX02 directory records the native
identity and one exact downstream pair. The [IX03 COLA directory](cola-indexes.md)
records a main pair and a terminal secondary native identity, with separate
borrowed streams for those routes. Both use the same KV02 native layout.
Section descriptors count physical bytes; the inner FC extent and residual
offsets retain the byte/bit policy units.
Cola manifests and merge continuations belong in SQLite rows and versioned BLOBs.

## Mapped lifetime

We keep published files immutable and fixed in length, and map them read-only. A
`mapped_slice` keeps its mapping alive, so a view cannot accidentally outlive the
mapped object merely because the opener closed its handle. Checked subranges
use lengths and relative offsets, not unchecked pointer arithmetic. Empty
files need no zero-length mapping. Never truncate, overwrite or recycle a file
while a reader can still reach it.

The [mapped blob views](mapped-blobs.md) navigate ordinary-FC, cut-LCP, rank
and Elias–Fano sections directly from their mappings.
Opaque whole-file compression would require another decompressed allocation
before those structures can be used. LevelDB similarly avoids a second cached
copy when an uncompressed block already resides in stable mapped memory; its
compressed block paths allocate decoded storage.
[LevelDB block reader](https://github.com/google/leveldb/blob/main/table/format.cc)

LevelDB's POSIX environment limits mappings and open descriptors and can fall
back to positional reads. Its process-local lock table supplements operating
system locking. We likewise need explicit mapping/descriptor budgets.
With SQLite, short catalog transactions arbitrate publication and reclamation;
workers build private generations outside those transactions.
Any implementation using an additional coordinator must prevent duplicate
ownership, including repeated opens within one process.
[LevelDB POSIX environment](https://github.com/google/leveldb/blob/main/util/env_posix.cc)

We need both mapping lifetime and catalog pins. The former keeps already mapped
bytes addressable; the latter prevents removal before we open a later dependency.
We acquire the exact representation and its dependency closure through the
catalog's reader-owner transaction before opening files, and retain that owner
until all views and deferred opens are finished. A SQLite read transaction alone
does not pin external objects. Acquisition and GC claims must serialize; an
object already claimed for deletion cannot be reacquired.
See [reader acquisition and liveness](catalog.md#6-reader-acquisition-and-garbage-collection).

## What to retain from LevelDB's lifecycle

LevelDB separates immutable tables from the metadata selecting them. Table
construction finishes and synchronizes the file before offering it to the
version update. Metadata installation appends and synchronizes a version edit;
a newly created manifest also needs root selection before the in-memory
version is installed. These publication boundaries apply independently of the
compaction schedule we choose for Diet.
[Table builder](https://github.com/google/leveldb/blob/main/db/builder.cc),
[version installation](https://github.com/google/leveldb/blob/main/db/version_set.cc)

LevelDB's obsolete-file collector includes pending outputs and all retained
versions in its live set. It stops collection after a background error, because
publication may be uncertain. We need the same protection for builders, together
with explicit saves, old fractional targets and shared merge candidates.
Completing our merge does not release another owner's old index dependency.
[Live files and pending outputs](https://github.com/google/leveldb/blob/main/db/db_impl.cc)

Diet's catalog records exact dependency edges and explicit owners. Runtime
reference counts can cache that graph, but recovery must reconstruct its durable
roots. The LevelDB examples supply useful publication and retention rules;
SQLite supplies our metadata transactions and recovery machinery.

## Publication protocol

Publish a new generation in this order:

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

Keeping the recovery owner through step 4 preserves both possible outcomes if
the selecting commit becomes uncertain. We do long writes,
checksums and file synchronization outside the catalog write transaction. Exact
SQL relationships, retry identities and transitions are in [catalog.md](catalog.md).

File synchronization and directory-entry persistence are separate obligations.
SQLite's commit cannot flush Diet's external outputs for us. The backend
must specify its file, directory and device barriers, including initial catalog
creation. Platform details and failed-`fsync` handling are in
[durability.md](durability.md).

An error after we attempt publication does not prove rollback. We preserve old
roots and candidates, stop retirement/reclamation in the affected domain, and
reconcile the catalog operation outcome and external bytes. A later successful
flush alone does not prove that we have regenerated failed writeback data.

## Acknowledgment, checkpoints and recovery

A durable acknowledgment requires both the complete immutable payload and the
catalog transaction selecting it to be durable. SQLite journals the metadata;
there is no second custom update log. We cannot acknowledge
in-memory-only payloads under this durability contract.

The checkpoint design places versioned merge/index continuations in SQLite BLOBs referring to
durable sealed ranges of `.kv` or `.index` outputs. We store exact P, byte/bit
address units, virtual interval K, physical width W, record counts, input
identities and sufficient coding/comparison contexts. A query comparison binds
its exact query, bit agreement, optional full key length and direction. A
construction cursor instead needs the actual key context for its current
position; a merge input ordinal names its next unconsumed record.
Index continuations retain their cut ordinal, borrowed rank and cut-LCP state,
while physical continuations retain predecessor and terminal length accounting.
Counts of records are not byte or bit offsets. Commit a
checkpoint only after the ranges it names are durable, and preserve the prior
checkpoint until the new one has committed. Full key contexts can be large;
charge their storage and encoding work. See
[resumable checkpoints](durability.md#resumable-merge-checkpoints).

On recovery, we first open the catalog under its configured, verified SQLite
policy. We reconcile stable operation IDs and uncertain attempts, validate exact
dependencies and restore owners before GC. A missing sealed object required by
a readable representation or selected checkpoint is corruption. A reserved
output may legitimately not have been created yet; a GC intent may explain an
unlinked, unrooted object. Interpret these cases through the catalog lifecycle
state, not by silently dropping missing dependencies. Neither a directory listing
nor a weak cola signature establishes reachability or content integrity.

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

## Persistence verification

The object writer's tests cover short writes, interruptions, disk-full errors,
close errors and injected failure at each write/synchronization/install boundary.
They preserve uncertain private and final outputs. Full storage-backend
validation additionally needs:

- Process termination at publication cuts, uncertain catalog commit outcomes,
  operation retry and recovery from both possible outcomes of a failed barrier.
- Missing dependencies, corrupted bodies, invalid K/W or unit metadata, stale
  merge cursors and noncanonical bit padding.
- Cut LCPs inconsistent with the exact paired streams, incorrect terminal key
  lengths, and comparison contexts attached to a different query.
- Reader lifetime and reclamation races, abandoned builders, retained snapshots
  and adoption of somebody else's completed merge; catalog acquisition must
  beat a GC claim or reject cleanly.
- SQLite transaction errors, concurrent checkpointing, reader-session death,
  stale liveness observations and directory synchronization failures.
- Persistent recovery compared with an independent logical-cola oracle.

Parser, mapping and writer tests exercise current implementations, and the
protocol-model tests check publication decisions. The injected syscall failures
do not simulate every filesystem's writeback behavior or physical power loss;
those remain separate backend validation tasks.
