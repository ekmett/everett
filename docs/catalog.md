# SQLite catalog for worlds, pins and background work

Chosen architecture, 2026-09-15. A multiverse uses SQLite for its relatively small
metadata: logical worlds, exact representations, retained roots, publication
attempts and merge/index continuations. Native data and fractional indexes remain
Everett's `.kv` and `.index` files. There is no additional custom manifest,
transaction-log or checkpoint-file format; SQLite owns the catalog and its own
journaling files.

This is an architecture and acceptance contract. The SQLite adapter, schema
migrations and transactional store executor are not implemented yet. The existing
in-memory pin owner and durability state machine remain executable models.

## 1. What the catalog owns

SQLite records which exact immutable objects are reachable and which transitions
have committed. It does not contain the bulk key/value table, and a committed
catalog row does not itself make an external file durable.

Keep logical and physical identity separate:

- A **world** identifies an admitted logical cut, with its schema context, live
  count and weak composite fingerprint. The fingerprint is not a unique ID.
- A **representation** is an immutable manifest of the exact blob/index versions
  used to read that world. Pure compaction makes a new representation of the
  same world. It does not change an old representation in place.
- A **timeline** selects a current world and representation, with a monotonically
  advancing revision used for conditional publication.
- A **branch point** retains one exact representation. Readers, saved branch
  points, jobs and caches likewise have explicit retention owners.

Metadata recording that a historical world existed need not retain its files
forever. Retention comes from the designated roots below, not merely from the
continued existence of descriptive rows. A branch point intended to remain
readable must have such a root.

The catalog records the canonical policy P: byte/bit unit, group size, value
layout and interpretation versions. Opening it as `multiverse<P>` validates
that policy. Changing P is an explicit format migration. Sort and category
schema versions are pinned by each world and by the jobs interpreting it.

## 2. Proposed relational schema

The following names specify the relationships; final DDL and migrations are
implementation work. Immutable rows are inserted once and cannot be edited into
a different object, manifest, recipe or checkpoint. Mutable lifecycle rows carry
an expected revision/generation for conditional updates.

### Contents and exact layouts

| Table | Essential columns | Required relationships and constraints |
| --- | --- | --- |
| `catalog_info` | singleton ID, schema version, canonical P, catalog identity, allocator state | exactly one identity/policy; reject unsupported versions |
| `schema_contexts` | schema ID, sort/codec/hash/category resolver versions | immutable canonical descriptor; worlds and recipes refer to exact versions |
| `objects` | allocation/object ID, kind, generation, final content identity, relative path, lifecycle state, byte extent, integrity digest, optional native fingerprint | kind is `kv` or `index`; unique ID/path; generation and IDs never reused; final identity/extent are fixed at sealing |
| `object_edges` | source object, target object, dependency role | composite primary key; both ends foreign keys; exact immutable dependency graph |
| `blob_versions` | blob ID, native object, optional index object, optional target blob, native/augmented counts | native kind is `kv`; index kind is `index`; exact target and policy validation; immutable |
| `worlds` | world ID, admitted cut ID, schema ID, live count, composite fingerprint | immutable; signature has no uniqueness constraint |
| `representations` | representation ID, world ID, layout version, cached contribution sum | immutable; composite unique key `(representation_id, world_id)` |
| `representation_entries` | representation ID, position, logical factor ID, blob ID, contribution role, contribution | primary key `(representation_id, position)`; unique factor within one representation; exact semantic ordering |

Construction identities and generations are reserved before output exists.
A final content identity is established only after the bytes and its defined
hash input have been verified. The catalog distinguishes these roles; the weak
world fingerprint is never a file content address. A private construction can
acquire its final name at sealing, before durable publication. Sealed identity,
path and content descriptors are immutable.

An index's dependency edges include its own native object and the native/index
objects of its exact target blob. Additional retained value/arrow dependencies
must also be explicit edges. Registering a blob validates agreement among those
edges, the target blob and the external index metadata. Foreign keys establish
row existence; they do not establish this semantic agreement or graph acyclicity.

The initial chain representation rejects cycles and requires a target to be
fully registered before a dependent is sealed. Metadata about unfinished work
is not a visible query graph. Paths come from the checked object-name allocator;
a path field is not permission to open an arbitrary filesystem path.

### Retention roots and mutable selections

