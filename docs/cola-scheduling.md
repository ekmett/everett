COLA scheduling with immutable pairs
===================================

I use the standard COLA distinction between main, secondary and shadow arrays
as the scheduling baseline. A main array routes to the next level's main and
optional secondary arrays. A secondary is a leaf. Main edges name exact
immutable native/index pairs; secondary edges retain the native file directly,
without a secondary `.index`. Changing a native file's role can require a new
main index without changing its native bytes.

The [count model](../tests/cola_schedule_model.h) executes this topology and its
visibility transitions. It is a test specification, not a production scheduler
or a durable job system. Its work counters describe fixed-size record and
directory events; they are not measurements of the model's CPU time or disk I/O.
The [tests](../tests/cola_schedule.cc) check history coverage, snapshots, budget
partitioning and slot-reuse deadlines against independent graph traversals.

Main and secondary capacities
-----------------------------

Write $m_i,s_i$ for the native counts of the main and secondary arrays at level
i, and $M_i$ for the main array's augmented count. A missing secondary has
$s_i=0$. Its terminal representation contains only its native entries. Sampling
each target at interval K gives

$$
M_i=m_i+\left\lceil\frac{M_{i+1}}K\right\rceil
        +\left\lceil\frac{s_{i+1}}K\right\rceil.
$$

If $m_i,s_i\le B_i$, $B_{i+1}\le2B_i$, and $K>2$, induction gives

$$
M_i\le\frac{K+2}{K-2}B_i+2.
$$

The coefficient a solves $a=1+2(a+1)/K$. Each integer ceiling contributes at
most $(K-1)/K$, so an additive allowance of 2 is preserved by the recurrence.

| K | Main augmented/native-capacity coefficient | Main borrowed/native-capacity coefficient |
|---|---:|---:|
| 3 | 5 | 4 |
| 7 | 9/5 | 4/5 |
| 15 | 17/13 | 4/13 |
| 31 | 33/29 | 4/29 |

K=3 therefore works with a main reservation of $5B_i+2$ augmented entries.
Only one child recurses; treating both children as recursive would incorrectly
introduce a $4/K$ feedback coefficient for this topology. Conversely, allowing
the secondary to continue to further levels would invalidate this argument.

I keep native capacities $B_i=2^i$ and reserve the separate index space using
the bound above. There is no need to enlarge the smallest native run. If we
instead insist on a purely multiplicative main cap cB, without additive room,
a sufficient minimum is

$$
c>\frac{K+2}{K-2},\qquad
B\ge\left\lceil\frac{2}{c-(K+2)/(K-2)}\right\rceil.
$$

For c=2 this gives B at least 10 for K=7 and at least 3 for K=15. K=3 cannot
fit that half-native allocation simply by choosing a larger minimum run.

Let T count all augmented main entries and terminal-secondary entries in one
representation, N its native entries, and h its levels. Summing the recurrence
gives the separate global bound

$$
T\le\frac K{K-1}N+2(h-1).
$$

These are entry counts. Encoded literals, values, incomplete outputs and the
union of historical snapshot dependencies have separate space charges.

Navigation and sampling
-----------------------

