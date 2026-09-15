/-
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-/
import Diet.Fractional

namespace Diet.fractional_examples

open fractional

def native_input : List occurrence := [⟨1, false, 0⟩, ⟨5, false, 1⟩]
def borrowed_input : List occurrence :=
  [⟨2, true, 0⟩, ⟨5, true, 1⟩, ⟨5, true, 2⟩, ⟨5, true, 3⟩,
   ⟨5, true, 4⟩, ⟨5, true, 5⟩, ⟨5, true, 6⟩, ⟨5, true, 7⟩,
   ⟨5, true, 8⟩, ⟨10, true, 9⟩]
def duplicates : List occurrence :=
  [⟨1, false, 0⟩, ⟨2, true, 0⟩, ⟨5, false, 1⟩, ⟨5, true, 1⟩,
   ⟨5, true, 2⟩, ⟨5, true, 3⟩, ⟨5, true, 4⟩, ⟨5, true, 5⟩,
   ⟨5, true, 6⟩, ⟨5, true, 7⟩, ⟨5, true, 8⟩, ⟨10, true, 9⟩]

theorem duplicates_is_merge : augment native_input borrowed_input = duplicates := by
  simp [augment, native_input, borrowed_input, duplicates, List.cons_merge_cons, before, priority]

theorem duplicates_sorted : duplicates.Pairwise (fun a b => before a b = true) := by decide

theorem duplicates_unique : unique_native_keys duplicates := by
  have finite : ∀ (i j : Fin duplicates.length),
      duplicates[i.val].borrowed = false → duplicates[j.val].borrowed = false →
      duplicates[i.val].key = duplicates[j.val].key → i.val = j.val := by decide
  intro i j hi hj native_i native_j same
  exact finite ⟨i, hi⟩ ⟨j, hj⟩ native_i native_j same

example : duplicates.length = 12 := by decide
example : duplicates.filter native = native_input := by decide
example : duplicates.filter occurrence.borrowed = borrowed_input := by decide
example : samples duplicates 3 =
    [(0, ⟨1, false, 0⟩), (3, ⟨5, true, 1⟩),
     (6, ⟨5, true, 4⟩), (9, ⟨5, true, 7⟩)] := by decide

/-- Equal borrowed keys span several cuts. Their identities and multiplicity survive. -/
example : predecessor duplicates 5 = some 10 := by decide
example : route duplicates 3 5 = 9 := by decide
example : window duplicates 9 12 =
    [⟨5, true, 7⟩, ⟨5, true, 8⟩, ⟨10, true, 9⟩] := by decide
example : projection native duplicates 9 12 = [] := by decide
example : projection occurrence.borrowed duplicates 9 12 = window duplicates 9 12 := by decide
example : rank native duplicates 9 = 2 := by decide
example : rank native duplicates 10 = 2 := by decide
example : false_borrow_flag duplicates 10 = true := by decide
example : false_borrow_flag duplicates 11 = false := by decide
example : (duplicates.filter native)[rank native duplicates 10 - 1]? = some ⟨5, false, 1⟩ := by decide

theorem duplicate_recovery :
    0 < rank native duplicates 10 ∧
      (duplicates.filter native)[rank native duplicates 10 - 1]? = some duplicates[2] :=
  false_borrow_recovery duplicates duplicates_sorted duplicates_unique 2 10
    (by decide) (by decide) (by decide) (by decide) (by decide)

example : routed_predecessor duplicates 3 5 = predecessor duplicates 5 :=
  routed_predecessor_correct duplicates 3 5 (by decide) (sorted_keys _ duplicates_sorted)
example : (List.range 13).all (fun q => routed_predecessor duplicates 3 q == predecessor duplicates q) := by decide
example : (List.range 13).all (fun q => routed_predecessor duplicates 15 q == predecessor duplicates q) := by decide

/-- Empty target and before-first queries do not fabricate occurrence zero. -/
example : samples [] 3 = [] := by decide
example : routed_predecessor [] 3 42 = none := by decide
example : projection native [] 0 0 = [] := by decide
example : predecessor duplicates 0 = none := by decide
example : route duplicates 3 0 = 0 := by decide
example : routed_predecessor duplicates 3 0 = none := by decide

def borrowed_only : List occurrence := [⟨4, true, 0⟩, ⟨4, true, 1⟩, ⟨4, true, 2⟩, ⟨7, true, 3⟩]
example : augment [] borrowed_only = borrowed_only := by simp [augment]
example : route borrowed_only 3 7 = 3 := by decide
example : projection native borrowed_only 3 4 = [] := by decide
example : rank native borrowed_only 3 = 0 := by decide
example : false_borrow_flag borrowed_only 2 = false := by decide
example : routed_predecessor borrowed_only 3 7 = some 3 := by decide

/-- K=15 has a two-entry tail; K is a local mathematical spacing parameter. -/
def tail_target : List occurrence := (List.range 17).map (fun i => ⟨i + 10, false, i⟩)
example : sample_positions tail_target 15 = [0, 15] := by decide
example : route tail_target 15 100 = 15 := by decide
example : (window tail_target 15 17).length = 2 := by decide
example : routed_predecessor tail_target 15 100 = some 16 := by decide
example : (projection native tail_target 15 17).length = 2 := by decide
example : projection occurrence.borrowed tail_target 15 17 = [] := by decide

/-- A native equality run is legal for merge/routing but outside unique-native recovery. -/
def repeated_native : List occurrence := [⟨5, false, 0⟩, ⟨5, false, 1⟩, ⟨5, true, 0⟩]
example : repeated_native.Pairwise (fun a b => before a b = true) := by decide
example : predecessor repeated_native 5 = some 2 := by decide
example : ¬ unique_native_keys repeated_native := by
  intro unique
  have impossible := unique 0 1 (by decide) (by decide) (by decide) (by decide) (by decide)
  contradiction

def exact_target : blob (List occurrence) := ⟨10, 20, none, duplicates⟩
def source_pair : blob (List occurrence) := ⟨11, 21, some 0, []⟩
def pair_catalog : catalog (List occurrence) :=
  catalog.install (catalog.install (fun _ => none) 0 exact_target) 1 source_pair
def replacement_target : blob (List occurrence) := ⟨10, 22, none, tail_target⟩
def extended_catalog : catalog (List occurrence) := catalog.install pair_catalog 2 replacement_target

example : target_samples pair_catalog 1 3 = some (samples duplicates 3) := by decide
example : target_samples extended_catalog 1 3 = some (samples duplicates 3) := by decide
example : build_index pair_catalog 0 3 = some ⟨0, samples duplicates 3⟩ := by decide
example : build_index extended_catalog 2 3 = some ⟨2, samples tail_target 3⟩ := by decide
example : build_index pair_catalog 9 3 = none := by decide

def stored_index : sampled_index := ⟨0, samples duplicates 3⟩
theorem stored_index_certified : index_matches pair_catalog stored_index 3 :=
  (build_index_matches pair_catalog 0 3 stored_index (by decide)).2

example : index_route stored_index 5 = 9 := by decide
example : indexed_predecessor stored_index duplicates 3 5 = some 10 := by decide
example : indexed_predecessor stored_index duplicates 3 0 = none := by decide
example : indexed_predecessor stored_index exact_target.payload 3 5 =
    predecessor exact_target.payload 5 :=
  indexed_predecessor_correct pair_catalog stored_index 3 5 (by decide) exact_target
    (by decide) stored_index_certified (sorted_keys _ duplicates_sorted)

end Diet.fractional_examples
