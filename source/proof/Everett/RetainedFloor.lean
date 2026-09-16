/-
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-/
import Std

namespace Everett.retained_floor

/-! A canceled record can still own literal units inherited by surviving keys.
The replacement carries those units literally by capping its retained prefix at
the exact target's stored retention. Positions use one common key grammar and
unit. The model constructs certificates from the known complete key; it does
not assume that equal hashes establish key or physical-source equality. -/

structure target (α : Type) where
  file : Nat
  ordinal : Nat
  key : List α
  retained : Nat
  deriving Repr

def target.literal (a : target α) : List α := a.key.drop a.retained

structure certificate (α : Type) where
  file : Nat
  ordinal : Nat
  target_retained : Nat
  cap : Nat
  literal : List α
  deriving Repr

def cap (natural retained : Nat) : Nat := min natural retained

def extra (natural retained : Nat) : Nat := natural - cap natural retained

def certify (a : target α) (natural : Nat) : certificate α :=
  { file := a.file
    ordinal := a.ordinal
    target_retained := a.retained
    cap := cap natural a.retained
    literal := a.key.drop (cap natural a.retained) }

/-- Physical identity and stored retention are part of the alias contract.
Immutability/non-reuse of these identities belongs to the catalog model. -/
def bound_to (c : certificate α) (a : target α) : Prop :=
  c.file = a.file ∧ c.ordinal = a.ordinal ∧ c.target_retained = a.retained

theorem certify_bound_to (a : target α) (natural : Nat) :
    bound_to (certify a natural) a := by
  simp [bound_to, certify]

theorem cap_le_natural (natural retained : Nat) : cap natural retained ≤ natural :=
  Nat.min_le_left _ _

theorem cap_le_target (natural retained : Nat) : cap natural retained ≤ retained :=
  Nat.min_le_right _ _

theorem extra_eq (natural retained : Nat) :
    extra natural retained = natural - retained := by
  simp only [extra, cap]
  omega

/-- A relative control backs up farther by exactly the additional literal
length. Its predecessor must already support the ordinary retained position. -/
theorem backspace_increase (previous natural retained : Nat)
    (valid : natural ≤ previous) :
    previous - cap natural retained = previous - natural + extra natural retained := by
  have h := cap_le_natural natural retained
  simp only [extra]
  omega

theorem literal_growth (a : target α) (natural : Nat)
    (valid : natural ≤ a.key.length) :
    (certify a natural).literal.length =
      (a.key.drop natural).length + extra natural a.retained := by
  have h := cap_le_natural natural a.retained
  simp only [certify, List.length_drop, extra]
  omega

/-- Charge only the newly repeated units to the target's existing literals.
Control-code length, values, indexes and output allocation are separate costs. -/
theorem extra_le_target_literal (a : target α) (natural : Nat)
    (valid : natural ≤ a.key.length) :
    extra natural a.retained ≤ a.literal.length := by
  rw [extra_eq]
  simp only [target.literal, List.length_drop]
  omega

/-- Complete old literal coverage, without reconstructing the omitted prefix. -/
theorem target_literal_covered (a : target α) (natural : Nat) :
    (certify a natural).literal.drop (a.retained - (certify a natural).cap) =
      a.literal := by
  have h := cap_le_target natural a.retained
  simp only [certify, target.literal, List.drop_drop]
  congr 1
  omega

/-- Every real target-owned key position has an in-bounds replacement address. -/
theorem alias_in_bounds (a : target α) (natural position : Nat)
    (owned : a.retained ≤ position) (inside : position < a.key.length) :
    position - (certify a natural).cap < (certify a natural).literal.length := by
  have h := cap_le_target natural a.retained
  simp only [certify, List.length_drop]
  omega

theorem alias_lookup (a : target α) (natural position : Nat)
    (owned : a.retained ≤ position) :
    (certify a natural).literal[position - (certify a natural).cap]? =
      a.key[position]? := by
  have h := cap_le_target natural a.retained
  simp only [certify, List.getElem?_drop]
  congr 1
  omega

/-- A requested fragment keeps its exact source and literal ownership boundary.
The caller's owner lookup supplies this relationship; a keep mask alone does not. -/
structure fragment (α : Type) where
  source : target α
  natural : Nat
  position : Nat
  count : Nat
  deriving Repr

def fragment.original (f : fragment α) : List α :=
  (f.source.key.drop f.position).take f.count

def fragment.rescued (f : fragment α) : List α :=
  let c := certify f.source f.natural
  (c.literal.drop (f.position - c.cap)).take f.count

