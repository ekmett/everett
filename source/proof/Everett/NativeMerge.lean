/-
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-/
import Std

namespace Everett.native_merge

/-! An abstract two-way native merge. Equal keys call `compose key older newer`;
the callback cannot change the key. Each input run has strictly increasing
natural-number keys and arbitrary values. This executable list semantics is not
a C++ refinement, encoded-layout, callback-exception or persistence proof. -/

abbrev entry (Value : Type u) := Nat × Value

def above (key : Nat) : List (entry Value) → Prop
  | [] => True
  | value :: rest => key < value.1 ∧ above key rest

/-- Strict key order also excludes duplicate keys within each native run. -/
def ordered : List (entry Value) → Prop
  | [] => True
  | value :: rest => above value.1 rest ∧ ordered rest

def lookup (key : Nat) : List (entry Value) → Option Value
  | [] => none
  | value :: rest => if key = value.1 then some value.2 else lookup key rest

theorem above_iff (key : Nat) (xs : List (entry Value)) :
    above key xs ↔ ∀ value ∈ xs, key < value.1 := by
  induction xs with
  | nil => simp [above]
  | cons a xs ih => simp [above, ih]

/-- The recursive invariant is exactly Lean's strict pairwise key order. -/
theorem ordered_iff_pairwise (xs : List (entry Value)) :
    ordered xs ↔ xs.Pairwise (fun a b => a.1 < b.1) := by
  induction xs with
  | nil => simp [ordered]
  | cons a xs ih => simp [ordered, List.pairwise_cons, above_iff, ih]

theorem ordered_unique (xs : List (entry Value)) (h : ordered xs) :
    (xs.map Prod.fst).Nodup := by
  change (xs.map Prod.fst).Pairwise (fun a b => a ≠ b)
  apply List.pairwise_map.mpr
  exact ((ordered_iff_pairwise xs).mp h).imp (by intro a b less; exact Nat.ne_of_lt less)

/-- Missing bindings act as identities without an identity value in `Value`. -/
def combine_options (compose : Value → Value → Value) : Option Value → Option Value → Option Value
  | none, newer => newer
  | older, none => older
  | some older, some newer => some (compose older newer)

/-- Choose the smaller key, or compose the equal heads in chronological order. -/
def merge (compose : Nat → Value → Value → Value) : List (entry Value) → List (entry Value) → List (entry Value)
  | [], newer => newer
  | older, [] => older
  | older :: os, newer :: ns =>
    if older.1 < newer.1 then older :: merge compose os (newer :: ns)
    else if newer.1 < older.1 then newer :: merge compose (older :: os) ns
    else (older.1, compose older.1 older.2 newer.2) :: merge compose os ns
termination_by older newer => older.length + newer.length

theorem above_weaken (low high : Nat) (xs : List (entry Value))
    (bound : low ≤ high) (h : above high xs) : above low xs := by
  induction xs with
  | nil => trivial
  | cons a xs ih => exact ⟨by have := h.1; omega, ih h.2⟩

theorem lookup_above (key : Nat) (xs : List (entry Value))
    (h : above key xs) : lookup key xs = none := by
  induction xs with
  | nil => rfl
  | cons a xs ih =>
    have different : key ≠ a.1 := by have := h.1; omega
    simpa [lookup, different] using ih h.2

theorem above_merge (compose : Nat → Value → Value → Value) (key : Nat)
    (older newer : List (entry Value)) :
    above key older → above key newer → above key (merge compose older newer) := by
  induction older generalizing newer with
  | nil => intro _ hn; simpa [merge] using hn
  | cons a os iho =>
    induction newer with
    | nil => intro ho _; simpa [merge] using ho
    | cons b ns ihn =>
      intro ho hn
      by_cases ab : a.1 < b.1
      · simpa [merge, ab, above] using And.intro ho.1 (iho (b :: ns) ho.2 hn)
      · by_cases ba : b.1 < a.1
        · simpa [merge, ab, ba, above] using And.intro hn.1 (ihn ho hn.2)
        · simpa [merge, ab, ba, above] using And.intro ho.1 (iho ns ho.2 hn.2)

