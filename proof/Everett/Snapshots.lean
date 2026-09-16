/-
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-/
import Std

namespace Everett

universe u

/-- A logical immutable pair; native/index IDs are exact identities, not hashes.
The payload denotes its abstract meaning. Decoding that meaning is another layer. -/
structure blob (meaning : Type u) where
  native_id : Nat
  index_id : Nat
  target : Option Nat
  payload : meaning
  deriving Repr, DecidableEq

abbrev catalog (meaning : Type u) := Nat → Option (blob meaning)
abbrev owners := Nat → Option Nat

namespace catalog

variable {meaning : Type u}

def install (S : catalog meaning) (id : Nat) (b : blob meaning) : catalog meaning :=
  fun j => if j = id then some b else S j

/-- Extension preserves every present entry. Allocation.lean separately prevents
reuse of identifiers that have been reclaimed. -/
def extends_catalog (new old : catalog meaning) : Prop :=
  ∀ id b, old id = some b → new id = some b

theorem install_fresh_extends (S : catalog meaning) (id : Nat) (b : blob meaning)
    (fresh : S id = none) : extends_catalog (install S id b) S := by
  intro j old h
  by_cases same : j = id
  · subst j; rw [fresh] at h; contradiction
  · simp [install, same, h]

@[simp] theorem install_here (S : catalog meaning) (id : Nat) (b : blob meaning) :
    install S id b id = some b := by simp [install]

/-- The reflexive transitive closure follows the stored exact target ID. -/
inductive reaches (S : catalog meaning) (root : Nat) : Nat → Prop
  | root : reaches S root root
  | target {id next} (prior : reaches S root id) (b : blob meaning)
      (found : S id = some b) (edge : b.target = some next) : reaches S root next

theorem extends_reaches {S T : catalog meaning} (extension : extends_catalog T S)
    {root id} (h : reaches S root id) : reaches T root id := by
  induction h with
  | root => exact .root
  | target prior b found edge ih => exact .target ih b (extension _ _ found) edge

/-- Owners root the dependency closure. Index targets are retained, not summed. -/
def pinned (S : catalog meaning) (O : owners) (id : Nat) : Prop :=
  ∃ owner root, O owner = some root ∧ reaches S root id

theorem target_is_pinned {S : catalog meaning} {O : owners} {id next}
    (pin : pinned S O id) (b : blob meaning)
    (found : S id = some b) (edge : b.target = some next) : pinned S O next := by
  obtain ⟨owner, root, ho, path⟩ := pin
  exact ⟨owner, root, ho, .target path b found edge⟩

/-- Readiness is a separate obligation: a root and every target it reaches exist. -/
def ready (S : catalog meaning) (root : Nat) : Prop :=
  ∀ id, reaches S root id → ∃ b, S id = some b

def well_formed (S : catalog meaning) (O : owners) : Prop :=
  ∀ owner root, O owner = some root → ready S root

/-- Removing explicitly selected dead IDs is executable; eligibility is proved. -/
def reclaim (S : catalog meaning) (dead : List Nat) : catalog meaning :=
  fun id => if id ∈ dead then none else S id

def eligible (S : catalog meaning) (O : owners) (dead : List Nat) : Prop :=
  ∀ id, id ∈ dead → ¬ pinned S O id

theorem reclaim_preserves_pinned {S : catalog meaning} {O : owners} {dead : List Nat}
    (safe : eligible S O dead) {id} (pin : pinned S O id) :
    reclaim S dead id = S id := by
  have absent : id ∉ dead := fun h => safe id h pin
  simp [reclaim, absent]

/-- All original dependency paths, not only their roots, survive permitted GC. -/
theorem reclaim_preserves_paths {S : catalog meaning} {O : owners} {dead : List Nat}
    (safe : eligible S O dead) {owner root id} (ho : O owner = some root)
    (path : reaches S root id) : reaches (reclaim S dead) root id := by
  induction path with
  | root => exact .root
  | @target previous next prior b found edge ih =>
    have pin : pinned S O previous := ⟨owner, root, ho, prior⟩
    exact .target ih b (by rw [reclaim_preserves_pinned safe pin]; exact found) edge

