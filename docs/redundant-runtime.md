Redundant COLA execution
========================

`redundant_runtime<P, Compose>` executes the three-slot main/secondary/shadow
schedule with real native merges and fractional-index builders. I keep the
simpler [binary executor](cola-runtime.md) available as a separate backend.
Both are active encoded executors: the surrounding sort registry supplies
composition, tombstone interpretation and logical hashes. Neither executor
chooses those semantics from a storage policy alone. The typed/storage family
selector for this backend is `redundant_runtime_family<P>`. Nodes cache their
main-chain `depth()` when constructed, so a typed handle can enforce its depth
limit without traversing the graph on each operation.

This backend accepts the existing FC `profile_record` representation. Its
structural allowance is not a byte or elapsed-time budget. In particular,
existing EF finalizers execute atomically after their allowance has been
funded; a long key, value or callback also remains indivisible.

```cpp
using registry = diet::tip<diet::encoded_sort<diet::bit_encoding<>>>;
using policy = diet::storage_policy<registry>;
diet::redundant_runtime<policy> active;

auto before = active.snapshot();
auto after = active.contribute(encoded_record);
while (active.pending())
  active.advance(256);
```

`contribute` supplies the conservative structural service allowance. Extra
`advance` calls execute the remaining real work and may publish equivalent
layouts. Old snapshots remain queryable and retain their exact dependencies.

Slots and publication
---------------------

Each of at most 64 levels has three logical slots. An active run at level i
covers a contiguous admission interval of mass $2^i$. Duplicate keys may
coalesce without reducing this mass. Two active inputs are merged older first.

A local job reserves a destination at level i+1 and a lookahead-carrier slot at
level i. It then:

1. Executes the native merge and finalizes its physical offset directory.
2. Builds a destination main index, or retains the native as a terminal
   secondary beside the existing main.
3. Builds an empty-native carrier pointing to the exact destination main and
   optional secondary.
4. Marks the inputs consumed and activates the destination.

A destination does not become active merely because its native bytes are
complete. The carrier must also be ready. A later lower merge can fill a
prepared carrier using its unchanged target set. Higher completed outputs may
remain hidden until a lower publication reaches them.

Consumed objects retain their slots until they have been visible and the
current root no longer reaches them. The scheduler checks that a previous
destination became visible before its logical destination is reused. Historical
snapshots retain their own immutable owners independently of slot retirement.
There is no physical garbage collection here.

Only the main route recurses. The secondary is a native leaf. The scheduler
establishes chronological order: at a node its local native is newest, followed
by the terminal secondary, followed by the main child. A bounded synthetic
entry index includes both level-zero arrays. Replacement reads take the first
match; an associative composition folds occurrences oldest first. These are
properties of this scheduler's graph, not of arbitrary COLA graphs.

Service and admission
---------------------

A bit mask selects the smallest unsafe level without scanning every level for
each record step. Native steps consume at most two inputs and emit one output.
Index steps merge one augmented occurrence and allow for its target sampling
and navigation work. Constructors, finalizers, root preparation, slot changes
and immutable checkpoint copies receive separate charges.

For a level-i source mass B, the capacity bounds give loose limits of 2B native
output records, 12B destination augmented occurrences and 5B lookahead
occurrences for every supported $K\ge3$. This implementation uses

$$
C(P)=32(K+6)+2048
$$

as a conservative local structural ceiling per B. The additive room covers
setup, fixed-width scheduler bookkeeping and level-zero root preparation.
Each completed job checks its charged work against this bound. Automatic
checkpoint copies are charged separately. The bound concerns fixed-size
structural events; it does not charge the contents of a key as one byte.

With h the bit length of the admission count, the service allowance is
$8C(P)(h+2)$. A normal contribution offers this allowance after admission. If
prior explicitly under-serviced work blocks admission, the convenience method
first offers one allowance to that work. It does not drain an arbitrary job
without a limit. A still-blocked contribution is rejected before its record is
admitted; the existing logical state remains available.

