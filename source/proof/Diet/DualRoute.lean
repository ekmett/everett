/-
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-/
import Diet.Fractional
import Diet.Prefix

namespace Diet.dual_route

/-! Three-way projections and independent routes are list-level semantics.
Stored content certification, physical identities, false-borrow probes and the
scheduler remain separate refinement obligations. Secondary catalogs are leaves. -/

inductive origin where
  | native | main | secondary
  deriving DecidableEq, Repr

def has_origin (tag : α → origin) (which : origin) (x : α) : Bool := decide (tag x = which)
def project (tag : α → origin) (which : origin) (xs : List α) (lo hi : Nat) : List α :=
  fractional.projection (has_origin tag which) xs lo hi

theorem three_filter_lengths (tag : α → origin) (xs : List α) :
    (xs.filter (has_origin tag .native)).length +
      (xs.filter (has_origin tag .main)).length +
      (xs.filter (has_origin tag .secondary)).length = xs.length := by
  induction xs with
  | nil => rfl
  | cons x xs ih => cases h : tag x <;> simp [has_origin, h] at * <;> omega

/-- Endpoint ranks recover each complete origin-filtered interval. -/
theorem project_exact (tag : α → origin) (which : origin) (xs : List α)
    (lo hi : Nat) (ordered : lo ≤ hi) :
    project tag which xs lo hi = (fractional.window xs lo hi).filter (has_origin tag which) :=
  fractional.project_window _ xs lo hi ordered

/-- All three source intervals share one virtual-window entry budget. -/
theorem projected_lengths_sum (tag : α → origin) (xs : List α) (lo hi : Nat)
    (ordered : lo ≤ hi) (inside : hi ≤ xs.length) :
    (project tag .native xs lo hi).length + (project tag .main xs lo hi).length +
      (project tag .secondary xs lo hi).length = hi - lo := by
  simp only [project_exact tag _ xs lo hi ordered]
  rw [three_filter_lengths]
  simp only [fractional.window, List.length_take, List.length_drop]
  omega

theorem projected_budget (tag : α → origin) (xs : List α) (lo hi K : Nat)
    (ordered : lo ≤ hi) (inside : hi ≤ xs.length) (bounded : hi - lo ≤ K) :
    (project tag .native xs lo hi).length + (project tag .main xs lo hi).length +
      (project tag .secondary xs lo hi).length ≤ K := by
  rw [projected_lengths_sum tag xs lo hi ordered inside]
  exact bounded

def routed_pair (main secondary : List fractional.occurrence) (K query : Nat) :
    Option Nat × Option Nat :=
  (fractional.routed_predecessor main K query, fractional.routed_predecessor secondary K query)

/-- Independent children may have different lengths, equality runs and answers. -/
theorem routed_pair_correct (main secondary : List fractional.occurrence) (K query : Nat)
    (positive : 0 < K) (main_sorted : fractional.key_sorted main)
    (secondary_sorted : fractional.key_sorted secondary) :
    routed_pair main secondary K query =
      (fractional.predecessor main query, fractional.predecessor secondary query) := by
  simp only [routed_pair, fractional.routed_predecessor_correct main K query positive main_sorted,
    fractional.routed_predecessor_correct secondary K query positive secondary_sorted]

/-- Absence differs from an existing empty-string predecessor. -/
def repair (present : Option (List Nat)) (cut incoming query_length : Nat) : Option (Nat × Bool) :=
  present.map (fun _ => (min cut incoming, decide (min cut incoming = query_length)))
def exact_comparison (present : Option (List Nat)) (query : List Nat) : Option (Nat × Bool) :=
  present.map (fun key => (prefix.lcp key query, decide (key = query)))