/-- Strict input order and uniqueness are preserved for every value callback. -/
theorem merge_ordered (compose : Nat → Value → Value → Value)
    (older newer : List (entry Value)) :
    ordered older → ordered newer → ordered (merge compose older newer) := by
  induction older generalizing newer with
  | nil => intro _ hn; simpa [merge] using hn
  | cons a os iho =>
    induction newer with
    | nil => intro ho _; simpa [merge] using ho
    | cons b ns ihn =>
      intro ho hn
      by_cases ab : a.1 < b.1
      · have after : above a.1 (b :: ns) :=
          ⟨ab, above_weaken a.1 b.1 ns (Nat.le_of_lt ab) hn.1⟩
        have head := above_merge compose a.1 os (b :: ns) ho.1 after
        have tail := iho (b :: ns) ho.2 hn
        simpa [merge, ab, ordered] using And.intro head tail
      · by_cases ba : b.1 < a.1
        · have after : above b.1 (a :: os) :=
            ⟨ba, above_weaken b.1 a.1 os (Nat.le_of_lt ba) ho.1⟩
          have head := above_merge compose b.1 (a :: os) ns after hn.1
          have tail := ihn ho hn.2
          simpa [merge, ab, ba, ordered] using And.intro head tail
        · have equal : a.1 = b.1 := by omega
          have after : above a.1 ns := by simpa [equal] using hn.1
          have head := above_merge compose a.1 os ns ho.1 after
          have tail := iho ns ho.2 hn.2
          simpa [merge, ab, ba, ordered] using And.intro head tail

theorem merge_unique (compose : Nat → Value → Value → Value)
    (older newer : List (entry Value)) (ho : ordered older) (hn : ordered newer) :
    ((merge compose older newer).map Prod.fst).Nodup :=
  ordered_unique _ (merge_ordered compose older newer ho hn)

/-- The recursive algorithm computes optional pointwise composition. -/
theorem lookup_merge (compose : Nat → Value → Value → Value) (key : Nat)
    (older newer : List (entry Value)) :
    ordered older → ordered newer →
    lookup key (merge compose older newer) =
      combine_options (compose key) (lookup key older) (lookup key newer) := by
  induction older generalizing newer with
  | nil => intro _ _; simp [merge, lookup, combine_options]
  | cons a os iho =>
    induction newer with
    | nil =>
      intro _ _
      have empty : merge compose (a :: os) [] = a :: os := by simp [merge]
      rw [empty]
      change lookup key (a :: os) = combine_options (compose key) (lookup key (a :: os)) none
      cases lookup key (a :: os) <;> rfl
    | cons b ns ihn =>
      intro ho hn
      by_cases ab : a.1 < b.1
      · by_cases hit : key = a.1
        · have after : above key (b :: ns) := by
            subst key
            exact ⟨ab, above_weaken a.1 b.1 ns (Nat.le_of_lt ab) hn.1⟩
          have absent := lookup_above key (b :: ns) after
          rw [absent]
          simp [merge, ab, lookup, hit, combine_options]
        · simpa [merge, ab, lookup, hit] using iho (b :: ns) ho.2 hn
      · by_cases ba : b.1 < a.1
        · by_cases hit : key = b.1
          · have after : above key (a :: os) := by
              subst key
              exact ⟨ba, above_weaken b.1 a.1 os (Nat.le_of_lt ba) ho.1⟩
            have absent := lookup_above key (a :: os) after
            rw [absent]
            simp [merge, ab, ba, lookup, hit, combine_options]
          · simpa [merge, ab, ba, lookup, hit] using ihn ho hn.2
        · have equal : a.1 = b.1 := by omega
          by_cases hit : key = a.1
          · simp [merge, ab, ba, lookup, hit, equal, combine_options]
          · have miss : key ≠ b.1 := by omega
            simpa [merge, ab, ba, lookup, hit, miss] using iho ns ho.2 hn.2

/-- Strictly ordered runs are a canonical representation of their lookups. -/
theorem ordered_ext (a b : List (entry Value)) :
    ordered a → ordered b → (∀ key, lookup key a = lookup key b) → a = b := by
  induction a generalizing b with
  | nil =>
    cases b with
    | nil => intros; rfl
    | cons b bs =>
      intro _ _ equal
      have impossible := equal b.1
      simp [lookup] at impossible
  | cons a as ih =>
    cases b with
    | nil =>
      intro _ _ equal
      have impossible := equal a.1
      simp [lookup] at impossible
    | cons b bs =>
      intro ha hb equal
      have keys : a.1 = b.1 := by
        by_cases ab : a.1 < b.1
        · have after : above a.1 (b :: bs) :=
            ⟨ab, above_weaken a.1 b.1 bs (Nat.le_of_lt ab) hb.1⟩
          have absent := lookup_above a.1 (b :: bs) after
          have impossible := equal a.1
          rw [absent] at impossible
          simp [lookup] at impossible
        · by_cases ba : b.1 < a.1
          · have after : above b.1 (a :: as) :=
              ⟨ba, above_weaken b.1 a.1 as (Nat.le_of_lt ba) ha.1⟩
            have absent := lookup_above b.1 (a :: as) after
            have impossible := equal b.1
            rw [absent] at impossible
            simp [lookup] at impossible
          · omega
      have values : a.2 = b.2 := by simpa [lookup, keys] using equal a.1
      have entries : a = b := Prod.ext keys values
      subst b
      have tails : ∀ key, lookup key as = lookup key bs := by
        intro key
        by_cases hit : key = a.1
        · subst key
          rw [lookup_above a.1 as ha.1, lookup_above a.1 bs hb.1]
        · simpa [lookup, hit] using equal key
      exact congrArg (List.cons a) (ih bs ha.2 hb.2 tails)

