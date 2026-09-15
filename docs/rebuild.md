# Strong deletion by incremental rebuilding

Design checkpoint, 2026-09-15. Tombstones remove bindings from query results,
but leave their old records behind. To keep the active representation proportional
to live state, we need to rebuild before too much of it becomes history. This
is the rebuilding executor planned above the [Diet design](design.md).
The existing reference cola tests eager compaction, live counts and fingerprints;
it does not implement this schedule or its disk publication protocol.

The concrete elision and clean-image rules below apply to replacement-valued
records. To use the [per-key categorical extension](arrows.md), we additionally
need a category-specific clean representation: preserve the composite arrow,
or establish that a materialized endpoint is sufficient for all supported future
updates and observations. Equal endpoint hashes alone do not authorize us to
discard arrow semantics or source dependencies.

## 1. The Overmars–van Leeuwen construction

The starting point is **Theorem 1 in §2**, printed pages 3–4 of Overmars and van
Leeuwen, *Worst-case optimal insertion and deletion methods for decomposable
searching problems*. The
[Utrecht author copy](https://ics-archive.science.uu.nl/research/techreps/repo/CS-1980/1980-10.pdf#page=5)
contains the same construction. The journal version is associated with
[DOI 10.1016/0020-0190(81)90093-4](https://doi.org/10.1016/0020-0190(81)90093-4).

The theorem converts weak updates into clean updates, preserving asymptotic
query cost and adding rebuilding work $O(P(n)/n)$ to each weak update.
Its stated cost functions are nondecreasing and smooth, with construction cost
at least linear; the constant-factor size changes in the argument use these
assumptions.
Their construction normally has `MAIN`. After enough transactions, it becomes
`OLD-MAIN`, which continues answering queries and receiving updates while a new
`MAIN` is built. Intervening updates enter `BUF`. Crucially, an update contributes
both a rebuilding allowance and an allowance for its later execution on the new
structure. During initial construction, both allowances accelerate construction;
afterward they service buffered updates. This leaves finite time to catch up.

The report triggers after transactions exceed half the structure's initial
size and requires takeover within another quarter of the size at construction
start. Those are the report's constants. I choose the more conservative
thresholds below for this store; they are not constants from the paper.

We have to count **weak transactions**, not just deletions. An overwrite that
leaves an obsolete version behind must participate in the rebuilding schedule
too.

## 2. What strong deletion promises here

When we accept a tombstone, the binding immediately disappears from logical
queries. That is a weak deletion: we may still retain the older binding and its
nominal level weight in the representation. Strong deletion removes this
historical burden often enough that current live state determines the active
representation's size.

Let:

- $N$ be the current number of live bindings.
- $b$ be the live cardinality at the clean base cut of a generation.
- $u$ count distinct admitted per-key mutations since that cut, including
  each such mutation applied by candidate replay.
- $n_s$ be the live cardinality of the frozen snapshot used by a rebuild.

These are counts of uniquely addressed logical bindings. Several historical
records for one key are not several current members available for deletion.
For unit-valued multiplicity normalized to $(k,n_k)$, a decrement requires
$n_k>0$ and counts as a weak mutation. It changes $N$ only on
$1\to0$. Total multiplicity $\sum_k n_k$ is a separate quantity;
shrinking it does not imply that the table has fewer occupied keys. The count's
representation and arithmetic costs also remain part of the byte/work budget.

A mutation that emits an unchanged binding still consumes history if we append
it, so we count it in $u$. We reject an absent-key delete and treat an identical
network delivery already accepted as a replay, not another mutation. Executing
a distinct admitted mutation on the candidate does count toward that candidate
generation's $u$. A batch of a thousand key changes contains a thousand
scheduling units.

The target is an active record universe $O(N)$, hence $O(\log(N+1))$ levels,
with the redundant-level scheme's constant factors. Concurrent updates can create
new obsolete versions immediately after cleanup. The schedule must bound their
accumulation; it need not eliminate every tombstone at every instant.

“Half-size” refers to a cola with half as many live bindings. Its byte size may
change quite differently: key lengths, prefix compression and retained snapshots
can differ substantially.
The record-count result alone does not establish a byte-space bound relative to
the surviving strings. Section 8 states the additional byte accounting.

## 3. Trigger early and preserve the next generation's slack

For a sufficiently large generation, I trigger rebuilding when

$$
u \ge \lfloor b/4\rfloor.
$$

We check after each mutation and trigger at the first crossing, when
$u=\lfloor b/4\rfloor$. Each mutation changes cardinality by at most one, so
we know that at this cut

$$
3b/4 \le n_s \le 5b/4.
$$

We freeze the exact current root. I choose a handoff horizon of

$$
h=\lfloor n_s/8\rfloor.
$$

We must finish the rebuild and catch-up within the next $h$ admitted mutations.
Even if all of them delete live bindings, we hand off while

$$
N \ge n_s-h \ge 7n_s/8 \ge 21b/32 > b/2.
$$

We therefore have this generation's replacement ready before half its clean base
has disappeared. If we waited until the half-live threshold to begin, we would
have no such slack. Inserts and overwrites also advance the work; counting only
deletions would let repeated overwrites grow the history without bound.

At handoff the new generation has clean-base cardinality $b'=n_s$, but its
mutation count is **already** the number of replayed mutations:

$$
u' = r \le h \le b'/8.
$$

We must carry $u'$ forward rather than reset it to zero: those mutations already
created history in the new generation. The next trigger at $\lfloor b'/4\rfloor$
still has at least $\lfloor b'/8\rfloor$ mutations of slack. A generation can
therefore finish before another rebuild is needed.

Small generations use an eager bounded-size base case, for example below 64 live
bindings.
We must give large incoming batches their per-binding work budget while staging
them. Counting one large batch as one mutation would bypass both the trigger and
the deadline. An atomic batch
that crosses several thresholds needs internal per-key staging cuts, or must
pay the aggregate work and finish the required rebuilding phases before its
large state jump is acknowledged. Atomic publication is not a work exemption.

## 4. Freeze, build, replay, adopt

### Freeze a complete logical cut

We pin the current immutable root $S$, its precise index dependencies, and its
resolution/precedence rules, and record $n_s=N(S)$ and $C_s=C(S)$. We keep
serving queries through a current foreground root descended from $S$, without
exposing the incomplete candidate.

Persistence gives us a stable rebuilding source directly. We can scan the pinned
snapshot while the foreground root changes, without maintaining a second mutable
copy just to protect that source.

Every subsequently accepted change has two effects:

1. It updates the foreground cola through the ordinary validated update path.
2. Its exact operation identity and old/new bindings enter a replay queue for
   the candidate, with a reserved bound on its replay/maintenance work.

Queue insertion is part of admission. A crash must not leave an acknowledged
foreground mutation absent from the recorded rebuilding continuation.

### Build the clean base

We merge the **entire resolved frozen cola** into fresh immutable native data.
For each key, we retain its newest visible live value and emit nothing if its
resolved value is a tombstone. This removes every version obsolete at the freeze
cut, including the deletion markers that hid them.

This is where we turn tombstones into strong deletion. We cannot get that result
by copying every frozen physical record or merging only some older runs without
a coverage proof.

We construct fresh native offsets, index streams and their dependency closure
as part of the rebuilding job. If the new data file's fractional indexes still
require the old generation, we have not yet built a fully independent root.

### Replay while the foreground remains available

Once the clean base is usable, we apply the queued changes to the candidate in
their semantic order. We preserve the order of updates to the same key across
rounds; within a round, we can use any agreed linearization of disjoint partition
batches. We check old values and advertised deltas as in normal admission.

The candidate may reuse an update's native file when its contents and semantics
are identical. Its fractional indexes must still name the candidate's exact
targets, so index repair is included in replay work. Sharing physical update
bytes is not a reason to share an incompatible index version.

A new deletion applied during replay can still require a tombstone against the
clean base. It is recent history, counted in $u'$, and is removed by a later
coverage-complete compaction or global rebuild. The construction does not demand
an empty tombstone set before adoption.

### Adopt at a finite cut

When the queue is empty after a serviced admission, foreground and candidate
represent the same accepted cut. In that serialized admission step, we switch
the writer destination to the candidate and publish its root. A later arrival
belongs to the new generation. This gives us no interval in which a mutation
can miss both destinations or be applied twice.

The new clean base is semantically older than every replayed update. Manifest
construction must retain that precedence; appending a compacted base after
newer entries in a vector interpreted as chronological order would be wrong.
The current generic `pin_set` owns lifetimes and contributions, not a complete
version-precedence policy for arbitrary partial replacement.

For a durable handoff, use the barriers and recovery states in
[Durability and merge resumption](durability.md). Candidate output
and its replacement manifest become durable before the old durable root's pins
are retired. Other owners may continue retaining all of those old objects.

## 5. Why the replay queue actually finishes

Let $R$ bound the initial clean-base rebuilding job, including scans,
resolution, encoding, metadata and index construction. Let $w_i$ bound the
work required to replay mutation $i$ and leave the candidate's query/index
invariants usable. The candidate's applicable weak-update algorithm must provide
such bounds; merely naming an amortized cost is insufficient.

For each admitted mutation during rebuilding, we allocate this background budget:

$$
q_i=\lceil R/h\rceil+w_i.
$$

The foreground mutation's normal execution has its own budget. The background
worker runs work-conservingly: first the finite base-building job, then the FIFO
of replay jobs. During base construction, the $w_i$ allowance advances that
construction too; it does not sit idle waiting for replay to become possible.

Let $W_i$ be outstanding background work after servicing mutation $i$.
Initially $W_0\le R$. The new replay job adds at most $w_i$, so

$$
W_i\le\max\bigl(0,W_{i-1}+w_i-(\lceil R/h\rceil+w_i)\bigr)
\le\max(0,R-i\lceil R/h\rceil).
$$

Consequently $W_h=0$: we have finished the base and every admitted replay job.
The argument lets us handle different replay costs without assuming that two
arbitrarily sized changes cost comparable amounts. We are expressing the
original theorem's acceleration and buffering argument as a work budget.

We must include newly created index work and all other required publication work
represented in the cost model. If a job adds unforeseen work, our reserved bound
was wrong and we must correct it; hiding the work in a final catch-up pass would
invalidate the proof. We also need to resume background tasks at the charged
work granularity, including inside long strings.

If arrivals stop, we can keep the same worker running with an idle-time allocation.
The mutation-count deadline still requires CPU/I/O service. An unbounded
filesystem stall or failed durability barrier can prevent a successful save;
the counting argument supplies no wall-clock bound across such a failure.

## 6. Why historical updates stop determining the universe

Immediately after adoption, the candidate consists of a clean frozen base with
$n_s$ live records plus history from at most $h$ recent mutations. Ignoring
the redundant level scheme's fixed replication constants, this is at most

$$
n_s+h\le 9n_s/8
$$

record occurrences, while current live cardinality is at least $7n_s/8$.
We have therefore bounded this part of the representation by $9/7$ times
current live size. The candidate contains no obsolete records from before the
freeze.

Between handoffs, the carried counter and quarter-size trigger bound the new
history by another constant multiple of the base. During a rebuild there are a
bounded number of generations; each is still $O(N)$, assuming the local
redundant-level scheduler itself keeps only its bounded array multiplicities.
The active level bound is therefore $O(\log(N+1))$, rather than $O(\log H)$
for the number $H$ of updates since the timeline began.

We have to count overwrites for this argument to work. Repeatedly changing one
key must eventually replace its old generation even when live cardinality never
changes. We would not reset the universe by resetting a nominal counter while
retaining all physical versions.

This bound includes the current query dependency closure and unfinished current
rebuilds. We charge objects retained solely by old saves, readers, other
timeline forks or their checkpoints separately as retained history. We cannot
require a snapshot of an old large cola to occupy space proportional to today's
small cola.

## 7. Coverage, contributions and durable continuation

### Elision coverage

A frozen-cola rebuild knows every older contribution relevant to that cola,
so it can omit both a winning tombstone and every value that tombstone hides.
An unrelated historical snapshot does not prevent this omission in the new
root; that snapshot retains its own old files. A partial compaction may elide a
tombstone only if its recipe proves that no unmerged older binding can become
visible through the result's query graph.

### Hash and live-count checks

At any common admitted cut $t$, require

$$
C(\text{candidate}_t)=C_s+\sum_{i\le t}\Delta_i=C(\text{foreground}_t),
$$

$$
N(\text{candidate}_t)=n_s+\sum_{i\le t}
\bigl([v_i^{\rm new}\ne\mathrm{Nothing}]
-[v_i^{\rm old}\ne\mathrm{Nothing}]\bigr).
$$

We check each root independently. Adding the foreground root's signature to the
candidate root's signature would count the same logical cola twice during
rebuilding. The candidate clean base contributes $C_s$;
its replayed update entries contribute their validated deltas. Index-only
dependencies contribute zero. The old owner is replaced, not algebraically
combined with the new owner. Fingerprint equality remains a lint check, not a
proof of record equality or of complete elision coverage.

### Checkpoint contents

To resume a rebuild, we must record at least:

- Exact frozen root, current foreground root, merge recipe and input versions.
- Candidate root or construction cursor, input decoding contexts, verified
  output prefix and encoder state, offset/index builders and pending jobs.
- Durable replay-log identity, admitted-cut identifiers, durable log head and
  replay cursor, with the operation identities needed for exactly-once replay.
- $b,u,n_s,h$, the replay count that will become the new $u$, work bounds,
  completed work and remaining publication obligations.
- Candidate and foreground fingerprints/live counts at their respective cuts.

We checkpoint only bytes already made durable. During recovery, we either resume
an exact verified continuation or discard the incomplete candidate and rebuild
from retained inputs. We must not reset the weak-update debt or release the old
root because uncertain output looks complete. Durable round replay metadata and
merge-continuation metadata have related cuts but distinct purposes.

## 8. Records are not bytes

For general update arrows, a live-key count also does not bound the size or
evaluation cost of the composite change. A single key can retain a growing
program or a large source value. The construction bound must include composing,
normalizing or materializing those arrows, and replay must charge their actual
work. The outstanding-work inequality still applies when those work bounds
exist; the replacement theorem does not supply them for an arbitrary category.

For bounded-size records, if rebuilding costs $R=O(n_s\log n_s)$, its allowance
$R/h$ is $O(\log n_s)$ per mutation. This matches the intended prepaid
logarithmic record work. Actual constants include duplicated index construction
and the candidate's weak-update maintenance.

For unbounded strings, we keep separate bounds for records, bytes read/emitted,
character comparisons and index work. We can apply the same outstanding-work proof
to a chosen bounded work measure, but its per-admission $R/h$ term can be
large. A huge unchanged key plus many tiny deleted keys is a counterexample to
charging the whole rebuild only to the deleted keys' lengths.

The disk executor must therefore reserve the required byte work from earlier
ingestion/maintenance credits or explicitly charge and schedule a byte budget
proportional to the frozen rebuild. Prepayment funds work; actual execution must
still meet the service schedule. A record-count proof alone does not establish
$O(|\text{changed key}|\log N)$ worst-case update time for strings.

Likewise, we need a separate obsolete-byte trigger if we want a space promise
relative to live encoded byte volume. Deleting one enormous key can halve that
volume while barely changing $N$. A quarter-record-count trigger therefore
cannot establish a half-byte-size deadline. We still need to measure and bound that
weighted rebuilding policy when implementing the executor.

## 9. Acceptance cases for the executor

Test the executor's schedule against a simple resolved-table oracle:

- Pure deletion, insertion and overwrite streams, including a fixed one-key
  live cola with arbitrarily many overwrites.
- Adversarial updates immediately before a key is copied, immediately after it
  is copied, and while queued replay has not reached it yet.
- Repeated delete/reinsert cycles; absent deletes and duplicate deliveries must
  not invent accounting credits or replay history.
- Unequal replay costs, verifying the outstanding-work inequality after every
  admission; large batches are charged per changed binding.
- Handoff exactly at a queue-empty cut, followed immediately by another update;
  replay debt persists and the next trigger still has the promised slack.
- Complete tombstone elision versus partial coverage, including snapshots that
  continue reading values deleted from the current cola.
- Replacement indexes that do not retain unwanted old query dependencies.
- Foreground/candidate fingerprints and live counts at matched cuts, without
  double-counting temporary copies.
- Failure and restart at every durable barrier, with all checkpoint/pin owners
  retained until the recovery protocol permits reclamation.

These are required tests for the future executor. Its implementation must
preserve the record-count schedule and outstanding-work inequality developed
above.
