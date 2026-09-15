Rebuilding replacement tables
=============================

`replacement_rebuild_engine` maintains a typed replacement table while removing
history that is no longer needed by its current state. It uses the
[redundant runtime](redundant-runtime.md), a
[resolved scan](typed-scan.md), and a private candidate built by real typed
contributions. Existing snapshots keep their own dependencies.

```cpp
#include <diet/replacement_rebuild.h>

diet::replacement_rebuild_engine<> table;
auto first = table.contribute(decltype(table)::put("name", "Edward"));
auto empty = table.contribute(decltype(table)::erase("name"));
while (table.pending()) table.advance(4096);
```

The initial interface supports one occupied replacement sort. Key transport follows
the selected runtime family, including the opt-in sort-owned native format.
The default is the bit-oriented optional-string table. A custom sort must have the same state
and arrow type, declare replacement semantics, and provide
`clean(key, state)` to encode a resolved state as a replacement arrow. I check
that applying this arrow to the initial state reproduces the scanned state.
Type equality alone does not supply that semantic law.

This executor is synchronous and uses in-memory continuations. It does not
persist the scan cursor, replay queue or rebuilding generation. There is no
adapter to an asynchronous tap or durable named connection in this interface.

Generations and admission
-------------------------

A generation records its clean-base cardinality b and its subsequent mutation
count u. Native admission mass is exactly b+u, including overwritten bindings
and tombstones. Logical live cardinality is a separate quantity.

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

Restore and failure boundaries
------------------------------

`from_clean(snapshot)` accepts a starting state only when native admission mass
equals live cardinality. It does not guess a clean base from a dirty table's
current live count. A restored native job must receive `advance` service until
`admission_ready()` before another contribution is accepted; the wrapper does
not hide an arbitrary recovery drain inside that contribution. Restoring an unfinished rebuild will require the complete
generation and replay metadata described in the strong-deletion design.

`advance(0)` performs no work. Positive idle service can finish a rebuild even
if writes stop. A moved-from or failed active handle rejects mutation; snapshots
already returned remain immutable and queryable.

Contact Information
-------------------

Questions and patches are welcome at <ekmett@gmail.com>.

-Edward Kmett
