Rebuilding replacement tables
=============================

`replacement_rebuild_engine` maintains a typed replacement table while removing
history that is no longer needed by its current state. It uses the
[redundant runtime](redundant-runtime.md), a
[resolved scan](typed-scan.md), and a private candidate built by real typed
contributions. Existing snapshots keep their own dependencies.

```cpp

import everett;

everett::replacement_rebuild_engine<> table;
auto first = table.contribute(decltype(table)::put("name", "Edward"));
auto empty = table.contribute(decltype(table)::erase("name"));
while (table.pending()) table.advance(4096);
```

The initial interface supports one occupied replacement sort. Key transport follows
the selected runtime family, including the sort-owned native format.
The default is the byte-oriented optional-string table under `storage_policy<>`.
Use `replacement_rebuild_engine<string_policy>` for the bit policy.
A custom sort must have the same state
and arrow type, declare replacement semantics, and provide
`clean(key, state)` to encode a resolved state as a replacement arrow. I check
that applying this arrow to the initial state reproduces the scanned state.
Type equality alone does not supply that semantic law.

The executor also satisfies the `session` and `persistent_engine` contracts. The
ordinary named connection selects it when the registry contains only the
optional-string sort:

```cpp
#include <optional>
#include <string>

import everett.sqlite;

auto table = everett::connect(existing_directory, "earth-616");
table.put("name", "Edward");
auto saved = table.snapshot();
table.save("before-edit", saved);
```

An explicit bit-profile connection selects `streaming_sort_runtime_family<P>`
from `<everett/sort_runtime_context.h>` as its `Family`. The connection opens
a catalog-bound storage context and shares it between the foreground and every
large cleanup candidate. Completed native and fractional-index outputs are
sealed and mapped. Bounded small cleanups use the in-memory construction below
and retain the same storage context for their final handoff.

`from_snapshot(state, storage)` and `from_clean(state, storage)` accept that
same concrete context when scheduling directly. `storage()` returns a copy of
the context owner. A settled `rebase(state)` substitutes an admitted equivalent
mapped graph without replacing its context or generation counters. Candidate
handoff, active recovery and later native merges keep that owner. The empty
candidate seed and its context-aware restoration both receive initialization
charges within the setup allowance; neither decodes the frozen table.

File or seal-record failures poison both private executors and their shared
context. Public `poison()` provides the same boundary for an outer publication
failure. The previous acknowledged snapshot stays readable; continuing requires
a fresh context restored from durable state. Uncertain private outputs retain
their attempt identities for catalog reconciliation. Cleanup does not reclaim
those files or resume their partially written payloads.

The current runtime and generation counters are durable. Private scan cursors,
candidate objects and FIFO replay queues are not. Reopening an active rebuild
starts a new funded cleanup from the latest acknowledged state, with writes
gated until that cleanup finishes. It does not pretend to resume paid private
progress.

Generations and admission
-------------------------

A generation records its clean-base cardinality b and its subsequent mutation
count u. Native admission mass is exactly b+u, including overwritten bindings
and tombstones. Logical live cardinality is a separate quantity.

For the ordinary optional-string sort, inserting a new key into a clean
generation extends $b$ directly. Both the live count and admission mass increase
by one, so there is no obsolete record to collect. This still pays ordinary
merge service and increments the total mutation counter in `work()`. Once
$u$ is nonzero, even fresh-key insertions count toward $u$ and fund the existing
cleanup obligation. Custom sorts retain their explicit cleaning path.

For a large generation I start a rebuild at the first mutation reaching
$u=\lfloor b/4\rfloor$. The frozen snapshot contains $n_s$ live rows. The
handoff horizon is $h=\lfloor n_s/8\rfloor$ subsequent mutations. New writes
continue to update the foreground table and enter a FIFO replay queue. That
queue owns decoded keys and old/new values, rather than one entire foreground
snapshot per mutation.

Small generations use eager cleanup below 64 live bindings. Identical-value
writes still count as mutations when admitted. Deleting an absent key is
rejected. Deduplicating external deliveries remains the responsibility of the
operation-replay layer; two distinct contribution calls are two mutations.

Batches are staged per key internally. A large batch cannot skip a trigger or
consume only one scheduling unit. The public snapshot changes only after all
staged keys and their required rebuilding service succeed. If execution fails,
the handle is poisoned and the public snapshot remains at the previous
complete batch. Private source pins stay owned for inspection and cleanup.