| Table | Essential columns | Required relationships and constraints |
| --- | --- | --- |
| `sessions` | session ID, boot/process-start identity, liveness state, observed heartbeat | nonreused session identity; heartbeats are diagnostic, not sole reclamation authority |
| `owners` | owner ID, kind, optional session ID, lifecycle state | kinds include timeline, branch point, reader, job, cache and recovery; durable owners do not expire with a process |
| `owner_representations` | owner ID, representation ID | composite primary key and foreign keys; retains that exact manifest |
| `owner_blobs` | owner ID, blob ID | direct roots for jobs/cached results before a world adopts them |
| `owner_objects` | owner ID, object ID | roots for unfinished outputs, sealed ranges and quarantined generations |
| `timelines` | timeline ID/name, owner ID, head world, head representation, revision | unique name; head representation belongs to head world and is retained by that owner |
| `branch_points` | branch ID/name, owner ID, world ID, representation ID | exact world/representation pair retained by that owner |

Use composite foreign keys where they can express the contract directly. For
example, `(head_representation_id, head_world_id)` references the corresponding
unique pair in `representations`, and `(owner_id, head_representation_id)`
references `owner_representations`. The latter can be deferrable during an
atomic head change. A branch point uses the equivalent constraints. Index the
reverse dependency and owner-reference columns used by reachability and cleanup.

A root retains the transitive closure of its objects. Cached refcounts may
accelerate this, but must be transactionally maintained and auditable against
the graph. They are not an independent authority to delete files.

### Operations and background work

| Table | Essential columns | Required relationships and constraints |
| --- | --- | --- |
| `operations` | operation ID, operation kind, canonical request digest, expected revision, outcome IDs, status | duplicate ID with different contents is rejected; committed outcome is discoverable after acknowledgment loss |
| `rounds` | round ID, base world/representation, partition rule version, admission/completion state | retain the exact shared read base; distinguish round completion from representation work |
| `accepted_batches` | round ID, batch ID, partition owner, exact update blob, request digest, advertised delta, accepted cut | unique `(round_id, batch_id)`; independent validation before first admission |
| `jobs` | job ID, recipe ID, owner ID, state, attempt generation, latest checkpoint, work budget/progress | stable job identity; fresh attempt/output generations; checked state transitions |
| `job_inputs` | job ID, input position, nullable blob ID, nullable representation ID, semantic role | exactly one typed input reference is present, with its own foreign key; ordered immutable recipe inputs include older-coverage context when required |
| `job_outputs` | job ID, attempt generation, object ID, output role | every output has a durable owner before construction; no generation reuse |
| `job_checkpoints` | job ID, attempt generation, checkpoint sequence, encoding version, context BLOB, work counters | immutable checkpoint rows; unique sequence within an attempt; latest pointer changes atomically |
| `checkpoint_extents` | checkpoint identity, object ID, sealed range, range units, integrity digest | ranges agree with file/profile units and already verified immutable bytes |
| `publication_attempts` | operation ID, expected old head/revision, candidate representation, recovery owner, state | retain both recovery possibilities until the outcome is established |
| `gc_work` | object ID, deletion generation, claim state, error/outcome | one current deletion claim per object; claims block new acquisition |

A request digest is a quick mismatch check. Exact canonical operation descriptors
and referenced immutable update identities define replay equivalence; a weak
fingerprint match alone cannot authorize a changed request under an old ID.

A job's input/output rows must be backed by its owner roots; a descriptive job
row alone is not a pin. Publication and checkpoint transactions validate this
coverage. Build, rebuild and reindex jobs share the mechanism but have distinct
versioned recipe/context encodings.

Avoid a SQLite row for every logical key merely to record replay bookkeeping.
An accepted batch can retain its immutable update file; a round can reconstruct
its claimed-key set from admitted files after restart, or use a separately
specified compact summary. That reconstruction is real recovery work. Completed
rounds need an explicit replay horizon or durable rejection watermark before
old batch identities and their retained update files can be retired.

### Value encodings and constraints

Object/world/job identities and algebra elements use canonical BLOB encodings
when their full range is not a checked SQLite integer range. Never truncate an
unsigned ID, reinterpret a wrapping fingerprint as a signed sum, or store an
algebra element as floating point. Checked nonnegative integer counters need
explicit overflow limits.