theorem repair_correct (present : Option (List Nat)) (boundary query : List Nat)
    (cut incoming : Nat) (boundary_lower : boundary ≤ query)
    (incoming_exact : incoming = prefix.lcp boundary query)
    (predecessor_lower : ∀ c, present = some c → c ≤ boundary)
    (cut_exact : ∀ c, present = some c → cut = prefix.lcp c boundary) :
    repair present cut incoming query.length = exact_comparison present query := by
  cases present with
  | none => rfl
  | some c =>
    have recovered := prefix.frontier_recovery_without_length c boundary query
      (predecessor_lower c rfl) boundary_lower cut incoming (cut_exact c rfl) incoming_exact
    simp only [repair, exact_comparison, Option.map_some']
    simp only [recovered.2.1, recovered.2.2]

/-- Each borrowed stream supplies its own presence and exact cut scalar. -/
theorem repair_both (main secondary : Option (List Nat)) (boundary query : List Nat)
    (main_cut secondary_cut incoming : Nat) (boundary_lower : boundary ≤ query)
    (incoming_exact : incoming = prefix.lcp boundary query)
    (main_lower : ∀ c, main = some c → c ≤ boundary)
    (secondary_lower : ∀ c, secondary = some c → c ≤ boundary)
    (main_exact : ∀ c, main = some c → main_cut = prefix.lcp c boundary)
    (secondary_exact : ∀ c, secondary = some c → secondary_cut = prefix.lcp c boundary) :
    (repair main main_cut incoming query.length, repair secondary secondary_cut incoming query.length) =
      (exact_comparison main query, exact_comparison secondary query) := by
  rw [repair_correct main boundary query main_cut incoming boundary_lower incoming_exact main_lower main_exact,
    repair_correct secondary boundary query secondary_cut incoming boundary_lower incoming_exact
      secondary_lower secondary_exact]

/-- Only main links recurse. An optional secondary is a terminal native list;
a synthetic empty-native level can represent the root above level zero. -/
inductive chain where
  | empty
  | level (entries : List fractional.occurrence)
      (secondary : Option (List fractional.occurrence)) (main : chain)
  deriving Repr

def height : chain → Nat
  | .empty => 0
  | .level _ _ main => height main + 1

def search (K query : Nat) : chain → List (Option Nat)
  | .empty => []
  | .level entries secondary main =>
    fractional.routed_predecessor entries K query ::
      (secondary.toList.map (fun xs => fractional.routed_predecessor xs K query) ++ search K query main)
def full_search (query : Nat) : chain → List (Option Nat)
  | .empty => []
  | .level entries secondary main =>
    fractional.predecessor entries query ::
      (secondary.toList.map (fun xs => fractional.predecessor xs query) ++ full_search query main)
def sorted : chain → Prop
  | .empty => True
  | .level entries secondary main =>
    fractional.key_sorted entries ∧
      (∀ xs, secondary = some xs → fractional.key_sorted xs) ∧ sorted main

/-- Exact samples are computed by this model; encoded content needs a separate
certificate. Every visited catalog retains its own answer, including none.
This traversal recomputes each route; it does not yet carry a parent window
into the next catalog. -/
theorem search_correct (tree : chain) (K query : Nat) (positive : 0 < K)
    (valid : sorted tree) : search K query tree = full_search query tree := by
  induction tree with
  | empty => rfl
  | level entries secondary main ih =>
    obtain ⟨main_sorted, leaf_sorted, rest_sorted⟩ := valid
    simp only [search, full_search,
      fractional.routed_predecessor_correct entries K query positive main_sorted, ih rest_sorted]
    cases secondary with
    | none => rfl
    | some leaf =>
      simp only [Option.toList_some, List.map_cons, List.map_nil,
        fractional.routed_predecessor_correct leaf K query positive (leaf_sorted leaf rfl)]

/-- Catalog visits, not key-byte, instruction or I/O work. -/
theorem visits_bound (tree : chain) (K query : Nat) :
    (search K query tree).length ≤ 2 * height tree := by
  induction tree with
  | empty => simp [search, height]
  | level entries secondary main ih =>
    cases secondary <;> simp [search, height] at * <;> omega

-- Kernel-reduced mixed origins and endpoint examples.
example : project id .main [.native, .main, .secondary, .main] 1 4 = [.main, .main] := by decide
example : routed_pair [⟨1, false, 0⟩, ⟨4, false, 1⟩, ⟨7, false, 2⟩]
    [⟨2, false, 0⟩, ⟨4, false, 1⟩, ⟨4, false, 2⟩, ⟨9, false, 3⟩] 3 4 =
    (some 1, some 2) := by decide
example : routed_pair [] [⟨2, false, 0⟩] 3 1 = (none, none) := by decide
example : repair none 17 9 0 = none := rfl
example : repair (some []) 0 0 0 = some (0, true) := rfl
example : repair (some [1]) 1 2 3 = some (1, false) := rfl
example : (search 3 7 (.level [] (some []) .empty)).length = 2 := rfl

end Diet.dual_route
