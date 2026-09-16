/-
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-/
import Everett.Navigation

namespace Everett.carried_route
open fractional

/-! One certified parent-to-child edge. A concrete stable builder retains the
child's exact samples. A qualifying parent cut transfers through its origin rank
to the child route; the transfer does not search or resample the child. This is
not yet a three-origin chain or a byte-parser refinement. -/

def qualifies (query : Nat) (entry : occurrence) : Bool := decide (entry.key ≤ query)

/-- A half-open parent cut, as established by an exact predecessor search. -/
def cut_certificate (parent : List occurrence) (query cut : Nat) : Prop :=
  cut ≤ parent.length ∧ ∀ i, i < parent.length → (i < cut ↔ key_at parent i ≤ query)

def after (answer : Option Nat) : Nat := answer.map (· + 1) |>.getD 0

theorem predecessor_cut (parent : List occurrence) (query : Nat)
    (sorted : key_sorted parent) :
    cut_certificate parent query (after (predecessor parent query)) := by
  cases found : predecessor parent query with
  | none =>
    have absent := (predecessor_none parent query).mp found
    constructor
    · simp [after]
    · intro i inside; have := absent i inside; simp [after]; omega
  | some last =>
    obtain ⟨last_inside, last_key, maximal⟩ := predecessor_spec parent query last found
    constructor
    · simp [after]; omega
    · intro i inside
      simp only [after, Option.map_some', Option.getD_some]
      constructor
      · intro before; exact Nat.le_trans (sorted i last inside last_inside (by omega)) last_key
      · intro key; have := maximal i inside key; omega

/-- The cut retains precisely the qualifying prefix, including entire equality runs. -/
theorem certified_prefix (parent : List occurrence) (query cut : Nat)
    (certified : cut_certificate parent query cut) :
    parent.take cut = parent.filter (qualifies query) := by
  have prefix_kept : (parent.take cut).filter (qualifies query) = parent.take cut := by
    apply List.filter_eq_self.mpr
    intro entry member
    obtain ⟨i, inside, eq⟩ := List.mem_iff_getElem.mp member
    have global : i < parent.length := by simp only [List.length_take] at inside; omega
    have before : i < cut := by simp only [List.length_take] at inside; omega
    have key := (certified.2 i global).mp before
    simpa [qualifies, ← eq, key_at, global] using key
  have suffix : (parent.drop cut).filter (qualifies query) = [] := by
    apply List.filter_eq_nil_iff.mpr
    intro entry member
    obtain ⟨i, inside, eq⟩ := List.mem_iff_getElem.mp member
    have global : cut + i < parent.length := by simp only [List.length_drop] at inside; omega
    have key := certified.2 (cut + i) global
    simp only [qualifies, decide_eq_true_eq]
    have value : entry.key = key_at parent (cut + i) := by
      simp [← eq, key_at, global]
    rw [value]
    omega
  have split := congrArg (List.filter (qualifies query)) (List.take_append_drop cut parent)
  rw [List.filter_append, prefix_kept, suffix, List.append_nil] at split
  exact split

def borrowed_entry (sample : Nat × occurrence) : occurrence :=
  ⟨sample.2.key, true, sample.1⟩
def borrowed_samples (child : List occurrence) (K : Nat) : List occurrence :=
  (samples child K).map borrowed_entry
def native_entries (native : List occurrence) : List occurrence :=
  native.map (fun entry => { entry with borrowed := false })
def build_parent (native child : List occurrence) (K : Nat) : List occurrence :=
  augment (native_entries native) (borrowed_samples child K)

theorem native_entries_sorted (native : List occurrence) (sorted : key_sorted native) :
    (native_entries native).Pairwise (fun a b => before a b = true) := by
  apply List.pairwise_map.mpr
  apply List.pairwise_iff_getElem.mpr
  intro i j hi hj ij
  have le := sorted i j hi hj (Nat.le_of_lt ij)
  simp only [key_at, getElem!_pos native i hi, getElem!_pos native j hj] at le
  simp [before, priority]
  omega

theorem borrowed_samples_sorted (child : List occurrence) (K : Nat) (sorted : key_sorted child) :
    (borrowed_samples child K).Pairwise (fun a b => before a b = true) := by
  simp only [borrowed_samples, samples, List.map_map]
  apply List.pairwise_map.mpr
  have ordered := (List.pairwise_le_range (n := child.length)).filter (fun i => i % K == 0)
  apply ordered.imp_of_mem
  intro i j hi hj ij
  have left := (mem_sample_positions child K i).mp hi
  have right := (mem_sample_positions child K j).mp hj
  have le := sorted i j left.1 right.1 ij
  simp only [key_at] at le
  change before (borrowed_entry (i, child[i]!)) (borrowed_entry (j, child[j]!)) = true
  simp only [borrowed_entry, before, priority, Bool.true_eq, ↓reduceIte, decide_eq_true_eq]
  omega

/-- Sorted inputs establish sorted parent entries through the actual builder. -/
theorem build_parent_sorted (native child : List occurrence) (K : Nat)
    (native_sorted : key_sorted native) (child_sorted : key_sorted child) :
    key_sorted (build_parent native child K) :=
  sorted_keys _ (augment_sorted _ _ (native_entries_sorted native native_sorted)
    (borrowed_samples_sorted child K child_sorted))

/-- Builder correctness is proved by stable merge filtering, not assumed as a
certificate. Target ordinals and multiplicity are retained exactly. -/
theorem build_parent_borrowed (native child : List occurrence) (K : Nat) :
    (build_parent native child K).filter occurrence.borrowed = borrowed_samples child K := by
  apply augment_filter_right
  · intro entry member
    obtain ⟨source, _, rfl⟩ := List.mem_map.mp member
    rfl
  · intro entry member
    obtain ⟨sample, _, rfl⟩ := List.mem_map.mp member
    rfl

theorem borrowed_ordinals_sorted (child : List occurrence) (K : Nat) :
    ((borrowed_samples child K).map occurrence.ordinal).Pairwise (· ≤ ·) := by
  simpa [borrowed_samples, borrowed_entry, samples, List.map_map, Function.comp_def,
    sample_positions] using
    (List.pairwise_le_range (n := child.length)).filter (fun i => i % K == 0)

/-- Origin rank at the parent cut identifies exactly the passed child samples. -/
theorem passed_samples (native child : List occurrence) (K query cut : Nat)
    (certified : cut_certificate (build_parent native child K) query cut) :
    (borrowed_samples child K).take (rank occurrence.borrowed (build_parent native child K) cut) =
      (borrowed_samples child K).filter (qualifies query) := by
  rw [← build_parent_borrowed native child K, take_rank,
    certified_prefix _ query cut certified]
  simp only [List.filter_filter]
  congr 1
  funext entry
  exact Bool.and_comm _ _

/-- For ordered sample ordinals, last-element access gives the same result as
the semantic maximum. Empty samples use the established zero bootstrap. -/
theorem last_index_eq_last (positions : List Nat) (sorted : positions.Pairwise (· ≤ ·)) :
    last_index positions = positions.getLast?.getD 0 := by
  induction positions with
  | nil => rfl
  | cons first rest ih =>
    obtain ⟨head_le, tail_sorted⟩ := List.pairwise_cons.mp sorted
    cases rest with
    | nil => simp [last_index]
    | cons next tail =>
      cases found : (next :: tail).getLast? with
      | none => simp at found
      | some last =>
        have le := head_le last (List.mem_of_getLast? found)
        rw [last_index, ih tail_sorted]
        simp [found, Nat.max_eq_right le]

/-- Read one retained sample at its origin ordinal. Zero passes no sample and
uses the established zero bootstrap. No query comparisons occur here. -/
def carry (borrowed : List occurrence) (passed : Nat) : Nat :=
  if passed = 0 then 0 else (borrowed[passed - 1]?.map occurrence.ordinal).getD 0

theorem carry_eq_last (borrowed : List occurrence) (passed : Nat)
    (inside : passed ≤ borrowed.length) :
    carry borrowed passed = (((borrowed.take passed).getLast?).map occurrence.ordinal).getD 0 := by
  by_cases empty : passed = 0
  · simp [carry, empty]
  · have valid : passed - 1 < borrowed.length := by omega
    simp [carry, empty, List.getLast?_take, List.getElem?_eq_getElem valid]

theorem rank_le_origin (parent : List occurrence) (cut : Nat) :
    rank occurrence.borrowed parent cut ≤ (parent.filter occurrence.borrowed).length := by
  have lengths := congrArg List.length (take_rank occurrence.borrowed parent cut)
  simp only [List.length_take] at lengths
  change min (rank occurrence.borrowed parent cut) (parent.filter occurrence.borrowed).length =
    rank occurrence.borrowed parent cut at lengths
  omega

/-- The outgoing route equals exact child sampling, but its executable definition
uses only a parent rank and retained sample access, without rerouting the child. -/
theorem carry_correct (native child : List occurrence) (K query cut : Nat)
    (certified : cut_certificate (build_parent native child K) query cut) :
    carry (borrowed_samples child K) (rank occurrence.borrowed (build_parent native child K) cut) =
      route child K query := by
  have inside := rank_le_origin (build_parent native child K) cut
  rw [build_parent_borrowed native child K] at inside
  rw [carry_eq_last _ _ inside, ← List.getLast?_map]
  have ordered := (borrowed_ordinals_sorted child K).take
      (i := rank occurrence.borrowed (build_parent native child K) cut)
  rw [← List.map_take] at ordered
  rw [← last_index_eq_last _ ordered, passed_samples native child K query cut certified]
  simp [borrowed_samples, borrowed_entry, samples, List.filter_map, List.map_map,
    Function.comp_def, qualifies, route, key_at]

/-- The computed carry brackets the child's global predecessor in at most K
entries, including equality runs and a final partial group. -/
theorem carried_bracket (native child : List occurrence) (K query cut answer : Nat)
    (positive : 0 < K) (child_sorted : key_sorted child)
    (certified : cut_certificate (build_parent native child K) query cut)
    (found : predecessor child query = some answer) :
    let lo := carry (borrowed_samples child K)
      (rank occurrence.borrowed (build_parent native child K) cut)
    lo ≤ answer ∧ answer < min (lo + K) child.length := by
  dsimp
  rw [carry_correct native child K query cut certified]
  exact predecessor_bracket child K query positive child_sorted answer found

/-- Search only a supplied parent window and translate its result to an absolute ordinal. -/
def local_predecessor (parent : List occurrence) (lo hi query : Nat) : Option Nat :=
  (predecessor (window parent lo hi) query).map (lo + ·)

/-- An incoming bracket is enough for local search; no route is recomputed.
The bracket premise is precisely what the preceding edge's theorem supplies. -/
theorem local_predecessor_correct (parent : List occurrence) (lo hi query : Nat)
    (ordered : lo ≤ hi) (inside : hi ≤ parent.length)
    (bracket : ∀ answer, predecessor parent query = some answer → lo ≤ answer ∧ answer < hi) :
    local_predecessor parent lo hi query = predecessor parent query := by
  have length : (window parent lo hi).length = hi - lo := by
    simp only [window, List.length_take, List.length_drop]; omega
  cases found : predecessor parent query with
  | none =>
    have absent := (predecessor_none parent query).mp found
    have local_none : predecessor (window parent lo hi) query = none := by
      apply (predecessor_none _ _).mpr
      intro k hk
      rw [key_at_window parent lo hi k hk]
      apply absent
      rw [length] at hk
      omega
    simp [local_predecessor, local_none]
  | some answer =>
    have bounds := bracket answer found
    have spec := predecessor_spec parent query answer found
    have relative_inside : answer - lo < (window parent lo hi).length := by rw [length]; omega
    have relative_key : key_at (window parent lo hi) (answer - lo) = key_at parent answer := by
      rw [key_at_window parent lo hi (answer - lo) relative_inside]
      congr 1; omega
    obtain ⟨j, local_found⟩ := predecessor_exists (window parent lo hi) query (answer - lo)
      relative_inside (by rw [relative_key]; exact spec.2.1)
    have local_spec := predecessor_spec (window parent lo hi) query j local_found
    have global_inside : lo + j < parent.length := by rw [length] at local_spec; omega
    have global_key : key_at parent (lo + j) ≤ query := by
      rw [← key_at_window parent lo hi j local_spec.1]
      exact local_spec.2.1
    have le := spec.2.2 (lo + j) global_inside global_key
    have ge := local_spec.2.2 (answer - lo) relative_inside
      (by rw [relative_key]; exact spec.2.1)
    have same : lo + j = answer := by omega
    simp [local_predecessor, local_found, same]

/-- A stored boundary rank and a local origin scan produce the passed-sample
count. The absent case passes zero samples, including a before-first query. -/
def passed_from_window (parent : List occurrence) (boundary lo hi query : Nat) : Nat :=
  match predecessor (window parent lo hi) query with
  | none => 0
  | some relative => boundary +
      ((window parent lo (lo + relative + 1)).filter occurrence.borrowed).length

theorem passed_from_window_correct (parent : List occurrence) (boundary lo hi query : Nat)
    (exact_boundary : boundary = rank occurrence.borrowed parent lo) :
    passed_from_window parent boundary lo hi query =
      rank occurrence.borrowed parent (after (local_predecessor parent lo hi query)) := by
  subst boundary
  cases found : predecessor (window parent lo hi) query with
  | none => simp [passed_from_window, local_predecessor, found, after, rank]
  | some relative =>
    simp only [passed_from_window, local_predecessor, found, after,
      Option.map_some', Option.getD_some]
    exact (rank_split occurrence.borrowed parent lo (lo + relative + 1) (by omega)).symm

/-- Construction retains both the augmented parent and its exact borrowed
sample stream. A query receives this immutable value, not constructor inputs. -/
structure edge where
  parent : List occurrence
  borrowed : List occurrence
  target : List occurrence
  spacing : Nat
  populations : List Nat
  directory : List Nat
  deriving Repr, DecidableEq

def build_edge (native child : List occurrence) (K : Nat) : edge :=
  let parent := build_parent native child K
  let populations := navigation.classes occurrence.borrowed parent K
  ⟨parent, borrowed_samples child K, child, K, populations, navigation.checkpoints populations 128⟩

/-- Complete one-edge transfer: local parent search, boundary-rank correction,
and one retained sample access. Construction is outside this query definition. -/
def transfer (stored : edge) (boundary lo hi query : Nat) : Nat :=
  carry stored.borrowed (passed_from_window stored.parent boundary lo hi query)

theorem transfer_correct (native child : List occurrence) (K boundary lo hi query : Nat)
    (exact_boundary : boundary = rank occurrence.borrowed (build_parent native child K) lo)
    (native_sorted : key_sorted native) (child_sorted : key_sorted child)
    (ordered : lo ≤ hi) (inside : hi ≤ (build_parent native child K).length)
    (bracket : ∀ answer, predecessor (build_parent native child K) query = some answer →
      lo ≤ answer ∧ answer < hi) :
    transfer (build_edge native child K) boundary lo hi query = route child K query := by
  change carry (borrowed_samples child K)
    (passed_from_window (build_parent native child K) boundary lo hi query) = _
  rw [passed_from_window_correct _ boundary lo hi query exact_boundary,
    local_predecessor_correct _ lo hi query ordered inside bracket]
  exact carry_correct native child K query _
    (predecessor_cut _ query (build_parent_sorted native child K native_sorted child_sorted))

/-- Use the stored class/checkpoint directory at an existing group boundary.
No full parent prefix is scanned by this executable definition. -/
def transfer_at_group (stored : edge) (group hi query : Nat) : Option Nat :=
  (navigation.boundary_rank stored.populations stored.directory 128 group).map
    (fun boundary => transfer stored boundary (group * stored.spacing) hi query)

/-- The concrete builder discharges the directory and sample correspondence
obligations. Only the incoming window invariant and sorted source inputs remain. -/
theorem transfer_at_group_correct (native child : List occurrence) (K group hi query : Nat)
    (native_sorted : key_sorted native) (child_sorted : key_sorted child)
    (valid_group : group < (navigation.classes occurrence.borrowed (build_parent native child K) K).length)
    (ordered : group * K ≤ hi) (inside : hi ≤ (build_parent native child K).length)
    (bracket : ∀ answer, predecessor (build_parent native child K) query = some answer →
      group * K ≤ answer ∧ answer < hi) :
    transfer_at_group (build_edge native child K) group hi query = some (route child K query) := by
  unfold transfer_at_group
  have boundary := navigation.grouped_rank_correct occurrence.borrowed
    (build_parent native child K) K 128 group (by decide) valid_group
  change (navigation.boundary_rank
    (navigation.classes occurrence.borrowed (build_parent native child K) K)
    (navigation.checkpoints (navigation.classes occurrence.borrowed (build_parent native child K) K) 128)
    128 group).map (fun boundary => transfer (build_edge native child K) boundary (group * K) hi query) = _
  rw [boundary, Option.map_some']
  exact congrArg some (transfer_correct native child K _ (group * K) hi query rfl
    native_sorted child_sorted ordered inside bracket)

/-- The local origin correction never expands the incoming window budget. -/
theorem origin_scan_budget (parent : List occurrence) (lo hi query relative : Nat)
    (found : predecessor (window parent lo hi) query = some relative) :
    (window parent lo (lo + relative + 1)).length ≤ (window parent lo hi).length := by
  have valid := (predecessor_spec (window parent lo hi) query relative found).1
  simp only [window, List.length_take, List.length_drop] at *
  omega

/-- Follow the retained target edge and search only the outgoing K-entry window.
The outer Option distinguishes an invalid parent group from an absent key. -/
def descend_at_group (stored : edge) (group hi query : Nat) : Option (Option Nat) :=
  (transfer_at_group stored group hi query).map (fun lo =>
    local_predecessor stored.target lo (min (lo + stored.spacing) stored.target.length) query)

/-- One complete cascade edge: actual builder, stored rank directory, local
parent scan, retained sample access and bounded child search. -/
theorem descend_at_group_correct (native child : List occurrence) (K group hi query : Nat)
    (positive : 0 < K) (native_sorted : key_sorted native) (child_sorted : key_sorted child)
    (valid_group : group < (navigation.classes occurrence.borrowed (build_parent native child K) K).length)
    (ordered : group * K ≤ hi) (inside : hi ≤ (build_parent native child K).length)
    (bracket : ∀ answer, predecessor (build_parent native child K) query = some answer →
      group * K ≤ answer ∧ answer < hi) :
    descend_at_group (build_edge native child K) group hi query = some (predecessor child query) := by
  unfold descend_at_group
  rw [transfer_at_group_correct native child K group hi query native_sorted child_sorted
    valid_group ordered inside bracket, Option.map_some']
  change some (routed_predecessor child K query) = _
  rw [routed_predecessor_correct child K query positive child_sorted]

theorem child_window_budget (child : List occurrence) (K lo : Nat) :
    (window child lo (min (lo + K) child.length)).length ≤ K := by
  simp only [window, List.length_take, List.length_drop]
  omega

-- Equality runs remain complete, while native entries precede equal borrows.
def example_child : List occurrence :=
  [⟨1, false, 0⟩, ⟨2, false, 1⟩, ⟨2, false, 2⟩, ⟨2, false, 3⟩,
   ⟨2, false, 4⟩, ⟨2, false, 5⟩, ⟨2, false, 6⟩, ⟨9, false, 7⟩]
def example_parent : List occurrence :=
  [⟨0, false, 10⟩, ⟨1, true, 0⟩, ⟨2, false, 11⟩,
   ⟨2, true, 3⟩, ⟨2, true, 6⟩, ⟨8, false, 12⟩]

theorem example_parent_built :
    build_parent [⟨0, false, 10⟩, ⟨2, false, 11⟩, ⟨8, false, 12⟩] example_child 3 = example_parent := by
  simp [build_parent, native_entries, borrowed_samples, samples, sample_positions,
    example_child, example_parent, List.range_succ, borrowed_entry, augment, List.cons_merge_cons,
    before, priority]

def example_edge : edge := build_edge [⟨0, false, 10⟩, ⟨2, false, 11⟩, ⟨8, false, 12⟩] example_child 3

theorem example_edge_built : example_edge =
    ⟨example_parent, [⟨1, true, 0⟩, ⟨2, true, 3⟩, ⟨2, true, 6⟩], example_child, 3, [1, 2], [0]⟩ := by
  unfold example_edge build_edge
  rw [example_parent_built]
  decide

example : example_edge.parent.map (fun x => (x.key, x.borrowed, x.ordinal)) =
    [(0, false, 10), (1, true, 0), (2, false, 11), (2, true, 3), (2, true, 6), (8, false, 12)] := by
  rw [example_edge_built]; decide
example : transfer_at_group example_edge 1 6 2 = some 6 := by rw [example_edge_built]; decide
example : descend_at_group example_edge 1 6 2 = some (some 6) := by rw [example_edge_built]; decide
example : descend_at_group example_edge 0 3 0 = some none := by rw [example_edge_built]; decide
example : descend_at_group example_edge 1 6 99 = some (some 7) := by rw [example_edge_built]; decide
example : transfer_at_group example_edge 2 6 2 = none := by rw [example_edge_built]; decide
example : transfer (build_edge [] [] 3) 0 0 0 7 = 0 := by decide
example : descend_at_group (build_edge [] [] 3) 0 0 7 = none := by
  simp only [descend_at_group, transfer_at_group, build_edge, build_parent, native_entries,
    augment, List.map_nil, List.nil_merge]
  decide
example : descend_at_group (build_edge [⟨1, false, 0⟩] [] 3) 0 1 1 = some none := by
  simp only [descend_at_group, transfer_at_group, build_edge, build_parent, native_entries,
    borrowed_samples, samples, sample_positions, List.length_nil, List.range_zero,
    List.filter_nil, List.map_nil, augment, List.merge_right]
  decide
-- A tiny child uses its short final window; no native source is required.
example : descend_at_group (build_edge [] [⟨4, false, 0⟩, ⟨6, false, 1⟩] 15) 0 1 6 =
    some (some 1) := by
  simp only [descend_at_group, transfer_at_group, build_edge, build_parent, native_entries,
    augment, List.map_nil, List.nil_merge]
  decide

end Everett.carried_route