/-- An associative callback remains associative after adjoining missing keys. -/
theorem combine_options_assoc (compose : Value → Value → Value)
    (associative : ∀ a b c, compose (compose a b) c = compose a (compose b c))
    (a b c : Option Value) :
    combine_options compose (combine_options compose a b) c =
      combine_options compose a (combine_options compose b c) := by
  cases a <;> cases b <;> cases c <;> simp [combine_options, associative]

/-- Reassociation preserves every lookup with the same chronological leaves.
The callback need not commute. This theorem states semantic lookup equality. -/
theorem lookup_merge_assoc (compose : Nat → Value → Value → Value)
    (associative : ∀ key a b c, compose key (compose key a b) c = compose key a (compose key b c))
    (a b c : List (entry Value)) (ha : ordered a) (hb : ordered b) (hc : ordered c)
    (key : Nat) :
    lookup key (merge compose (merge compose a b) c) =
      lookup key (merge compose a (merge compose b c)) := by
  rw [lookup_merge compose key _ _ (merge_ordered compose a b ha hb) hc,
      lookup_merge compose key a b ha hb,
      lookup_merge compose key _ _ ha (merge_ordered compose b c hb hc),
      lookup_merge compose key b c hb hc]
  exact combine_options_assoc (compose key) (associative key) _ _ _

/-- Disjoint supports never invoke the callback, so their order is immaterial. -/
theorem lookup_merge_disjoint (compose : Nat → Value → Value → Value)
    (a b : List (entry Value)) (ha : ordered a) (hb : ordered b)
    (disjoint : ∀ key, lookup key a = none ∨ lookup key b = none) (key : Nat) :
    lookup key (merge compose a b) = lookup key (merge compose b a) := by
  rw [lookup_merge compose key a b ha hb, lookup_merge compose key b a hb ha]
  rcases disjoint key with absent | absent
  · rw [absent]; cases lookup key b <;> rfl
  · rw [absent]; cases lookup key a <;> rfl

/-- Canonical ordered output strengthens lookup reassociation to list equality. -/
theorem merge_assoc (compose : Nat → Value → Value → Value)
    (associative : ∀ key a b c, compose key (compose key a b) c = compose key a (compose key b c))
    (a b c : List (entry Value)) (ha : ordered a) (hb : ordered b) (hc : ordered c) :
    merge compose (merge compose a b) c = merge compose a (merge compose b c) := by
  exact ordered_ext _ _
    (merge_ordered compose _ _ (merge_ordered compose a b ha hb) hc)
    (merge_ordered compose _ _ ha (merge_ordered compose b c hb hc))
    (lookup_merge_assoc compose associative a b c ha hb hc)

theorem merge_disjoint (compose : Nat → Value → Value → Value)
    (a b : List (entry Value)) (ha : ordered a) (hb : ordered b)
    (disjoint : ∀ key, lookup key a = none ∨ lookup key b = none) :
    merge compose a b = merge compose b a := by
  exact ordered_ext _ _ (merge_ordered compose a b ha hb) (merge_ordered compose b a hb ha)
    (lookup_merge_disjoint compose a b ha hb disjoint)

-- Kernel-checked examples; list concatenation retains its argument order.
theorem concat_example : merge (fun _ (a b : List Nat) => a ++ b)
    [(1, [10]), (3, [30])] [(2, [20]), (3, [31])] =
    [(1, [10]), (2, [20]), (3, [30, 31])] := by simp [merge]
theorem reverse_concat_example : merge (fun _ (a b : List Nat) => a ++ b)
    [(3, [31])] [(3, [30])] = [(3, [31, 30])] := by simp [merge]
theorem ordered_example : ordered (merge (fun _ (a b : List Nat) => a ++ b)
    [(1, [10]), (3, [30])] [(2, [20]), (3, [31])]) := by simp [merge, ordered, above]

/-- Reversing two same-key chronological inputs can change the result. -/
theorem concat_not_commutative :
    merge (fun _ (a b : List Nat) => a ++ b) [(3, [30])] [(3, [31])] ≠
    merge (fun _ (a b : List Nat) => a ++ b) [(3, [31])] [(3, [30])] := by
  simp [merge]

/-- Concatenation illustrates associativity without commutativity. -/
theorem concat_assoc (a b c : List (entry (List Nat)))
    (ha : ordered a) (hb : ordered b) (hc : ordered c) :
    merge (fun _ x y => x ++ y) (merge (fun _ x y => x ++ y) a b) c =
      merge (fun _ x y => x ++ y) a (merge (fun _ x y => x ++ y) b c) := by
  exact merge_assoc _ (by intro _ x y z; exact List.append_assoc x y z) a b c ha hb hc

end Everett.native_merge