theorem reclaim_reaches_original {S : catalog meaning} {dead : List Nat}
    {root id} (path : reaches (reclaim S dead) root id) : reaches S root id := by
  induction path with
  | root => exact .root
  | @target previous next prior b found edge ih =>
    have original : S previous = some b := by
      by_cases h : previous ∈ dead
      · simp [reclaim, h] at found
      · simpa [reclaim, h] using found
    exact .target ih b original edge

theorem reclaim_well_formed {S : catalog meaning} {O : owners} {dead : List Nat}
    (wf : well_formed S O) (safe : eligible S O dead) :
    well_formed (reclaim S dead) O := by
  intro owner root held id path
  have old_path := reclaim_reaches_original path
  obtain ⟨b, found⟩ := wf owner root held id old_path
  exact ⟨b, by rw [reclaim_preserves_pinned safe ⟨owner, root, held, old_path⟩]; exact found⟩

def observe (S : catalog meaning) (root : Nat) : Option meaning :=
  (S root).map blob.payload

def read (S : catalog meaning) (O : owners) (owner : Nat) : Option meaning :=
  (O owner).bind (observe S)

end catalog

namespace owners

/-- Replacing this owner's root cannot mutate another owner's snapshot. -/
def adopt (O : owners) (owner root : Nat) : owners :=
  fun j => if j = owner then some root else O j

@[simp] theorem adopt_here (O : owners) (owner root : Nat) :
    adopt O owner root owner = some root := by simp [adopt]

@[simp] theorem adopt_other (O : owners) (owner root other : Nat) (h : other ≠ owner) :
    adopt O owner root other = O other := by simp [adopt, h]

variable {meaning : Type u}

/-- A retained snapshot's entire old dependency graph remains pinned. -/
theorem snapshot_retention {S T : catalog meaning} (extension : catalog.extends_catalog T S)
    (O : owners) (current new_root snapshot old_root id : Nat)
    (different : snapshot ≠ current) (held : O snapshot = some old_root)
    (path : catalog.reaches S old_root id) :
    catalog.pinned T (adopt O current new_root) id := by
  exact ⟨snapshot, old_root, by simpa [different] using held,
    catalog.extends_reaches extension path⟩

theorem snapshot_read_unchanged {S T : catalog meaning}
    (extension : catalog.extends_catalog T S) (O : owners)
    (current new_root snapshot old_root : Nat) (b : blob meaning)
    (different : snapshot ≠ current) (held : O snapshot = some old_root)
    (found : S old_root = some b) :
    catalog.read T (adopt O current new_root) snapshot = catalog.read S O snapshot := by
  simp [catalog.read, catalog.observe, different, held, found, extension _ _ found]

/-- Adoption preserves closure when the new exact target graph is complete. -/
theorem adopt_ready {S : catalog meaning} {O : owners} (wf : catalog.well_formed S O)
    (current new_root : Nat) (ready : catalog.ready S new_root) :
    catalog.well_formed S (adopt O current new_root) := by
  intro owner root held
  by_cases same : owner = current
  · subst owner
    have eq_root : new_root = root := by simpa using held
    subst root
    exact ready
  · apply wf owner root
    simpa [same] using held

/-- The semantic rewrite is an explicit premise. Both roots must exist;
equality of two missing lookups cannot justify adoption. -/
theorem independent_adoption (S : catalog meaning) (O : owners)
    (current old_root new_root : Nat) (old_blob new_blob : blob meaning)
    (held : O current = some old_root) (old_found : S old_root = some old_blob)
    (new_found : S new_root = some new_blob)
    (rewrite_correct : new_blob.payload = old_blob.payload) :
    catalog.read S (adopt O current new_root) current = catalog.read S O current := by
  simp [catalog.read, catalog.observe, held, old_found, new_found, rewrite_correct]

end owners
end Everett