The allowance is intentionally conservative. `work().granted` records offered
allowance; `work().charged` records operations actually attempted. They are not
the same quantity. Unused allowance is discarded once all actual work settles.
Native input/output and index-occurrence counters report successful worker
steps separately.

The admission gate is part of the schedule. `try_contribute(record, 0)` can
leave real jobs and an outstanding service obligation. Another record is
refused until that obligation has been offered, or the actual jobs finish.
`service_due()` exposes the remaining obligation. It is not a fake worker:
reducing it does not count as executed merge work. `advance` also pays the
actual worker actions and only performs an action whose allowance is funded.
`advance(0)` changes nothing.

This gives an explicit logarithmic allowance policy and checked local cost
ceilings. The schedule tests exercise slot deadlines, visibility and ownership
closure against independent traversals. A universal proof of the immutable
visibility-before-reuse schedule remains separate from those executable
checks. Atomic finalization means that a bound on offered structural service
must not be read as a hard per-call latency bound.

Immutable checkpoints and restart
---------------------------------

`snapshot()` is a cheap immutable handle. The runtime automatically captures a
new whole-frontier checkpoint at visible publication and when service settles.
While a private worker advances without publication, `snapshot()` may still
return the previous equivalent checkpoint.

`checkpoint()` explicitly captures the current frontier using metadata only.
It charges for the copy and does not scan keys, finish an index, or share mutable
cursors. Its frontier includes:

- all three slots at each level, their roles and visibility history;
- exact native/main pairs, admission intervals and downstream routes;
- hidden completed outputs and prepared carriers;
- each active job's source/destination reservations and completed artifacts;
- the next local identity and outstanding service obligation.

The prepared query root is also retained. A persistence adapter must save it
and the full frontier closure; retaining only objects reachable by queries
would lose hidden outputs and private plans.

```cpp
auto checkpoint = active.checkpoint();
auto restored = diet::redundant_snapshot<policy>::restore(
  checkpoint.frontier(), checkpoint.query_root().head());
auto fork = diet::redundant_runtime<policy>::from_snapshot(restored);
```

`restore` validates metadata, exact target relationships, slot roles, interval
coverage and the prepared root's attachment to the frontier. Payload admission
or explicit trust remains a separate requirement. A loader interns immutable
identities before constructing mapped facades with
`redundant_node::from_mapped_parts`.

For a validated snapshot, every object occupies exactly one level slot, and
its object routes lead to the next level. The checkpoint codec visits levels
from large to small and slots in their fixed order. This gives a complete,
children-before-parents object stream without a temporary DFS vector or hash
sets. Hidden completed job outputs already occupy their reserved destination
slots; separate native artifacts and carrier pairs are still retained explicitly.
The metadata itself remains $O(\log U)$ and must still be written.

`runtime_storage_codec::for_each_object(snapshot, visitor)` and
`object_count(snapshot)` expose that allocation-free traversal. The raw-frontier
`objects(frontier)` helper remains defensive. Decoding still checks every
reference and runs the full restore validator; only then can the slot count
reject an encoded descriptor that belongs to no slot. Earlier checkpoints with
DFS-ordered descriptors remain readable. New encodings use the deterministic
level/slot order, so an operation replay retains its original request bytes.

A restart recipe owns complete artifacts, never a partially written stream.
A restarted executor rebuilds only its unfinished stage and inherits no service
credit. While replay is needed it imposes a recovery barrier: queries work, but
new admissions wait for recovery service. That rework is not silently charged
as a constant part of the next input. Durable partial-cursor continuation and
catalog publication are responsibilities of the persistence adapter.

Execution failures poison the active handle while retaining the last published
snapshot and private source owners. A new executor can recover from an immutable
checkpoint. Preflight rejection and a refused `try_contribute` do not poison or
mutate the handle.

Contact Information
-------------------

Questions and patches are welcome at <ekmett@gmail.com>.

-Edward Kmett
