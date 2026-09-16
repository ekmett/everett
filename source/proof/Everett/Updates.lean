/-
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-/
import Everett.Category

namespace Everett

universe u v w

/-- A key's object type, and subsequently its category, may depend on that key. -/
abbrev world {key : Type u} (state : key → Type v) := (k : key) → state k

/-- Componentwise composition preserves each key's entire arrow, including loops. -/
def world_category {key : Type u} {state : key → Type v}
    (C : (k : key) → category.{v,w} (state k)) : category (world state) where
  hom := fun X Y => (k : key) → (C k).hom (X k) (Y k)
  ident := fun X k => (C k).ident (X k)
  comp := fun f g k => (C k).comp (f k) (g k)
  ident_comp := fun f => by funext k; exact (C k).ident_comp (f k)
  comp_ident := fun f => by funext k; exact (C k).comp_ident (f k)
  assoc := fun f g h => by funext k; exact (C k).assoc (f k) (g k) (h k)

/-- The correctly typed commuting square at two independent coordinates. -/
theorem disjoint_arrow_square {A : Type u} {B : Type v}
    (C : category A) (D : category B) {x x' : A} {y y' : B}
    (f : C.hom x x') (g : D.hom y y') :
    (C.comp f (C.ident x'), D.comp (D.ident y) g) =
      (C.comp (C.ident x) f, D.comp g (D.ident y')) := by
  rw [C.comp_ident, C.ident_comp, D.ident_comp, D.comp_ident]

variable {key : Type u} [DecidableEq key] {state : key → Type v}

/-- A dependent functional update changes exactly one coordinate. -/
def put (W : world state) (k : key) (value : state k) : world state :=
  fun j => if h : j = k then h.symm ▸ value else W j

@[simp] theorem put_same (W : world state) (k : key) (value : state k) :
    put W k value k = value := by simp [put]

@[simp] theorem put_other (W : world state) (k j : key) (value : state k)
    (different : j ≠ k) : put W k value j = W j := by simp [put, different]

theorem put_commute (W : world state) (a b : key) (va : state a) (vb : state b)
    (different : a ≠ b) : put (put W a va) b vb = put (put W b vb) a va := by
  funext k
  by_cases ha : k = a
  · subst k; simp [put, different, Ne.symm different]
  · by_cases hb : k = b
    · subst k; simp [put, different, Ne.symm different]
    · simp [put, ha, hb]

/-- The hom carries the policy's admissibility evidence, not a weak signature. -/
structure mutation (C : (k : key) → category.{v,w} (state k)) where
  key : key
  source : state key
  target : state key
  arrow : (C key).hom source target

namespace mutation

variable {C : (k : key) → category.{v,w} (state k)}
variable [∀ k, DecidableEq (state k)]

def valid (W : world state) (m : mutation C) : Prop := W m.key = m.source

/-- Admission checks the exact source before installing the typed target.
This executable state projection does not itself retain or evaluate the arrow;
`world_category` and `history` carry full arrow semantics separately. -/
def apply (W : world state) (m : mutation C) : Option (world state) :=
  if W m.key = m.source then some (put W m.key m.target) else none

theorem apply_valid (W : world state) (m : mutation C) (h : valid W m) :
    apply W m = some (put W m.key m.target) := by simp [apply, valid] at *; assumption

theorem apply_invalid (W : world state) (m : mutation C) (h : ¬ valid W m) :
    apply W m = none := by simp [apply, valid] at *; assumption

theorem admitted_source (W W' : world state) (m : mutation C)
    (h : apply W m = some W') : valid W m := by
  by_cases source : W m.key = m.source
  · exact source
  · simp [apply, source] at h

theorem admitted_target (W W' : world state) (m : mutation C)
    (h : apply W m = some W') : W' m.key = m.target := by
  have hv := admitted_source W W' m h
  rw [apply_valid W m hv] at h
  cases Option.some.inj h
  exact put_same _ _ _

def apply_two (W : world state) (a b : mutation C) : Option (world state) :=
  (apply W a).bind (fun W' => apply W' b)

/-- Distinct keys remain applicable in either order against the same base. -/
theorem disjoint_commute (W : world state) (a b : mutation C)
    (different : a.key ≠ b.key) (ha : valid W a) (hb : valid W b) :
    apply_two W a b = apply_two W b a := by
  have hab : valid (put W a.key a.target) b := by
    simpa [valid, put, Ne.symm different] using hb
  have hba : valid (put W b.key b.target) a := by
    simpa [valid, put, different] using ha
  simp only [apply_two, apply_valid W a ha, apply_valid W b hb, Option.bind,
    apply_valid _ b hab, apply_valid _ a hba]
  rw [put_commute W a.key b.key a.target b.target different]

end mutation
end Everett