Building and replay
-------------------

The worker scans the whole frozen logical table. It emits each live resolved
row once, omits winning tombstones, and builds a candidate whose admission mass
is exactly $n_s$. It then applies queued mutations in their original order,
checking the candidate's old and new value at every replay step.

Before handoff, the candidate's remaining native/index jobs finish, the queue
is empty, and its signature, live count and schema equal the foreground's.
The candidate admission mass must be $n_s+r$, where r is the replay count.
The new generation receives $b'=n_s$ and $u'=r$; replay history is not erased by
resetting a counter. The new graph owns its own native/index closure, without
routing through the frozen generation.

The count and fingerprint checks supplement the coverage-complete scan and
ordered replay. A fingerprint is not a proof of equality or authentication.

Small streamed cleanups
----------------------

For an eager cleanup with at most 64 live rows and at most 256 physical input
occurrences, I build the candidate with the corresponding owning runtime. Its
intermediate singleton admissions and merges create no durable files. This
also applies to an active recovery that satisfies both bounds. Larger frozen
states keep the incremental streamed path.

The owning candidate still performs the full resolved scan, validates each
clean arrow, checks the scan count and table fingerprint, and finishes its
ordinary scheduler work. I then translate its settled frontier to the streamed
family, preserving admission intervals, object identities, slots, routes and
visibility history. Native owners are shared; only the final fractional indexes
are rebuilt for the new node type. The translated frontier passes the ordinary
checked restore before handoff. Publication seals this final graph once.

This deliberately preserves the scheduler's carrier history. A sparse binary
decomposition alone would skip levels that its routing invariants require.
There is no padding of the clean admission mass: it remains exactly the live
row count. The owning construction is bounded in records, not bytes; large keys
or values can still require substantial memory.

The whole tiny cleanup is one funded structural action. A separate conversion
allowance covers its final indexes and metadata. With at most seven levels,
the slots, carriers and prepared query head use fewer than 64 distinct pairs.
Each contains at most 132 occurrences for sampling factors at least three.
`work().tiny_generations`, `tiny_indexes` and `tiny_conversion_charged` expose
this path; conversion charges are also included in `candidate_charged`.

Structural reservations
-----------------------

`status()` exposes the current generation, horizon, replay progress, initial
bound, action allowance and committed allowance. `work()` distinguishes reserved
bounds, granted budgets, committed action allowances, actual runtime charges,
physical records scanned and rows written. A reservation is not reported as
executed merge work.

The scan's physical input count and run count come from immutable metadata.
Its initial heap and first-record loads receive a setup allowance. Each scan
step consumes at most one physical record; a reconstructed output row remains
held until its candidate admission can run.

For a frozen live count $n_s$, I reserve against the largest possible candidate
admission mass $L=n_s+h$. The bound includes:

- the physical scan, resolution and cursor/heap setup;
- all clean candidate admissions and their index maintenance;
- a conservative local-job bound through L, including a carry first triggered
  during replay;
- finite fragments of nested service offers at settlement;
- the FIFO replay admission allowance and final metadata checks;
- an explicit $hG$ margin for indivisible actions of allowance at most G.

After each intervening mutation the worker receives
$\lceil R/h\rceil+G$, where R includes that margin. It first builds the base,
then services FIFO replay and remaining candidate maintenance. Every action is
funded before it runs, with unused allowance carried to the next call. The
executor checks the reserved-work limit and refuses to silently exceed the
handoff horizon. Tests check the funded remainder after every mutation:

$$
\max(0,R+iG-\text{committed}-\text{credit})
\le \max(0,R-i\lceil R/h\rceil).
$$

The credit term matters: an atomic finalizer can have funded work waiting to
run. The physical-scan count must equal the frozen input count before replay
starts, including obsolete occurrences and tombstones.

These are conservative structural allowances. They do not bound key/value
bytes, string comparisons, hashing callbacks, allocator latency or elapsed
time. Existing EF finalization and a ready typed contribution remain atomic;
their structural work is prepaid. The separate byte-accounting requirement in
[Strong deletion](rebuild.md) still applies.

Static session quotes
-----------------

A session needs a quote before it owns the mutable executor. `reservation(input)`
therefore uses a conservative, state-independent ceiling per admitted record,
plus the underlying typed engine's allowance. It counts every record in a
batch, including unchanged replacements. The byte quote is the encoded input
size; it does not describe the replay queue, retained snapshots or executor's
working set.

