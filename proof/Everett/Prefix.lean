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

/-- Without an ordering hypothesis, equality needs both endpoint lengths. -/
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

/-- For a known lower key, matching the entire query already establishes
equality. Its own full length is unnecessary. -/
theorem lcp_full_right_iff_of_le (xs ys : List Nat) (ordered : xs ≤ ys) :
    xs = ys ↔ lcp xs ys = ys.length := by
  induction xs generalizing ys with
  | nil => cases ys <;> simp
  | cons x xs ih =>
    cases ys with
    | nil => simp at ordered
    | cons y ys =>
      by_cases same : x = y
      · subst y
        simpa [lcp] using ih ys (List.le_of_cons_le_cons ordered)
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

/-- An ordered borrowed frontier can be repaired using only exact LCPs and
the query length, with no preceding-key length checkpoint. -/
theorem frontier_recovery_without_length (c b q : List Nat) (cb : c ≤ b) (bq : b ≤ q)
    (cut_lcp incoming_lcp : Nat) (cut_exact : cut_lcp = lcp c b)
    (incoming_exact : incoming_lcp = lcp b q) :
    c ≤ q ∧ lcp c q = min cut_lcp incoming_lcp ∧
      (c = q ↔ min cut_lcp incoming_lcp = q.length) := by
  subst cut_lcp
  subst incoming_lcp
  have cq := List.le_trans cb bq
  have repair := ordered_lcp_min c b q cb bq
  have equality := lcp_full_right_iff_of_le c q cq
  rw [repair] at equality
  exact ⟨cq, repair, equality⟩

/-- A merge that emits each distinct key can only increase the retained
prefix relative to an input frame. Skipped output keys require another premise. -/
theorem merge_retained_le (previous_input previous_output key : List Nat)
    (before : previous_input ≤ previous_output) (after : previous_output ≤ key)
    (retained : Nat) (valid : retained ≤ lcp previous_input key) :
    retained ≤ lcp previous_output key := by
  rw [ordered_lcp_min previous_input previous_output key before after] at valid
  exact Nat.le_trans valid (Nat.min_le_right _ _)

/-- The output literal is a suffix of the current input literal whenever
the output retains at least as much prefix. -/
theorem merge_literal_suffix (key : List Nat) (input_retained output_retained : Nat)
    (within : input_retained ≤ output_retained) :
    key.drop output_retained = (key.drop input_retained).drop (output_retained - input_retained) := by
  rw [List.drop_drop]
  congr 1
  omega

/-- The last key of a walk with a supplied first key. An empty tail leaves
that first key as the endpoint. -/
def walk_last (first : List Nat) : List (List Nat) → List Nat
  | [] => first
  | next :: rest => walk_last next rest

/-- Exact LCPs of consecutive pairs, in traversal order. A singleton walk
has no adjacent pairs. -/
def adjacent_lcps (first : List Nat) : List (List Nat) → List Nat
  | [] => []
  | next :: rest => lcp first next :: adjacent_lcps next rest

theorem ordered_walk_last (first : List Nat) (rest : List (List Nat))
    (ordered : (first :: rest).Pairwise (fun a b => a ≤ b)) :
    first ≤ walk_last first rest := by
  induction rest generalizing first with
  | nil => exact List.le_refl first
  | cons next rest ih =>
    have parts := List.pairwise_cons.mp ordered
    exact List.le_trans (parts.1 next (by simp)) (ih next parts.2)

/-- Left-to-right minimum accumulation preserves an existing cap. Bounding
that cap by the first key's length gives the correct zero-step convention. -/
theorem accumulated_lcp_min (first : List Nat) (rest : List (List Nat)) (cap : Nat)
    (ordered : (first :: rest).Pairwise (fun a b => a ≤ b)) (bounded : cap ≤ first.length) :
    (adjacent_lcps first rest).foldl min cap = min cap (lcp first (walk_last first rest)) := by
  induction rest generalizing first cap with
  | nil => simp [adjacent_lcps, walk_last, Nat.min_eq_left bounded]
  | cons next rest ih =>
    have parts := List.pairwise_cons.mp ordered
    have first_next := parts.1 next (by simp)
    have next_last := ordered_walk_last next rest parts.2
    have next_cap : min cap (lcp first next) ≤ next.length :=
      Nat.le_trans (Nat.min_le_right _ _) (lcp_le_right first next)
    simp only [adjacent_lcps, List.foldl_cons, walk_last]
    rw [ih next (min cap (lcp first next)) parts.2 next_cap, Nat.min_assoc,
      ← ordered_lcp_min first next (walk_last next rest) first_next next_last]

/-- After the first transition its exact adjacent LCP supplies the initial
cap. No artificial infinity or maximum-length sentinel is needed. -/
theorem adjacent_min_eq_endpoints (first next : List Nat) (rest : List (List Nat))
    (ordered : (first :: next :: rest).Pairwise (fun a b => a ≤ b)) :
    (adjacent_lcps next rest).foldl min (lcp first next) = lcp first (walk_last next rest) := by
  have parts := List.pairwise_cons.mp ordered
  rw [accumulated_lcp_min next rest (lcp first next) parts.2 (lcp_le_right first next)]
  exact (ordered_lcp_min first next (walk_last next rest)
    (parts.1 next (by simp)) (ordered_walk_last next rest parts.2)).symm

/-- Empty walk convention: zero. Singleton convention: that key's full length,
which is its self-LCP. Longer walks accumulate minima of exact adjacent LCPs.
These are string laws, not a refinement proof for a C++ cursor or codec. -/
def walk_min : List (List Nat) → Nat
  | [] => 0
  | first :: rest => (adjacent_lcps first rest).foldl min first.length

@[simp] theorem walk_min_empty : walk_min [] = 0 := rfl
@[simp] theorem walk_min_singleton (key : List Nat) : walk_min [key] = key.length := rfl

theorem walk_min_eq_endpoints (first : List Nat) (rest : List (List Nat))
    (ordered : (first :: rest).Pairwise (fun a b => a ≤ b)) :
    walk_min (first :: rest) = lcp first (walk_last first rest) := by
  rw [walk_min, accumulated_lcp_min first rest first.length ordered (Nat.le_refl _)]
  exact Nat.min_eq_right (lcp_le_left first (walk_last first rest))

example : walk_min [[], [1], [1], [1, 2]] = 0 := by decide
example : walk_min [[1], [1], [1, 2], [1, 2, 3]] = 1 := by decide
example : walk_min [[1, 2], [1, 2], [1, 2]] = 2 := by decide
example : walk_min [[1, 2], [1, 3], [2]] = 0 := by decide

-- Kernel-checked endpoint and interior examples; no native_decide shortcut.
example : lcp [] [1, 2] = 0 := by decide
example : lcp [1] [1, 2] = 1 := by decide
example : lcp [1, 2] [1, 2] = 2 := by decide
example : lcp [1, 2, 3] [1, 4] = 1 := by decide
example : [1] ≤ ([1, 2] : List Nat) := by decide
example : lcp [1] [1, 3] = min (lcp [1] [1, 2]) (lcp [1, 2] [1, 3]) :=
  ordered_lcp_min _ _ _ (by decide) (by decide)

end Everett.prefix
