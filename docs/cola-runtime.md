Charged encoded COLA execution
==============================

`cola_runtime<P, Compose>` is a low-level active executor over the existing
encoded FC records. I supply `Compose` from the active sort registry; the
immutable cola does not choose value semantics. This binary executor transports
opaque encoded keys and arrows. The [sort-owned runtime](sort-runtime.md) uses
each sort's physical grammar, and [runtime persistence](runtime-store.md)
publishes the selected executor's complete frontier.

A contribution creates a queryable immutable snapshot and pays for structural
work. A private queue then merges native runs, builds the replacement fractional
index, and constructs empty routing carriers as needed. `advance` performs that
work and can return a different physical representation of the same logical
state. It does not merely decrease a debt counter.

```cpp
using bits = diet::tip<diet::encoded_sort<diet::bit_encoding<>>>;
using policy = diet::storage_policy<bits>;
diet::cola_runtime<policy> active;

auto first = active.contribute(record, 0); // leave eligible cleanup queued
while (active.pending())
  active.advance(128);
auto equivalent = active.snapshot();
```

Here `record` is an already encoded `profile_record`. The default merge policy
replaces an older value with the newer one. A custom associative policy receives
older and newer values in that order, optionally with the key as its first
argument. Commutativity is unnecessary. Its encoded result must satisfy the
policy's actual value framing. Registry dispatch, tombstone semantics and
logical hashes belong to the active handler around this executor.

Snapshots and chronology
------------------------

A snapshot owns its query graph and all native/index dependencies. It contains
no mutable job, credit balance or callback. Holding a snapshot while continuing
or destroying its executor is safe. `from_snapshot(snapshot, compose)` starts
an independent executor; it does not share a continuation or duplicate existing
service credit.

The runtime's graph contains only main routes. Each actual native run covers a
contiguous admission interval `[first,last)`. Runs are returned oldest first by
`snapshot.runs()`, while query matches in this particular graph are newest
first. The general `cola_query_cursor` does not promise chronology for arbitrary
COLA graphs; the runtime establishes it through these intervals and routes.
Replacement reads take the first occurrence. Composition reads fold all
occurrences from oldest to newest.

Admission mass counts contributions, including duplicate keys that subsequently
coalesce. A settled frontier has strictly decreasing power-of-two masses. An
unsettled visible frontier ends with two mass-one runs. Their private carry can
merge further equal-mass predecessors before publishing a complete replacement.
No partially built native or fractional index becomes visible.

Each published query root fits one K-window. After a carry, empty carriers
sample the completed result until it again fits that bound. When selecting a
carry's older dependency, the executor may remove empty routing ancestors; it
never skips an older native run.

Admission and service
---------------------

The first backend permits one unsettled admission at a time. The convenience
`contribute` finishes any prior carry before admitting another record. This is
conservative synchronous backpressure: a contribution can pay substantial old
work. It is **not** the three-slot schedule's worst-case logarithmic update
bound.

A caller that owns a separate debt queue uses:

- `admission_ready()` to test whether a new record can be admitted;
- `try_contribute(record, budget)` to refuse without mutation while pending;
- `admission_cost()` for the next ready singleton's structural charge, excluding
  optional service;
- `next_service_cost()` for the next indivisible service action's full charge;
- `advance(budget)` to add service credit and execute funded actions.

`advance(0)` changes nothing. Insufficient credit remains available for a later
call. Publishing the completed carry discards excess credit; a new admission or
fork does not inherit it. Calls with different budget partitions perform the
same sequence of structural operations. A caller reserving admission resources
must not quote old pending work as a constant part of a new record's allowance.

A batch is checked for nondecreasing encoded keys before execution. Equal keys
remain separate chronological admissions. Each record receives the supplied
service budget; previous pending work is drained between records. An execution
failure restores the batch's previous visible snapshot and poisons the active
handle. Invalid input rejected during preflight leaves the handle usable.

What is charged
---------------

`work()` distinguishes cumulative granted credit, consumed structural charges,
and successful record/merge/index counters. Charges cover:

| Operation | Structural allowance |
|---|---:|
| Native merge initialization | 3 |
| One native merge output step | 3: at most two consumed inputs and one output |
| Native finalization | one per EF sample, plus output ownership setup |
| Fractional-index initialization | 5 |
| One augmented occurrence | 1 occurrence + K target-scan allowance + 5 navigation allowance |
| Index finalization | both borrowed EF sample counts + 3 navigation allowance |
| Snapshot publication | frontier entries + query-chain visits + ownership setup |

Admission also charges singleton native output, its complete tiny index and
publication. Its target has at most one sample because the current query root
fits K. Job creation charges the copied frontier. `admission_cost()` calculates
these quantities from metadata, rather than assuming a fixed chain depth.

These are conservative **structural** allowances for executed or attempted
operations, not measured CPU instructions or bytes. An allowance can exceed
actual work, such as a merge step consuming one input instead of two. Successful
record counters are updated only after the corresponding worker step returns;
a throwing step may have attempted work covered by its allowance.

Long keys and values, allocation, encoded control lengths and composition
callbacks have additional costs. Constructors and EF finalizers execute
atomically after their structural allowance is funded. No single-call latency,
byte-volume or constant physical-space bound follows from these counters. The
count-model scheduler's numerical credit constant is not applied to this
backend.

Persistence boundary
--------------------

A persistence adapter retains the exact graph plus the oldest-first
`cola_runtime_interval` list. `snapshot.admissions()` is the final interval end.
The immutable nodes expose their owning artifact or mapped pair, native owner,
and exact main target, so the adapter can reuse existing files and seal newly
built nodes without rewriting unchanged natives.

```cpp
auto head = diet::cola_runtime_node<policy>::from_mapped(mapped_head);
auto saved = diet::cola_runtime_snapshot<policy>::restore(head, intervals);
auto active = diet::cola_runtime<policy>::from_snapshot(saved);
```

Wrapping and restoring inspect metadata only. They require an already admitted
or explicitly trusted main-only graph. Restore validates interval continuity,
power-of-two masses, frontier shape and native counts. It rejects secondary
routes: arbitrary COLA graphs need an explicit chronology mapping before they
can become runtime frontiers.

This restores an immutable admission frontier, not partial worker state.
Pending cleanup restarts from its retained inputs. File/catalog publication,
crash reconciliation and resource reservations belong to the durable adapter.
A failed executor retains its published snapshot and private input owners;
recovery uses a fresh executor from an immutable snapshot.

Contact Information
-------------------

Questions and patches are welcome at <ekmett@gmail.com>.

-Edward Kmett