def fragment.owned (f : fragment α) : Prop := f.source.retained ≤ f.position

def fragment.in_bounds (f : fragment α) : Prop :=
  f.position + f.count ≤ f.source.key.length

theorem fragment_covered (f : fragment α) (owned : f.owned) :
    f.rescued = f.original := by
  have h := cap_le_target f.natural f.source.retained
  have hp : cap f.natural f.source.retained +
      (f.position - cap f.natural f.source.retained) = f.position := by
    unfold fragment.owned at owned
    omega
  simp only [fragment.rescued, fragment.original, certify, List.drop_drop, hp]

theorem fragment_length (f : fragment α) (owned : f.owned) (inside : f.in_bounds) :
    f.rescued.length = f.count := by
  rw [fragment_covered f owned]
  simp only [fragment.original, List.length_take, List.length_drop]
  unfold fragment.in_bounds at inside
  omega

/-- Any finite gather schedule is preserved, including fragments from several
adjacent canceled records. The theorem does not invent or validate the schedule. -/
theorem fragments_covered (fs : List (fragment α))
    (owned : ∀ f ∈ fs, f.owned) :
    (fs.map fragment.rescued).flatten = (fs.map fragment.original).flatten := by
  induction fs with
  | nil => rfl
  | cons f fs ih =>
    have here := fragment_covered f (owned f (by simp))
    have rest := ih (fun g hg => owned g (by simp [hg]))
    simp only [List.map_cons, List.flatten_cons, here, rest]

/-- Each row issues one certificate for one canceled physical occurrence. -/
structure admission (α : Type) where
  source : target α
  natural : Nat

def admission.valid (a : admission α) : Prop := a.natural ≤ a.source.key.length
def admission.extra (a : admission α) : Nat :=
  Everett.retained_floor.extra a.natural a.source.retained
def admission.budget (a : admission α) : Nat := a.source.literal.length
def admission.identity (a : admission α) : Nat × Nat := (a.source.file, a.source.ordinal)

/-- The arithmetic counts the supplied targets once per row. To charge a set of
physical records once, the caller must also establish unique target identities. -/
theorem batch_charge (as : List (admission α))
    (valid : ∀ a ∈ as, a.valid) :
    (as.map admission.extra).sum ≤ (as.map admission.budget).sum := by
  induction as with
  | nil => simp
  | cons a as ih =>
    have here := extra_le_target_literal a.source a.natural (valid a (by simp))
    have rest := ih (fun b hb => valid b (by simp [hb]))
    simp only [List.map_cons, List.sum_cons]
    change extra a.natural a.source.retained + _ ≤ a.source.literal.length + _
    omega

def admitted_once (as : List (admission α)) : Prop :=
  (as.map admission.identity).Nodup

/-- One charged target's omitted prefix cannot silently be retargeted to a
different physical encoding, even if its complete logical key is unchanged. -/
theorem changed_retention_rejected (a b : target α) (natural : Nat)
    (changed : a.retained ≠ b.retained) :
    ¬bound_to (certify a natural) b := by
  intro h
  exact changed h.2.2

def example_target : target Nat := ⟨7, 3, [0, 1, 1, 0, 1, 1], 2⟩

example : (certify example_target 5).literal = [1, 0, 1, 1] := by decide
example : extra 5 2 = 3 := by decide
example : (certify example_target 5).literal[4 - 2]? = some 1 := by decide
example : extra 3 5 = 0 := by decide
example : extra 0 0 = 0 := by decide
example : (certify (⟨8, 0, [], 0⟩ : target Nat) 0).literal = [] := by decide

/-- Two adjacent canceled owners contribute separate pieces. The second
tombstone is deliberately not a complete anchor for the first piece. -/
def adjacent_fragments : List (fragment Nat) :=
  [ ⟨⟨9, 1, [0, 1, 1, 0, 0, 0], 1⟩, 4, 1, 2⟩,
    ⟨⟨9, 2, [0, 1, 1, 1, 0, 0], 3⟩, 5, 3, 2⟩ ]

example : (adjacent_fragments.map fragment.rescued).flatten = [1, 1, 1, 0] := by decide
example : ∀ f ∈ adjacent_fragments, f.owned ∧ f.in_bounds := by
  simp [adjacent_fragments, fragment.owned, fragment.in_bounds]
example : ¬bound_to (certify example_target 5)
    { example_target with retained := 1 } := by
  simp [bound_to, certify, example_target]

end Everett.retained_floor
