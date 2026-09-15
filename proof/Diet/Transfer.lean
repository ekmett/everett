/-
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-/
import Std

namespace Diet.transfer

/-- The comparison direction travels with its selected content mismatch. -/
inductive direction
  | left_less
  | right_less
  deriving DecidableEq, Repr

/-- Infinity is the absence of a finite content mismatch in this algebra.
It is not a string terminator, key length, or assertion that two keys are equal.
Endpoint ordering must be accounted for separately by a future string theorem. -/
inductive mismatch
  | finite (position : Nat) (sign : direction)
  | infinity
  deriving DecidableEq, Repr

/-- A finite content mismatch strictly before the retained-prefix threshold. -/
def before : mismatch → Nat → Prop
  | .finite position _, r => position < r
  | .infinity, _ => False

instance (d : mismatch) (r : Nat) : Decidable (before d r) := by
  cases d <;> unfold before <;> infer_instance

/-- Raw summaries permit stating exactly where validity is needed. -/
structure summary where
  r : Nat
  e : mismatch
  deriving DecidableEq, Repr

/-- An emitted mismatch cannot precede the retained-prefix threshold. -/
def summary.valid (a : summary) : Prop :=
  match a.e with
  | .finite position _ => a.r ≤ position
  | .infinity => True

instance (a : summary) : Decidable a.valid := by
  unfold summary.valid
  split <;> infer_instance

/-- T_(r,e)(d): retain an earlier mismatch; otherwise select the emitted one. -/
def run (a : summary) (d : mismatch) : mismatch :=
  if before d a.r then d else a.e

/-- Ordered composition: first a, then b. Tags remain attached to their e. -/
def compose (a b : summary) : summary :=
  ⟨min a.r b.r, if before a.e b.r then a.e else b.e⟩

theorem summary_ext {a b : summary} (hr : a.r = b.r) (he : a.e = b.e) : a = b := by
  cases a
  cases b
  cases hr
  cases he
  rfl

theorem before_min (d : mismatch) (a b : Nat) :
    before d (min a b) ↔ before d a ∧ before d b := by
  cases d with
  | finite position sign => unfold before; omega
  | infinity => simp [before]

theorem emitted_not_before {a : summary} (ha : a.valid) {r : Nat} (hr : r ≤ a.r) :
    ¬before a.e r := by
  cases he : a.e with
  | finite position sign => simp [summary.valid, he] at ha; simp [before]; omega
  | infinity => simp [before]

/-- Both valid inputs produce a valid stored summary. -/
theorem valid_compose {a b : summary} (ha : a.valid) (hb : b.valid) :
    (compose a b).valid := by
  by_cases h : before a.e b.r
  · simp only [compose, if_pos h, summary.valid]
    cases he : a.e with
    | finite position sign => simp [summary.valid, he] at ha; omega
    | infinity => trivial
  · simp only [compose, if_neg h, summary.valid]
    cases he : b.e with
    | finite position sign => simp [summary.valid, he] at hb; omega
    | infinity => trivial

/-- The formula is actual function composition, including the direction tag.
Validity of the earlier summary is essential; the later summary need not be
valid for this equality, though it must be valid for closure of the family. -/
theorem run_compose (a b : summary) (ha : a.valid) (d : mismatch) :
    run (compose a b) d = run b (run a d) := by
  cases d with
  | infinity => simp [run, compose, before]
  | finite position sign =>
    by_cases hpa : position < a.r
    · have hfirst : run a (.finite position sign) = .finite position sign := if_pos hpa
      rw [hfirst]
      by_cases hpb : position < b.r
      · have hm : position < min a.r b.r := by omega
        rw [show run b (.finite position sign) = .finite position sign from if_pos hpb]
        exact if_pos hm
      · have hm : ¬position < min a.r b.r := by omega
        have hr : b.r ≤ a.r := by omega
        have he := emitted_not_before ha hr
        rw [show run b (.finite position sign) = b.e from if_neg hpb]
        rw [show run (compose a b) (.finite position sign) = (compose a b).e from if_neg hm]
        exact if_neg he
    · have hm : ¬position < min a.r b.r := by omega
      rw [show run a (.finite position sign) = a.e from if_neg hpa]
      rw [show run (compose a b) (.finite position sign) = (compose a b).e from if_neg hm]
      rfl