Let C be the redundant runtime's local charge bound, H the supported admission
height (at most 64), and D=H+3. The candidate action allowance is

$$
G(H)=2C+16D+512+8C(H+2)+64(D+1)(K+W+16)+8H+16.
$$

There are at most 128 visible native runs. I use S=32 for each physical scan
action and T=128(S+8)+32 for setup. At a normal large trigger, the frozen
physical count M is no greater than the admission mass b+floor(b/4), while
$n_s\ge b-\lfloor b/4\rfloor$. Thus M is at most 5n_s/3. For
$h=\lfloor n_s/8\rfloor$ and $n_s\ge64$, we have $n_s/h<9$, M/h<15 and
$(n_s+h)/h<10$. Substituting these bounds into R/h, including both the hG
fragmentation margin and replay's additional G, gives this safe ceiling:

$$
22G(H)+10(C+32)(H+1)+T+26S+2.
$$

The eager small case has at most 64 output rows and, for an admitted valid
generation, fewer than 256 physical input occurrences. Its bound is
$T+321S+130G(7)+512(C+32)$. The quote takes the larger ceiling, adds the
freeze allowance 1056 and a depth-limited preflight query allowance, then adds
the foreground typed engine quote. The default policy's resulting single-record
quote is 40,164,546 structural units for the owning family. A streamed family
that enables small owning construction adds its explicit conversion allowance;
with `string_policy` the quote is 40,361,922 units. `reservation_work(n)`
prices $n$ records with the same arithmetic used by `reservation(input)`.
The ordinary connection's omitted work limit is its selected engine's quote
for 1024 records. An explicit caller limit is never raised to fit a
batch. Arithmetic is checked. Restored generations
must pass the same mass/trigger validation that justifies these ratios.

These deliberately generous quotes are separate from `work()`'s actual runtime
charges and committed action allowances. They bound structural admission work,
not string bytes, arbitrary sort callbacks, storage I/O or elapsed time. Recovery
debt is serviced through the readiness gate and is not silently charged as a
constant-cost new write. Tests compare each normal admission's granted rebuild
allowance plus foreground charges against its static quote, including overwrite
and all-delete sequences.

Restore and failure boundaries
------------------------------

`replacement_world` retains the typed snapshot plus `replacement_metadata`:
the signature, live count and schema, followed logically by b, u and the active
rebuild marker. Its semantic encoding has the `EVRT.RB` signature with a zero
terminator, a separate version 1 word, b, u and flags, then the existing typed
metadata. This is a checkpoint extension; native and index file formats do not
change. Plain typed semantic checkpoints are not automatically converted.

`restore(runtime, metadata, schema)` checks b+u against the admission mass,
the possible live-count interval, and the generation's trigger range. Inactive
metadata already past its rebuild trigger is rejected. This is shape and
consistency validation, not a payload scan or authentication.

`from_snapshot(snapshot)` preserves b and u exactly. For an active marker, it
freezes the latest published table and gates new admissions. `advance` first
services restored native jobs, then performs a new whole-table cleanup. It
keeps the old generation metadata until the candidate is complete and equal
to the published table; only then does it publish b'=N, u'=0 and clear the
marker. A second interruption repeats this recovery, charging the repeated
work. Reads, saved states and forks remain available during recovery.

The restart cost is explicit existing debt, serviced before a session claims a
queued input. There is no finite work guarantee under infinitely repeated
interruptions. Lost private candidate files are not claimed as resumable work;
the next recovery restarts that candidate from its acknowledged source.

`from_clean(snapshot)` is a separate convenience for a plain typed snapshot
whose native admission mass equals its live cardinality. It does not guess a
clean base from a dirty table's current live count. A restored native job must
receive `advance` service until `admission_ready()` before another contribution
is accepted. No arbitrary recovery drain is hidden inside a new contribution.

The persistent adapter atomically publishes the runtime and generation semantic
payload. Saves and forks retain that exact pair. Metadata changes are publication
changes even when `same_layout` reports the same physical graph.

`advance(0)` performs no work. Positive idle service can finish a rebuild even
if writes stop. A moved-from or failed active handle rejects mutation; snapshots
already returned remain immutable and queryable.

Contact Information
-------------------

Questions and patches are welcome at <ekmett@gmail.com>.

-Edward Kmett
