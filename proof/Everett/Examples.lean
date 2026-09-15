/-
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-/
import Everett.Updates
import Everett.Fingerprint
import Everett.Adoption

namespace Everett.examples

abbrev key_state : Bool → Type
  | false => Nat
  | true => Bool

instance (k : Bool) : DecidableEq (key_state k) := by
  cases k <;> simp only [key_state] <;> infer_instance

def policies (k : Bool) := replacement_category (key_state k)

def initial : world key_state
  | false => 0
  | true => false

def increment : mutation policies := ⟨false, 0, 3, ()⟩
def enable : mutation policies := ⟨true, false, true, ()⟩
def stale : mutation policies := ⟨false, 99, 3, ()⟩

def observation (W : Option (world key_state)) : Option (Nat × Bool) :=
  W.map (fun X => (X false, X true))

example : mutation.apply_two initial increment enable =
    mutation.apply_two initial enable increment :=
  mutation.disjoint_commute initial increment enable (by decide) rfl rfl

example : observation (mutation.apply_two initial increment enable) = some (3, true) := by
  decide

example : mutation.apply initial stale = none :=
  mutation.apply_invalid initial stale (by change (0 : Nat) ≠ 99; decide)

/-- Same-key source checks retain chronology: swapping these updates fails. -/
def next : mutation policies := ⟨false, 3, 5, ()⟩

example : observation (mutation.apply_two initial increment next) = some (5, false) := by decide
example : mutation.apply_two initial next increment = none := by rfl

def leaf_word (word : List Nat) : history.tree (C := word_category) () () := .leaf word

def left_tree : history.tree (C := word_category) () () :=
  .join (.join (leaf_word [1]) (leaf_word [2])) (leaf_word [3])
def right_tree : history.tree (C := word_category) () () :=
  .join (leaf_word [1]) (.join (leaf_word [2]) (leaf_word [3]))

example : history.tree.eval left_tree = history.tree.eval right_tree :=
  history.tree.reassociation left_tree right_tree rfl

/-- The old and new roots use different immutable index and target versions. -/
def old_tail : blob (List Nat) := ⟨10, 20, none, [2]⟩
def old_head : blob (List Nat) := ⟨11, 21, some 0, [1, 2]⟩
def new_tail : blob (List Nat) := ⟨10, 22, none, [2]⟩
def new_head : blob (List Nat) := ⟨11, 23, some 2, [1, 2]⟩
def old_catalog : catalog (List Nat) :=
  catalog.install (catalog.install (fun _ => none) 0 old_tail) 1 old_head
def new_catalog : catalog (List Nat) :=
  catalog.install (catalog.install old_catalog 2 new_tail) 3 new_head

def held_roots : owners := fun owner => if owner = 0 ∨ owner = 1 then some 1 else none

theorem catalog_grows : catalog.extends_catalog new_catalog old_catalog := by
  intro id b found
  apply catalog.install_fresh_extends (catalog.install old_catalog 2 new_tail) 3 new_head
    (by decide) id b
  exact catalog.install_fresh_extends old_catalog 2 new_tail (by decide) id b found

example : catalog.read new_catalog (owners.adopt held_roots 0 3) 1 = some [1, 2] := by decide
example : new_catalog 1 = some old_head := by decide
example : new_catalog 3 = some new_head := by decide

/-- The snapshot still follows old head 1 to old target 0 after current adopts 3. -/
theorem old_target_retained :
    catalog.pinned new_catalog (owners.adopt held_roots 0 3) 0 := by
  apply owners.snapshot_retention catalog_grows held_roots 0 3 1 1 0 (by decide) (by decide)
  exact .target .root old_head (by decide) rfl

example : ¬ catalog.eligible new_catalog (owners.adopt held_roots 0 3) [0] := by
  intro eligible
  exact eligible 0 (by simp) old_target_retained

/-- A permitted deletion of an unrelated object preserves both snapshot roots. -/
def unused : blob (List Nat) := ⟨90, 91, none, []⟩
def with_unused := catalog.install new_catalog 9 unused

example : catalog.reclaim with_unused [9] 1 = some old_head := by decide
example : catalog.reclaim with_unused [9] 3 = some new_head := by decide

end Everett.examples