Use `NOT NULL`, primary/unique keys, foreign keys and `CHECK` constraints for
local invariants: valid object kinds/states, nonnegative positions, ordered
ranges, expected digest widths and matching version tags. Use transactional
validation or triggers for cross-row rules and graph/state transitions that
cannot be expressed by those constraints. Enable and verify foreign-key
enforcement on every connection before beginning transactions.
[SQLite foreign-key configuration](https://www.sqlite.org/foreignkeys.html#fk_enable).

## 3. Contributions are not retention counts

Only `representation_entries` contribute to a representation's world sum:

- An initial base contributes its logical table fingerprint.
- An update contributes its validated old-to-new delta.
- A merged entry contributes the sum of the logical factors it replaces.

The sum is computed with the pinned algebra policy and compared with the world's
fingerprint. A file's own-native-record hash is a different optional diagnostic.
A tombstone can have zero native hash and a nonzero negative contribution.

Owner roots, borrowed keys, index dependencies, cached completed merges and
unfinished candidates add **retention**, not extra terms in that sum. Retaining
both an input representation and its replacement temporarily does not represent
twice the world. Two representations with equal sums can still have different
contents through collisions or a bug; the sum is a lint check, not the merge
correctness proof.

SQL `SUM(contribution)` is not the algebra implementation. Show canonical values
with `hex(...)`; use the configured C++ algebra to audit sums, including finite
fields and wrapping arithmetic. Contributions belong to the logical manifest
entries, not to one global object refcount.

## 4. Connection and transaction policy

Use one SQLite catalog for the atomic metadata boundary. The intended initial
backend is local, WAL mode with `synchronous=FULL`, and verified foreign keys.
SQLite documents a commit synchronization in WAL/FULL; the selected VFS and
storage path must honor its barrier assumptions. Initial catalog installation
must also persist its directory name.
[SQLite synchronization modes](https://www.sqlite.org/pragma.html#pragma_synchronous).

Require a SQLite release containing the WAL-reset race fix, such as 3.51.3 or
later, and record runtime/source version diagnostics. SQLite's WAL documentation
also restricts participating processes to one host.
[SQLite WAL requirements and fix](https://www.sqlite.org/wal.html#the_wal_reset_bug).

Use short `BEGIN IMMEDIATE` transactions for root acquisition, head publication
and GC claims. SQLite permits one writer at a time; obtaining that write
transaction can report `SQLITE_BUSY`. Apply bounded retry/backpressure for
contention, not a successful acknowledgment. SQLite errors can leave different
transaction states, which the adapter must inspect explicitly.
[SQLite transaction and error semantics](https://www.sqlite.org/lang_transaction.html).

Do long encoding, checksum and file-sync work outside the catalog write lock.
A writer transaction revalidates the exact expected revision before committing
results of that work. SQL browsing can use ordinary read transactions. Neither
kind of transaction substitutes for external-object ownership.

## 5. Publish files before adopting their catalog roots

The protocol spans external objects and a transactional catalog. Its ordering
is deliberate; SQLite does not atomically flush `.kv` or `.index` files on our
behalf.

1. **Reserve and retain.** Commit a job/attempt identity, fresh object identities
   and generation-specific private paths. Its owner retains exact inputs and
   every output it is about to create; final content addressing is completed
   when the output is sealed. For publication, independently commit a recovery
   owner retaining the expected old representation and the candidate's objects.
2. **Build privately.** Write only this attempt's private objects. Published
   objects and checkpointed immutable ranges are never rewritten.
3. **Seal external state.** Complete required data/index sections and validate
   lengths, policy tags, exact dependencies and integrity digests. Synchronize
   file contents and every directory/name change needed to reopen them. Only
   then may the catalog describe an object or checkpoint range as durably sealed.
4. **Prepare exact representations.** Register sealed objects, the immutable
   dependency graph and completed blob versions. A data merge may stop here and
   remain a retained cached result while dependents rebuild their indexes.
5. **Adopt atomically.** In a write transaction, recheck the operation identity,
   expected head/revision, exact inputs, candidate readiness and semantic checks.
   Insert the immutable representation/entries, add its selected owner root,
   change the timeline's head, and record the operation outcome in one commit.
   A logical update also creates its new world/cut; pure compaction keeps the
   world ID. Remove only this timeline owner's superseded selection.
6. **Acknowledge, then retire auxiliary retention.** After successful commit,
   report the durable outcome. A later transaction can release the job/recovery
   owner's old inputs. Branch points, readers, other jobs and caches retain their
   own independent roots.

The recovery owner is intentionally not released in the head-swap transaction.
If its commit result becomes uncertain, both the old representation and candidate
remain discoverable through retention already committed before that attempt.
An acknowledgment lost after success is resolved using the operation ID, not
by applying the same changes twice.

A failed file or directory sync leaves the output quarantined and the old
recovery roots retained. An I/O failure during catalog commit also requires
reconciliation: stop retirement and GC in the affected catalog/storage domain,
establish the catalog's recovered transaction outcome, and validate the selected external
objects. Do not infer rollback solely from an error, or durability solely from a
later successful sync. Keep uncertain generations until recovery proves they
can be reclaimed. The [durability protocol](durability.md) supplies the failure
model and resumption obligations.

Creating a durable branch point from an already retained representation is a
small catalog transaction adding another owner/root. Removing it releases that
owner only. Adopting a shared merge changes just the dependent representation
whose fractional index is ready; other snapshots keep their old target graph.

## 6. Reader acquisition and garbage collection

### Acquire a real external pin

A SQLite read transaction can see an old catalog snapshot while another
connection changes retention. It does **not** pin the files named by those rows.
Returning a path from `SELECT` and registering a reader later races with unlink.

The reader protocol is:

1. Enter a short catalog write transaction under a live session identity.
2. Select the exact representation and verify that its entire object closure is
   sealed/acquirable, with no GC claim or quarantine preventing acquisition.
3. Insert the reader owner's exact representation root and commit.
4. Open/map and validate the external files. If opening fails, preserve/report
   the error and release the acquired owner through the normal path.
5. Keep that owner while any borrowed view, mapping or deferred dependency open
   can still be used. Release it after the last such use ends.

The collector uses the same write-transaction serialization for its claims.
If acquisition commits first, reachability protects the objects. If GC claims
first, acquisition rejects the claimed closure and retries from an available
representation. Do not wait until after opening files to establish protection.

Session heartbeats help identify stalled work, but a timeout is not proof that
an mmap user stopped. The first local backend should retire abandoned reader
owners only after cooperative release or verified death of that exact process
instance, distinguished from PID reuse by boot/start identity or an equivalent
kernel-backed liveness mechanism. If liveness is uncertain, keep the pins.
Distributed lease expiry would require a separate fencing protocol that also
prevents stale clients from using external objects; a lease timestamp alone
cannot provide it.

### Claim, unlink, finish

Under `BEGIN IMMEDIATE`, compute reachability from all active owner roots through
representation entries, blob versions and exact object edges. Mark eligible
unreachable objects `gc_claimed` and insert deletion intents, then commit. New
roots cannot acquire claimed objects. Cached results must use the same atomic
lookup-and-retain rule.

Unlink outside the SQLite write transaction. Synchronize the directory change,
then commit the deletion outcome and mark the object collected. Keep enough
identity history to avoid object/path reuse and to diagnose interrupted cleanup.
If unlink or synchronization fails, preserve the deletion intent and uncertainty;
recovery reconciles the exact generation rather than treating its filename as
an unrelated reusable slot.

Metadata garbage collection is separate from physical unlink. Foreign-key rows
may retain descriptions of already collected objects without making those rows
live storage roots. Remove unneeded descriptions only after their references,
replay requirements and diagnostic retention policy permit it.

## 7. Checkpoints and rebuilding progress

Store small continuation state directly as versioned SQLite BLOBs, plus indexed
columns useful for inspection. A checkpoint binds at least:

- Job/attempt/checkpoint identities, exact ordered inputs, recipe and schema P.
- Native/borrowed cursors, record ordinals, full predecessor lengths, available
  prefixes and partial encoder/comparison state.
- Offset units, meaningful-bit lengths, group size, rank/offset builder state and
  pending index jobs.
- Immutable sealed output ranges and their verified digests; unfinished tails
  are not promoted by the checkpoint.
- Logical cut/replay positions, fingerprint/live-count accumulators and reserved,
  consumed and remaining work.

External ranges become durable before the checkpoint row commits. A new
checkpoint preserves the prior checkpoint's inputs until the new continuation
is durable. Appending must not overwrite a sealed physical update unit; use
padding or fresh immutable storage as required by the file backend. A checkpoint
names ranges in existing `.kv`/`.index` generations, not another custom file
format. After uncertain writes, resumption may need a fresh output generation.

A full predecessor key can be large. Calling it a cursor BLOB does not make its
space or write cost constant. Budget actual context bytes, avoid checkpointing
full contexts after every record, and allow references to sufficient already
sealed context when the codec proves them valid. Enforce limits/backpressure
rather than hiding a large spill in an unspecified checkpoint file.

A global rebuild additionally records the frozen source, current foreground and
candidate representations, admission/replay cuts and counters b, u, n_s and h.
Replayed mutations remain part of the candidate's weak-update debt. Foreground
and candidate roots have separate contributions; summing both is not a world
fingerprint. The [rebuilding schedule](rebuild.md) remains responsible for
bounded catch-up and byte/work accounting.

## 8. Browse with existing SQL tools

The catalog is deliberately inspectable without a custom metadata decoder. For
example, open it read-only using SQLite's shell and inspect ordinary rows/views:
[SQLite CLI](https://www.sqlite.org/cli.html#opening_database_files).

```sh
sqlite3 -readonly catalog.sqlite
```

```sql
.headers on
.mode box
SELECT sqlite_version(), sqlite_source_id();

SELECT t.name, t.revision, hex(t.head_world_id) AS world,
       hex(t.head_representation_id) AS representation,
       w.live_count, hex(w.composite_fingerprint) AS fingerprint
FROM timelines AS t
JOIN worlds AS w ON w.world_id = t.head_world_id;

SELECT e.position, hex(e.blob_id) AS blob, e.contribution_role,
       hex(e.contribution) AS contribution,
       hex(b.native_object_id) AS native_object,
       hex(b.index_object_id) AS index_object,
       hex(b.target_blob_id) AS target
FROM representation_entries AS e
JOIN blob_versions AS b ON b.blob_id = e.blob_id
WHERE e.representation_id = :representation_id
ORDER BY e.position;

SELECT j.state, count(*) AS jobs,
       max(length(c.context)) AS largest_checkpoint_bytes
FROM jobs AS j
LEFT JOIN job_checkpoints AS c
  ON c.job_id = j.job_id
 AND c.attempt_generation = j.attempt_generation
 AND c.checkpoint_sequence = j.latest_checkpoint
GROUP BY j.state;

SELECT state, count(*) AS objects, sum(byte_extent) AS recorded_bytes
FROM objects
GROUP BY state;
```

The schema should also expose a `live_object_roots(owner_id, object_id)` view
expanding active direct roots and each retained representation/blob into its
native/index objects. Exact dependency closure is then easy to inspect:

```sql
WITH RECURSIVE reachable(object_id) AS (
  SELECT object_id FROM live_object_roots
  UNION
  SELECT e.target_object_id
  FROM object_edges AS e
  JOIN reachable AS r ON r.object_id = e.source_object_id
)
SELECT hex(o.object_id) AS object, o.kind, o.relative_path, o.byte_extent
FROM objects AS o
LEFT JOIN reachable AS r ON r.object_id = o.object_id
WHERE o.state = 'sealed' AND r.object_id IS NULL;
```

This last query diagnoses apparently unowned objects at its read snapshot; it
is not authority to unlink them. Actual collection rechecks and claims them in
a write transaction. Read-only SQL browsing likewise does not acquire permission
to hold external file views. CLI parameter values must be bound with the correct
ID encoding; display hex is not automatically a BLOB parameter. A complete
multiverse backup must retain and copy the referenced external objects as well;
a catalog-only copy is a metadata backup.

## 9. Acceptance before the backend is called durable

Required implementation tests include:

- Schema/version/policy mismatch, foreign-key enforcement, invalid dependencies,
  wrong file kinds and cycles; equal fingerprints with distinct world/object IDs.
- Concurrent reader acquisition versus GC claims, including a paused live reader,
  a dead process, PID reuse and an acquisition abandoned after commit.
- Logical updates, pure compaction and reindexing with old branch points still
  readable through their exact original representations.
- Lost acknowledgments, duplicate operation IDs, changed requests under the same
  ID, and durable round restart without double application.
- Failure at reservation, file write/sync, directory sync, catalog commit,
  auxiliary-pin retirement, unlink and deletion-outcome commit.
- Restart with both possible publication outcomes, orphan candidates, unfinished
  checkpoints and immutable sealed ranges; no blind sync retry or ID reuse.
- Native-record fingerprints versus contribution sums, retained alternative
  representations, tombstones and rebuild replay debt.
- Measured catalog/context size, checkpoint work and writer-lock duration;
  large keys must not conceal unbounded cursor serialization.

The selected architecture removes the need to invent a metadata journal. It
still requires these cross-file publication, ownership and recovery protocols.