The original construction uses eighth-position samples for a sole main target
and sixteenth-position samples from each target when both are present, together
with duplicate-pointer cells. Its shadow arrays become visible through linked
arrays from level zero; its safety condition includes lookahead construction.
These are the scheduling ideas I adopt, while reserving space for Everett's
different index representation.
[Bender et al., §3, pp. 9–10](https://people.cs.georgetown.edu/~jfineman/papers/sbtree.pdf#page=9).

I initially use K for both target sampling intervals and virtual cut spacing.
Copying the paper's 8/16 rates without changing the cut grid would leave some
sampled ordinals between stored cuts or require larger query windows. For
independent target strides $k_m,k_s$ and capacity growth g, the corresponding
main coefficient would be

$$
a=\frac{1+g/k_s}{1-g/k_m},\qquad k_m>g.
$$

A main index merges three origins: native, main-borrow and secondary-borrow.
Two grouped population prefixes locate the borrowed ranges; subtraction gives
native rank. Their class counts must sum to at most the actual window length.
Native entries precede both borrowed roles on equality. Each borrowed stream
preserves its own occurrence order.

Each stream also has its own preceding borrowed key $C_j$, false-borrow flag
and exact cut LCP $\ell_j=\mathrm{lcp}_{\mathrm{bits}}(C_j,B)$. At a routed
boundary $B\le Q$, we repair the two query comparisons independently:

$$
\mathrm{lcp}_{\mathrm{bits}}(C_j,Q)
=\min\bigl(\ell_j,\mathrm{lcp}_{\mathrm{bits}}(B,Q)\bigr).
$$

The predecessor is absent exactly when that stream's borrowed rank is zero.
Its terminal nonzero rank still identifies a predecessor. A false flag refers
to the local native key, not the other borrowed stream. If both routes recover
the same local native ordinal, the query emits that native once and follows
both routes. Equal native matches in different child arrays remain separate
chronological contributions.

The three projected ranges total at most K forward records. Only the main
child continues, so queries visit at most two arrays per level. Level zero also
has two roots: main_0 alone does not reach secondary_0. A bounded two-root
entry point or a synthetic empty-native parent must cover both.

Logical slots, work and visibility
---------------------------------

The model gives each level three logical array slots. A slot records whether
its native run is staging, full and unmerged, or consumed; carrier readiness
and root visibility are separate properties. A native run carries a contiguous
chronological admission interval with mass $2^i$. Composing duplicate keys can
reduce its native record count without changing this scheduling mass.

Each unsafe-level job has three costed stages:

1. Merge the two full native runs, older then newer, into a reserved shadow
   destination one level higher. An input may itself still be hidden.
2. Complete the destination index. A filled carrier supplies its exact
   downstream targets; a prospective secondary is terminal.
3. Build a source-level lookahead carrier for the resulting destination main
   and optional secondary. At local commit, mark the source inputs consumed,
   make the source safe, and activate the destination as full and unmerged.

Native completion alone does not activate destination fullness. Otherwise the
destination could become unsafe while its source is still constructing the
lookahead needed to become safe.

A later lower-level merge prefers the prepared carrier slot. It fills the
native run and builds the final index against the carrier's unchanged target
set. Lower carriers link to that final immutable pair identity. Above level
zero, the empty carrier remains private until this fill. At level zero its
bounded empty-native pair can immediately replace the root.

A complete pair becomes visible when reached from the root. If it replaces two
already-consumed arrays at its level, their current logical slots are retired.
A complete shadow can participate in a higher merge before it becomes visible;
visibility must still occur before another merge into that level needs its
slot. Tests check this deadline rather than assuming that native completion
establishes it.

Snapshot owners retain exact old dependency graphs independently of those
logical slots. Dropping an old pair from the current graph does not revoke its
saved owners. The model keeps an immutable archive for its oracle and separately
computes the union of owned main-pair and secondary-native IDs; it does not
implement physical reclamation. A secondary node in the model is a handle for
that native file, not an additional index object.
Three logical slots consequently do not claim three physical `.index` files
including snapshots and partially constructed carriers.

Current-world object closure
----------------------------

The useful space invariant is stronger than counting slots: every exact target
owned by the current root, a live array or a private carrier must still occupy
a logical slot. Otherwise a retired slot could hide an arbitrarily long tail
of immutable dependencies. The model tests this closure independently after
admissions and paused service transitions. Its current-world closure has at
most $3h$ completed array handles across h levels. A main handle owns a native
file and index; a secondary handle owns only a native file. Unfinished output
and index stages add only a constant number of owners per active job.

The retirement obligation is that a newly visible replacement leaves no
private carrier or unfinished job referring to an input whose slot is being
released. The smallest-unsafe schedule and visibility-before-reuse rule are
what must establish that obligation; root unreachability alone is insufficient.
The executable histories satisfy it, but I have not proved it for all histories.
A production scheduler must also release completed builders and input owners.

This $O(h)$ working-owner argument excludes saved roots, timeline generations,
failed attempts and durable continuation checkpoints. Their retained graphs
must be counted as a union of exact identities, with separate retention limits.
The current insert-only catalog does not reclaim these records or files, so
this is not a bound on its on-disk history or an implemented garbage collector.

Counted service and its limits
------------------------------

`admit_one` establishes a bounded level-zero representation. `advance(budget)`
always services the smallest unsafe level and spends at most that many model
events. Zero budget changes nothing. Splitting a budget into one-event calls
has the same completed identities, roots and charges as a whole-budget call.

For native count n and target augmented counts $t_m,t_s$, define

$$
b_j=\lceil t_j/K\rceil,\qquad v=n+b_m+b_s.
$$

The model charges an index stage

$$
I=t_m+t_s+v+5\lceil v/K\rceil
  +\lceil n/W\rceil+\lceil b_m/W\rceil+\lceil b_s/W\rceil+3.
$$

These terms account for target scans, merged occurrences, grouped rank/cut
events, physical offset samples, and three stream endpoints. The carrier uses
the same charge with n=0. Empty stages therefore still cost work. Native merging
costs the two input counts plus output count and one setup event. A terminal
secondary needs only its native offset-directory completion, charged as
$\lceil n/W\rceil+1$, rather than constructing an index.

For $K\ge3$, $W\ge1$ and a level-r source capacity $B=2^r$, the capacity bounds
give destination-index cost at most 77B, carrier cost at most 43B, and native
cost at most 5B. I use a conservative local ceiling of 160B. Root visibility
bookkeeping is charged separately at $2h+4$; immediate admission has a separate
constant index charge. This makes the service constant explicit instead of
counting native records while omitting index completion.

The test driver supplies $512(h+2)$ events per admission, where h is the bit
length of the admitted count. The local ceiling is at most 80 events per unit
of the binary merge's 2B admission mass. This rate leaves room for the
smallest-unsafe-level argument and the separately charged root work. The tests
exercise the resulting deadlines; they are finite executable evidence, not a
formal proof for every history or an implementation of the paper's cache-I/O
eviction argument.

Native key unions and graph traversals are reference computations performed
atomically by the test program. Its charged stages are a work specification,
not a claim that those C++ calls are interruptible. In the real codec, linear
EF finalization must become scheduled work before a worst-case service claim.
Long keys, values, count codewords, callback evaluation, allocation and storage
barriers also need a byte/I/O service model.

Scope of the first model
------------------------

The tests cover K=3,7,15,31; W=1,16; distinct, repeated and mixed keys; fixed
admissions across power-of-two boundaries; pauses in every stage; hidden merge
inputs; immutable snapshots; and failure to admit without enough service.
History coverage comes from an independent traversal of exact pair edges and
contiguous native intervals. Per-key values are chronological token sequences,
so reversing intervals would change the oracle result.

I have not added arbitrary-size received-file admission, fork adoption, deletion
elision, durable scheduler recovery or live-size rebuilding to this model.
Shared native completion can later serve several worlds, but each world's
index adoption needs its own work budget. A large received file bypasses the
level-zero arrival slack; sorting files by size also does not authorize merging
nonadjacent same-key history. Those extensions retain the separate obligations
in [network admission](network-admission.md) and [rebuilding](rebuild.md).
