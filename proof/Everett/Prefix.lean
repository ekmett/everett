/-
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-/
import Std

namespace Everett.prefix

local instance : Std.Irrefl (fun a b : Nat => a < b) := ⟨by intro a; omega⟩
local instance : Std.Asymm (fun a b : Nat => a < b) := ⟨by intro a b; omega⟩
local instance : Trans (fun a b : Nat => ¬ a < b) (fun a b : Nat => ¬ a < b)
    (fun a b : Nat => ¬ a < b) := ⟨by intro a b c; omega⟩

/-! Finite strings use natural-number symbols and Lean's actual lexicographic
list order: the first unequal symbol decides, and a proper prefix is smaller.
No prefix-free encoding or nonempty-string assumption is imposed.

These lemmas concern strings and comparison state. The existing fractional
index model uses natural-number keys; connecting its routed cuts and exact
sample identities to these strings remains a separate topology/refinement
proof. No encoded layout, byte access or persistence fact is asserted here. -/

/-- The length of the actual longest common prefix, including string endpoints. -/
def lcp : List Nat → List Nat → Nat
  | a :: as, b :: bs => if a = b then lcp as bs + 1 else 0
  | _, _ => 0

@[simp] theorem lcp_nil_left (xs : List Nat) : lcp [] xs = 0 := by cases xs <;> rfl
@[simp] theorem lcp_nil_right (xs : List Nat) : lcp xs [] = 0 := by cases xs <;> rfl

@[simp] theorem lcp_self (xs : List Nat) : lcp xs xs = xs.length := by
  induction xs with
  | nil => rfl
  | cons a xs ih => simp [lcp, ih]

theorem lcp_le_left (xs ys : List Nat) : lcp xs ys ≤ xs.length := by
  induction xs generalizing ys with
  | nil => simp
  | cons a xs ih =>
    cases ys with
    | nil => simp
    | cons b ys =>
      by_cases same : a = b
      · simpa [lcp, same] using Nat.add_le_add_right (ih ys) 1
      · simp [lcp, same]

theorem lcp_comm (xs ys : List Nat) : lcp xs ys = lcp ys xs := by
  induction xs generalizing ys with
  | nil => simp
  | cons a xs ih =>
    cases ys with
    | nil => simp
    | cons b ys =>
      by_cases same : a = b
      · subst b; simp [lcp, ih]
      · simp [lcp, same, Ne.symm same]

theorem lcp_le_right (xs ys : List Nat) : lcp xs ys ≤ ys.length := by
  rw [lcp_comm]
  exact lcp_le_left ys xs

/-- The computed prefix really is shared by both strings. -/
theorem lcp_prefix (xs ys : List Nat) :
    xs.take (lcp xs ys) <+: xs ∧ xs.take (lcp xs ys) <+: ys := by
  constructor
  · exact List.take_prefix _ _
  · induction xs generalizing ys with
    | nil => simp
    | cons a xs ih =>
      cases ys with
      | nil => simp
      | cons b ys =>
        by_cases same : a = b
        · subst b; simpa [lcp] using ih ys
        · simp [lcp, same]

/-- Every shared prefix is no longer than the computed one. -/
theorem common_prefix_bound (p xs ys : List Nat)
    (px : p <+: xs) (py : p <+: ys) : p.length ≤ lcp xs ys := by
  induction p generalizing xs ys with
  | nil => simp
  | cons a p ih =>
    cases xs with
    | nil => simp at px
    | cons x xs =>
      cases ys with
      | nil => simp at py
      | cons y ys =>
        obtain ⟨same_x, tail_x⟩ := List.cons_prefix_cons.mp px
        obtain ⟨same_y, tail_y⟩ := List.cons_prefix_cons.mp py
        subst x
        subst y
        simpa [lcp] using Nat.add_le_add_right (ih xs ys tail_x tail_y) 1

