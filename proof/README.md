Everett: An Abstract Proof Core
==============================

I use this small Lean model to check the laws behind Everett's composable updates
and persistent representations. We can execute its admission and ownership
operations, and Lean checks the accompanying theorems. I keep this model separate
from the C++ implementation: a proof about these definitions does not by itself
verify the codecs, memory accesses or disk operations in `include/everett/`.

The [design](../docs/design.md), [per-key categories](../docs/arrows.md) and
[implementation ledger](../docs/implementation.md) give the surrounding context.
The first theorem connecting composition to publication is
`Everett.adopt_adjacent_merge`: replacing two adjacent changes by their composite
preserves the adopted root's full arrow meaning. We establish that equality from
the category laws, without treating equal fingerprints as equal states.

Build and run
-------------

I pin this project to **Lean 4.19.0**, release commit `6caaee842e94`, through
[lean-toolchain](./lean-toolchain). The [official release](https://github.com/leanprover/lean4/releases/tag/v4.19.0)
contains the toolchain. With Lean's `elan` toolchain manager installed, Lake uses
that file to select the version. No mathlib or external package download is
required after the toolchain is available.

From this directory:

```sh
LEAN_NUM_THREADS=4 lake --wfail build
lake env lean --run Main.lean
```

The default build checks every model module, the theorem-based examples and the
axiom audit. The interpreted executable prints the outcomes of
disjoint updates, an invalid reversal of same-key updates, and a retained snapshot
whose current owner has adopted a different exact index/target pair. It also
shows a sampled search whose native match lies before the routed window and is
recovered through a false borrow.

I use `by decide` only where Lean can reduce a concrete proposition in the
kernel; these examples do not use `native_decide` as a proof shortcut. Executing
the compiled examples also depends on the compiler and runtime. The theorem
checks and the executable output are separate forms of verification.

Field guide
-----------

| Module | Definitions and checked properties |
| --- | --- |
| [Category](Everett/Category.lean) | An explicit category policy; source/target-indexed histories; composition of concatenated histories; adjacent contraction in an arbitrary chronological context; binary-tree reassociation with the same ordered leaves |
| [Updates](Everett/Updates.lean) | A dependent family of state types and categories; componentwise world arrows; the typed disjoint-coordinate square; exact-source-checked single-key admission; target correctness; commutation of two valid updates at distinct keys |
| [Fingerprint](Everett/Fingerprint.lean) | Integer state potentials; endpoint-delta composition; history telescoping; adjacent-merge contribution preservation; the sum of per-key deltas over `Fin n` |
| [Snapshots](Everett/Snapshots.lean) | Exact pair/target identities; immutable catalog extension; owner-rooted reachability; snapshot retention and read preservation; complete-target readiness; eligible reclamation; independent adoption with an explicit semantic premise |
| [Allocation](Everett/Allocation.lean) | A monotone allocation watermark, fresh installation and non-reuse of issued IDs across allocation/reclamation sequences |
| [Adoption](Everett/Adoption.lean) | Discharges the semantic adoption premise for chronological adjacent merges, using the actual history-composition theorem |
| [Fractional](Everett/Fractional.lean) | Stable tagged merging; exact every-Kth samples; sampled predecessor windows; endpoint-rank projections; local/global predecessor equivalence; false-borrow recovery for unique native keys; a list-level index builder and exact-target retention |
| [Examples](Everett/Examples.lean) | Heterogeneous keys, valid and stale sources, noncommutative histories, changed index/target versions, and an old target that cannot be reclaimed while a snapshot retains it |
| [FractionalExamples](Everett/FractionalExamples.lean) | K=3 and K=15, equal keys across several cuts, empty native projections, false-borrow recovery, empty targets, before-first queries, short tails and stored-index routing |
| [Audit](Everett/Audit.lean) | Rejects unexpected axioms in every kernel-safe `Everett` declaration and its transitive dependencies |

Examples
--------

The checked fixture assigns a natural-number state to one key and a Boolean
state to another. We can prove equality of the resulting worlds, not merely
equality of their hashes:

```lean
import Everett
open Everett Everett.examples

example : mutation.apply_two initial increment enable =
    mutation.apply_two initial enable increment :=
  mutation.disjoint_commute initial increment enable (by decide) rfl rfl
```

Both changes are valid against `initial`. Their distinct keys make either
intermediate world a valid source for the other change. If we instead reverse
`increment` and `next`, which act on the same key, the second ordering fails its
source check. `Examples.lean` checks both outcomes.

For histories, I retain the arrow itself. `word_category` has one object and
lists of natural numbers as arrows; composition concatenates the lists. Its
arrows `[1]` and `[2]` do not commute, although every arrow has the same source
and target. Reassociation of a fixed chronological sequence is proved separately
from permutation. This example also gives a nonidentity arrow with zero endpoint
delta, so fingerprint equality cannot silently stand in for arrow equality.

In the snapshot fixture, current owner `0` and snapshot owner `1` initially name
root `1`, whose exact target is `0`. The current owner adopts new root `3`, whose
target is `2`. Roots `1` and `3` share a native identity but have different index
identities. `old_target_retained` proves that the snapshot still pins target `0`;
a proposed reclamation list containing `0` therefore fails the eligibility
condition.

Fractional indexing
-------------------

The [sampling design](../docs/sampling.md) starts from two ordered streams. We
merge their **occurrences**, retaining the origin tag and label even when keys
are equal. Native occurrences precede borrowed occurrences at the same key.
`augment_preserves_occurrences` proves a permutation of the complete input
records; `augment_filter` and `augment_filter_right` recover each source in its
original order. The labels are supplied by the caller. We preserve them without
assuming that arbitrary input labels are distinct.

`samples xs K` records positions $0,K,2K,\ldots$ that exist in `xs`, together
with the exact target occurrence at each position. Let $\ell$ be the last sampled
position whose key is at most the query, or zero if no sample qualifies. The
search window is

$$
[\ell,\min(\ell+K,|xs|)).
$$

`predecessor_bracket` places the global rightmost qualifying occurrence inside
that window. `routed_predecessor_correct` goes further: searching the actual
window and translating its result back to an absolute ordinal gives exactly
the same `Option Nat` as a full search. This includes equality runs, the short
last group, an empty target and queries before the first key. The local spacing
theorem needs only $K>0$; the storage policy's restriction to $K=2^n-1\geq3$ is
a separate codec choice.

Rank projects the virtual half-open window into a native interval and a
borrowed interval. `project_window` proves that each interval is exactly the
corresponding origin-filtered window, in order. `projected_lengths_sum` says
their lengths sum to the virtual length, so the two ranges share one budget of
at most $K$ occurrences.

There is an equality boundary worth keeping visible. A long run of borrowed
copies of a key may carry the route beyond its matching native occurrence.
Searching just the projected native interval would then miss the key. For a
pair with unique native keys, `false_borrow_recovery` proves that the matching
native occurrence is at

$$
\operatorname{rank}_{\mathrm{native}}(j)-1
$$

in the native stream, where $j$ is a borrowed occurrence of that key. The rank
is positive, so the subtraction is safe. `false_borrow_flag` constructs the
semantic flag by checking for a native match, and `false_borrow_flag_correct`
proves its exact meaning. `native_match_candidates` combines the ordinary
projected hit with this extra probe.

For example, the K=3 fixture has one native `5` at virtual position 2 and eight
borrowed copies spanning several cuts. A query for `5` routes to position 9,
finds its augmented predecessor at 10, and has an empty native projection.
Native rank is 2, so the extra probe retrieves native ordinal 1. These outcomes
are checked by reduction and by the general theorems. Native uniqueness is a
per-blob invariant; matching keys in different blobs remain separate history
segments. The more general merge and predecessor theorems preserve multiplicity
without that invariant, but equality recovery requires it.

The mathematical `rank` function is defined at every position. The concrete
`rank_groups<K>` API stores boundary ranks. `rank_inside_route` connects the
two: a finer rank is the boundary rank plus the native count in a local prefix
of fewer than $K$ occurrences. No arbitrary-position constant-time rank API is
assumed or proved.

Finally, `build_index` stores a target ID and its sampled target occurrences.
`build_index_matches` establishes exact correspondence from the builder's
output. `index_route` reads those stored samples, and
`indexed_predecessor_correct` proves that the resulting target-window search
agrees with a full search of that exact target. Catalog extension and eligible
reclamation preserve the certificate by preserving the target record. A source
pair's `target_samples` follows its literal stored target edge; adding a merged
target does not redirect that edge.

This builder produces mathematical lists. Its entries retain the destination's
occurrence labels and tags; constructing a source's borrowed stream requires
source-local labels and borrowed tags. That retagging, encoded-file decoding,
independent borrowed-predecessor routing and composition of an entire cascade
remain separate refinement obligations. The searches here enumerate finite
lists, so these theorems establish the window's entry bound and lookup meaning,
not the running time of binary search, compressed rank or key reconstruction.

What the assumptions mean
-------------------------

I expose category laws as fields of `category`: identity and associativity are
requirements on a policy, not axioms asserting Everett's desired result. The
replacement and noncommutative word policies supply concrete proofs of those
laws. Histories have typed endpoints, so a chain cannot contain an arrow whose
source differs from the preceding arrow's target.

`mutation.apply` is an executable **endpoint-state projection**. It checks the
exact old state and installs the target of an admissible arrow. It does not
retain the arrow or evaluate a compact diff. `world_category`, `history`, and
the adoption theorem describe full arrow semantics separately. I have not yet
proved an executor refinement connecting those two layers, or arbitrary
partition-batch replay and duplicate-delivery suppression.

`world_category` supports a category depending on the full key. This first model
uses ordinary dependent function worlds; it does not yet construct the restricted
product of worlds with finite support relative to a baseline. The fingerprint
sum explicitly enumerates `Fin n`, so each key in that finite universe occurs
once. Potentials take values in exact integers:

$$
\Delta(x,y)=\phi(y)-\phi(x),\qquad
\Delta(x,z)=\Delta(x,y)+\Delta(y,z).
$$

No injectivity, randomness or collision bound is assumed. Generalizing the
integer proof to a parameterized additive group is a separate extension.

The catalog is an immutable function from natural-number IDs to optional blob
records. Each record stores its exact native ID, index ID, target blob ID and an
abstract payload. The target edge is followed literally. Native/index IDs are
uninterpreted identities here; the model does not open files or calculate content
addresses. `storage` adds a monotone watermark so the allocation API cannot reuse
an issued ID after reclamation. Calling the lower-level `catalog.install` directly
requires its separate freshness proof; it is not a complete allocator.

`catalog.observe` reads a root's stored abstract payload. It does not decode or
compose the target graph. Consequently, independent adoption requires concrete
readable old/new records and equal payloads. `adopt_adjacent_merge` supplies that
equality for an actual typed merge. `adopt_ready` separately requires a complete
new target graph, while reclamation theorems require a proof that no deleted ID
is reachable from an owner. These are explicit interface obligations. I am not
assuming that an unverified external index satisfies them.

Axiom audit and verification boundary
------------------------------------

`Audit.lean` visits every kernel-safe declaration in the `Everett` namespace,
collects its transitive axiom dependencies, and fails the build if it finds
anything outside Lean's standard `propext`, `Quot.sound` and `Classical.choice`
foundations. The build reports the declaration count and the actual dependencies.
This slice uses all three, including `Classical.choice` through Std's list
theorems. The audit also catches a theorem placeholder hidden behind another
declaration. Compiler-generated unsafe execution artifacts are outside that
logical audit; no model source declares an unsafe definition or an additional
axiom.

I have deliberately not claimed:

- Correct byte/bit encoding, front coding, compressed rank or Elias–Fano
  representation bounds.
- A refinement from encoded source streams to the list-level sample certificate,
  an entire cascade search, or composition of same-key history across blobs.
- A bounded COLA scheduler, strong-deletion work accounting, or byte/I/O costs.
- Physical-file reachability, reader leases, SQLite transactions or crash recovery.
- C++ memory safety, compiler refinement or correctness of external implementations.

The next useful connection is a precise interpretation from encoded immutable
pairs to these abstract sequences and catalog records. The list-level builder
and local lookup theorems give that refinement a concrete contract to meet.

Contact Information
-------------------

Contributions and bug reports are welcome through the
[Everett issue tracker](https://github.com/ekmett/everett/issues).
I can also be reached at <ekmett@gmail.com>.

This proof layer uses the same [dual license](../LICENSE) as Everett:
BSD-2-Clause **or** Apache-2.0, at the recipient's choice.

-Edward Kmett