/-- Associativity holds as equality of stored r/e representations, not merely
as extensional equality of their functions. The middle validity assumption
prevents its emission from moving back across a later threshold. -/
theorem compose_assoc (a b c : summary) (hb : b.valid) :
    compose (compose a b) c = compose a (compose b c) := by
  apply summary_ext
  · exact Nat.min_assoc a.r b.r c.r
  · by_cases hab : before a.e b.r
    · by_cases hac : before a.e c.r
      · simp [compose, before_min, hab, hac]
      · have hr : c.r ≤ b.r := by
          cases he : a.e with
          | finite position sign => simp [he, before] at hab hac; omega
          | infinity => simp [he, before] at hab
        have hbc := emitted_not_before hb hr
        simp [compose, before_min, hab, hac, hbc]
    · simp [compose, before_min, hab]

/-- Validity is carried by the type for repeated composition. -/
abbrev valid_summary := {a : summary // a.valid}

def combine (a b : valid_summary) : valid_summary :=
  ⟨compose a.val b.val, valid_compose a.property b.property⟩

theorem combine_assoc (a b c : valid_summary) :
    combine (combine a b) c = combine a (combine b c) := by
  apply Subtype.ext
  exact compose_assoc a.val b.val c.val b.property

theorem run_combine (a b : valid_summary) (d : mismatch) :
    run (combine a b).val d = run b.val (run a.val d) :=
  run_compose a.val b.val a.property d

/-- Earlier mismatches keep both their positions and their directions. -/
theorem run_before (a : summary) (d : mismatch) (h : before d a.r) : run a d = d := by
  simp [run, h]

/-- Otherwise both position and direction come from the selected emission. -/
theorem run_after (a : summary) (d : mismatch) (h : ¬before d a.r) : run a d = a.e := by
  simp [run, h]

namespace examples

-- Different tag choices exercise selection rather than assuming commutativity.
def a : valid_summary := ⟨⟨3, .finite 5 .left_less⟩, by decide⟩
def b : valid_summary := ⟨⟨7, .finite 9 .right_less⟩, by decide⟩
def c : valid_summary := ⟨⟨4, .infinity⟩, by decide⟩
def d : valid_summary := ⟨⟨3, .finite 5 .right_less⟩, by decide⟩

example : (combine a b).val = ⟨3, .finite 5 .left_less⟩ := by decide
example : (combine b a).val = ⟨3, .finite 5 .left_less⟩ := by decide
example : combine a d ≠ combine d a := by decide
example : run a.val (.finite 2 .right_less) = .finite 2 .right_less := by decide
example : run a.val (.finite 3 .right_less) = .finite 5 .left_less := by decide
example : run c.val (.finite 8 .left_less) = .infinity := by decide
example : run (combine a c).val .infinity = .infinity := by decide
example : combine (combine a b) c = combine a (combine b c) := combine_assoc a b c

/-- Without e >= r, the proposed summary need not represent composition. -/
example :
    run (compose ⟨3, .finite 1 .left_less⟩ ⟨2, .finite 5 .right_less⟩) (.finite 2 .left_less) ≠
    run ⟨2, .finite 5 .right_less⟩ (run ⟨3, .finite 1 .left_less⟩ (.finite 2 .left_less)) := by
  decide

/-- The same missing invariant can also break representation associativity. -/
example :
    compose (compose ⟨1, .finite 2 .left_less⟩ ⟨3, .finite 0 .right_less⟩) ⟨2, .finite 4 .right_less⟩ ≠
    compose ⟨1, .finite 2 .left_less⟩ (compose ⟨3, .finite 0 .right_less⟩ ⟨2, .finite 4 .right_less⟩) := by
  decide

end examples
end Diet.transfer
