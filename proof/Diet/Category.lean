/-
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-/
import Std

namespace Diet

universe u v

/-- The policy assumptions for one key. `then` follows chronological order. -/
structure category (object : Type u) where
  hom : object → object → Type v
  ident : (x : object) → hom x x
  comp : {x y z : object} → hom x y → hom y z → hom x z
  ident_comp : ∀ {x y} (f : hom x y), comp (ident x) f = f
  comp_ident : ∀ {x y} (f : hom x y), comp f (ident y) = f
  assoc : ∀ {w x y z} (f : hom w x) (g : hom x y) (h : hom y z),
    comp (comp f g) h = comp f (comp g h)

/-- Typed endpoints make noncomposable histories unrepresentable. -/
inductive history {object : Type u} (C : category.{u,v} object) : object → object → Type (max u v)
  | nil (x) : history C x x
  | cons {x y z} (first : C.hom x y) (rest : history C y z) : history C x z

namespace history

variable {object : Type u} {C : category.{u,v} object}

def eval {x y} : history C x y → C.hom x y
  | .nil x => C.ident x
  | .cons f rest => C.comp f (eval rest)

def append {x y z} : history C x y → history C y z → history C x z
  | .nil _, rest => rest
  | .cons f tail, rest => .cons f (append tail rest)

theorem eval_append {x y z} (a : history C x y) (b : history C y z) :
    eval (append a b) = C.comp (eval a) (eval b) := by
  induction a with
  | nil => exact (C.ident_comp _).symm
  | cons f rest ih =>
    simp only [append, eval, ih]
    exact (C.assoc _ _ _).symm

/-- Adjacent contraction preserves the full composite, not just endpoints. -/
theorem merge_front {w x y z} (f : C.hom w x) (g : C.hom x y)
    (tail : history C y z) :
    eval (.cons f (.cons g tail)) = eval (.cons (C.comp f g) tail) := by
  exact (C.assoc f g (eval tail)).symm

theorem merge_in_context {a b c d e} (prior : history C a b)
    (f : C.hom b c) (g : C.hom c d) (suffix : history C d e) :
    eval (append prior (.cons f (.cons g suffix))) =
      eval (append prior (.cons (C.comp f g) suffix)) := by
  rw [eval_append, eval_append, merge_front]

/-- A binary merge tree records reassociation without allowing permutation. -/
inductive tree : object → object → Type (max u v)
  | leaf {x y} (f : C.hom x y) : tree x y
  | join {x y z} (left : tree x y) (right : tree y z) : tree x z

namespace tree

def eval {x y} : tree (C := C) x y → C.hom x y
  | .leaf f => f
  | .join a b => C.comp (eval a) (eval b)

def flatten {x y} : tree (C := C) x y → history C x y
  | .leaf f => .cons f (.nil _)
  | .join a b => append (flatten a) (flatten b)

theorem eval_flatten {x y} (t : tree (C := C) x y) :
    history.eval (flatten t) = eval t := by
  induction t with
  | leaf f => exact C.comp_ident f
  | join a b ha hb => simp only [flatten, eval_append, eval, ha, hb]

theorem reassociation {x y} (a b : tree (C := C) x y)
    (same_chronology : flatten a = flatten b) : eval a = eval b := by
  rw [← eval_flatten, ← eval_flatten, same_chronology]

end tree
end history

/-- The replacement instance permits any endpoints; the runtime checks the source. -/
def replacement_category (object : Type u) : category object where
  hom := fun _ _ => Unit
  ident := fun _ => ()
  comp := fun _ _ => ()
  ident_comp := fun f => by cases f; rfl
  comp_ident := fun f => by cases f; rfl
  assoc := fun _ _ _ => rfl

/-- A concrete noncommutative instance: ordered event words at one object. -/
def word_category : category Unit where
  hom := fun _ _ => List Nat
  ident := fun _ => []
  comp := List.append
  ident_comp := List.nil_append
  comp_ident := List.append_nil
  assoc := List.append_assoc

theorem same_key_order_matters :
    word_category.comp (x := ()) (y := ()) (z := ()) [1] [2] ≠
      word_category.comp (x := ()) (y := ()) (z := ()) [2] [1] := by
  change ([1, 2] : List Nat) ≠ [2, 1]
  decide

end Diet
