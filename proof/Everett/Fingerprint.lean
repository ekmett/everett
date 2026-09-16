/-
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-/
import Everett.Updates

namespace Everett

universe u v

/-- A concrete additive algebra: exact integers, with no division or injectivity. -/
def delta {object : Type u} (potential : object → Int) (x y : object) : Int :=
  potential y - potential x

@[simp] theorem delta_ident {object : Type u} (p : object → Int) (x : object) :
    delta p x x = 0 := by simp [delta]

theorem delta_comp {object : Type u} (p : object → Int) (x y z : object) :
    delta p x z = delta p x y + delta p y z := by simp only [delta]; omega

namespace history

variable {object : Type u} {C : category.{u,v} object}

def contribution (p : object → Int) {x y} : history C x y → Int
  | .nil _ => 0
  | .cons (y := z) _ tail => delta p x z + contribution p tail

theorem contribution_telescopes (p : object → Int) {x y} (h : history C x y) :
    contribution p h = delta p x y := by
  induction h with
  | nil => simp [contribution]
  | cons f rest ih =>
    simp only [contribution, ih]
    exact (delta_comp p _ _ _).symm

theorem merge_preserves_contribution (p : object → Int) {a b c d e}
    (prior : history C a b) (f : C.hom b c) (g : C.hom c d)
    (suffix : history C d e) :
    contribution p (append prior (.cons f (.cons g suffix))) =
      contribution p (append prior (.cons (C.comp f g) suffix)) := by
  rw [contribution_telescopes, contribution_telescopes]

end history

/-- Enumerating `Fin n` once avoids a finite-map or a summation-library dependency. -/
def finite_sum : (n : Nat) → (Fin n → Int) → Int
  | 0, _ => 0
  | n + 1, f => f 0 + finite_sum n (fun k => f k.succ)

theorem finite_sum_sub (n : Nat) (f g : Fin n → Int) :
    finite_sum n (fun k => f k - g k) = finite_sum n f - finite_sum n g := by
  induction n with
  | zero => simp [finite_sum]
  | succ n ih =>
    simp only [finite_sum]
    rw [ih]
    omega

/-- This first model has a finite dependent key family, enumerated exactly once. -/
def world_fingerprint {n : Nat} {state : Fin n → Type u}
    (potential : (k : Fin n) → state k → Int) (W : world state) : Int :=
  finite_sum n (fun k => potential k (W k))

theorem world_delta_sum {n : Nat} {state : Fin n → Type u}
    (potential : (k : Fin n) → state k → Int) (X Y : world state) :
    finite_sum n (fun k => delta (potential k) (X k) (Y k)) =
      world_fingerprint potential Y - world_fingerprint potential X := by
  exact finite_sum_sub n _ _

/-- Equal endpoint signatures cannot identify an arrow, even without collisions. -/
theorem nonidentity_zero_delta :
    ([7] : word_category.hom () ()) ≠ word_category.ident () ∧
    delta (fun _ : Unit => (0 : Int)) () () = 0 := by
  constructor
  · change ([7] : List Nat) ≠ []
    decide
  · rfl

end Everett