/-- A prefix defines an interval in lexicographic order, including endpoints. -/
theorem prefix_interval (p a b c : List Nat)
    (ab : a ≤ b) (bc : b ≤ c) (pa : p <+: a) (pc : p <+: c) : p <+: b := by
  induction p generalizing a b c with
  | nil => simp
  | cons x p ih =>
    cases a with
    | nil => simp at pa
    | cons a as =>
      cases c with
      | nil => simp at pc
      | cons c cs =>
        obtain ⟨same_a, tail_a⟩ := List.cons_prefix_cons.mp pa
        obtain ⟨same_c, tail_c⟩ := List.cons_prefix_cons.mp pc
        subst a
        subst c
        cases b with
        | nil => simp at ab
        | cons b bs =>
          have lower := List.cons_le_cons_iff.mp ab
          have upper := List.cons_le_cons_iff.mp bc
          have same : x = b := by omega
          subst b
          have tails := ih as bs cs (List.le_of_cons_le_cons ab)
            (List.le_of_cons_le_cons bc) tail_a tail_c
          simpa using tails

/-- The LCP of ordered endpoints is the smaller adjacent LCP. This includes
equal strings, empty strings, and cases where an endpoint is a proper prefix. -/
theorem ordered_lcp_min (c b q : List Nat) (cb : c ≤ b) (bq : b ≤ q) :
    lcp c q = min (lcp c b) (lcp b q) := by
  induction c generalizing b q with
  | nil => simp
  | cons c cs ih =>
    cases b with
    | nil => simp at cb
    | cons b bs =>
      cases q with
      | nil => simp at bq
      | cons q qs =>
        by_cases ends : c = q
        · subst q
          have lower := List.cons_le_cons_iff.mp cb
          have upper := List.cons_le_cons_iff.mp bq
          have same : c = b := by omega
          subst b
          have tails := ih bs qs (List.le_of_cons_le_cons cb) (List.le_of_cons_le_cons bq)
          simp only [lcp, ↓reduceIte]
          omega
        · by_cases first : c = b
          · subst b; simp [lcp, ends]
          · simp [lcp, ends, first]

/-- Equality needs both endpoint lengths, not a mismatch position alone. -/
theorem lcp_full_iff (xs ys : List Nat) :
    xs = ys ↔ lcp xs ys = xs.length ∧ lcp xs ys = ys.length := by
  induction xs generalizing ys with
  | nil => cases ys <;> simp [lcp]
  | cons x xs ih =>
    cases ys with
    | nil => simp [lcp]
    | cons y ys =>
      by_cases same : x = y
      · subst y; simpa [lcp] using ih ys
      · simp [lcp, same]

/-- Exact cut-local LCP metadata repairs the outgoing frontier's comparison
state. The frontier is at most the query; the recovered LCP and both full
lengths decide whether equality holds. The metadata equalities state which
measured adjacent LCPs are supplied, not the conclusion being recovered. -/
theorem frontier_recovery (c b q : List Nat) (cb : c ≤ b) (bq : b ≤ q)
    (cut_lcp incoming_lcp : Nat) (cut_exact : cut_lcp = lcp c b)
    (incoming_exact : incoming_lcp = lcp b q) :
    c ≤ q ∧ lcp c q = min cut_lcp incoming_lcp ∧
      (c = q ↔ min cut_lcp incoming_lcp = c.length ∧
        min cut_lcp incoming_lcp = q.length) := by
  subst cut_lcp
  subst incoming_lcp
  have repair := ordered_lcp_min c b q cb bq
  exact ⟨List.le_trans cb bq, repair, repair ▸ lcp_full_iff c q⟩

-- Kernel-checked endpoint and interior examples; no native_decide shortcut.
example : lcp [] [1, 2] = 0 := by decide
example : lcp [1] [1, 2] = 1 := by decide
example : lcp [1, 2] [1, 2] = 2 := by decide
example : lcp [1, 2, 3] [1, 4] = 1 := by decide
example : [1] ≤ ([1, 2] : List Nat) := by decide
example : lcp [1] [1, 3] = min (lcp [1] [1, 2]) (lcp [1, 2] [1, 3]) :=
  ordered_lcp_min _ _ _ (by decide) (by decide)

end Everett.prefix
