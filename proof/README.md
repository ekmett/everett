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
[lean-toolchain](lean-toolchain). The [official release](https://github.com/leanprover/lean4/releases/tag/v4.19.0)
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
whose current owner has adopted a different exact index/target pair.

An optional native executable target is available through `lake exe proof_examples`.
On macOS 26, I checked it using the system C compiler with `LEAN_CC`, adding the
pinned toolchain's `lib` directory to `LIBRARY_PATH`. The release's bundled linker
produced a binary rejected by that host's loader; the interpreted command above
avoids this platform-specific native-linking issue. Kernel checking does not
require the optional executable.

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
| [Examples](Everett/Examples.lean) | Heterogeneous keys, valid and stale sources, noncommutative histories, changed index/target versions, and an old target that cannot be reclaimed while a snapshot retains it |
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
foundations. The checked checkpoint audited 284 declarations and used only `propext` and
`Quot.sound`. It also catches a theorem placeholder hidden behind another
declaration. Compiler-generated unsafe
execution artifacts are outside that logical audit; no model source declares an
unsafe definition or an additional axiom.

I have deliberately not claimed:

- Correct byte/bit encoding, sampled-key correspondence, fractional-cascade
  windows, rank or Elias–Fano representation bounds.
- A bounded COLA scheduler, strong-deletion work accounting, or byte/I/O costs.
- Physical-file reachability, reader leases, SQLite transactions or crash recovery.
- C++ memory safety, compiler refinement or correctness of external implementations.

The next useful connection is a precise interpretation from encoded immutable
pairs to the abstract catalog. That would let us discharge sampled-target and
query equivalence obligations with codec proofs instead of leaving them at the
representation boundary.

Contact Information
-------------------

Contributions and bug reports are welcome through the
[Everett issue tracker](https://github.com/ekmett/everett/issues).
I can also be reached at <ekmett@gmail.com>.

This proof layer uses the same [dual license](../LICENSE) as Everett:
BSD-2-Clause **or** Apache-2.0, at the recipient's choice.

-Edward Kmett
