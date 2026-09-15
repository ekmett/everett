/-
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-/
import Everett.Prefix

namespace Everett.prefix

local instance : Std.Irrefl (fun a b : Nat => a < b) := ⟨by intro a; omega⟩
local instance : Std.Asymm (fun a b : Nat => a < b) := ⟨by intro a b; omega⟩
local instance : Trans (fun a b : Nat => ¬ a < b) (fun a b : Nat => ¬ a < b)
    (fun a b : Nat => ¬ a < b) := ⟨by intro a b c; omega⟩

/-! A sorted merge can carry the LCP from its preceding output to each head.
Unequal LCPs decide which head comes next. Equal LCPs permit a suffix-only
comparison. These are string laws, including equal strings and proper prefixes;
they do not assert a refinement of the C++ cursor or its encoded layout. -/

/-- The head agreeing longer with the preceding output sorts first. -/
theorem frontier_left (p a b : List Nat) (pb : p ≤ b)
    (more : lcp p b < lcp p a) : a < b := by
  apply List.not_le.mp
  intro ba
  have bound := ordered_lcp_min p b a pb ba
  omega

theorem frontier_right (p a b : List Nat) (pa : p ≤ a)
    (more : lcp p a < lcp p b) : b < a :=
  frontier_left p b a pa more

/-- After selecting the left head, the other head's new LCP is known exactly. -/
theorem frontier_left_lcp (p a b : List Nat) (pa : p ≤ a) (pb : p ≤ b)
    (more : lcp p b < lcp p a) : lcp a b = lcp p b := by
  have ab : a ≤ b := List.lt_asymm (frontier_left p a b pb more)
  have relation := ordered_lcp_min p a b pa ab
  omega

theorem frontier_right_lcp (p a b : List Nat) (pa : p ≤ a) (pb : p ≤ b)
    (more : lcp p a < lcp p b) : lcp a b = lcp p a := by
  rw [lcp_comm a b]
  exact frontier_left_lcp p b a pb pa more

theorem append_lcp (p a b : List Nat) :
    lcp (p ++ a) (p ++ b) = p.length + lcp a b := by
  induction p with
  | nil => simp
  | cons x p ih => simp [lcp, ih, Nat.add_assoc, Nat.add_comm, Nat.add_left_comm]

/-- A shared prefix may be removed from both operands without changing order. -/
theorem shared_prefix_lt (p a b : List Nat) (pa : p <+: a) (pb : p <+: b) :
    a < b ↔ a.drop p.length < b.drop p.length := by
  obtain ⟨as, rfl⟩ := pa
  obtain ⟨bs, rfl⟩ := pb
  simp only [List.drop_left]
  induction p with
  | nil => simp
  | cons x p ih => simpa [List.cons_lt_cons_iff] using ih

theorem shared_prefix_lcp (p a b : List Nat) (pa : p <+: a) (pb : p <+: b) :
    lcp a b = p.length + lcp (a.drop p.length) (b.drop p.length) := by
  obtain ⟨as, rfl⟩ := pa
  obtain ⟨bs, rfl⟩ := pb
  simpa using append_lcp p as bs

/-- Equal frontier lengths leave only the two suffixes to compare. -/
theorem frontier_equal_order (p a b : List Nat) (same : lcp p a = lcp p b) :
    a < b ↔ a.drop (lcp p a) < b.drop (lcp p a) := by
  have pa := (lcp_prefix p a).2
  have pb : p.take (lcp p a) <+: b := by simpa [same] using (lcp_prefix p b).2
  have length : (p.take (lcp p a)).length = lcp p a := by
    simp [List.length_take, Nat.min_eq_left (lcp_le_left p a)]
  simpa [length] using shared_prefix_lt (p.take (lcp p a)) a b pa pb

/-- The suffix comparison also recovers the exact new frontier length. -/
theorem frontier_equal_lcp (p a b : List Nat) (same : lcp p a = lcp p b) :
    lcp a b = lcp p a + lcp (a.drop (lcp p a)) (b.drop (lcp p a)) := by
  have pa := (lcp_prefix p a).2
  have pb : p.take (lcp p a) <+: b := by simpa [same] using (lcp_prefix p b).2
  have length : (p.take (lcp p a)).length = lcp p a := by
    simp [List.length_take, Nat.min_eq_left (lcp_le_left p a)]
  simpa [length] using shared_prefix_lcp (p.take (lcp p a)) a b pa pb

-- Equality remains a tie; consuming the native head need not consume its
-- borrowed copies. Empty strings and proper prefixes remain ordinary cases.
example : ¬ ([1, 2] : List Nat) < [1, 2] := by decide
example : ([] : List Nat) < [0] := by decide
example : lcp [1] [1, 2] = 1 := by decide
example : lcp [1, 2] [1, 3] > lcp [1, 2] [2] := by decide

end Everett.prefix
