/-
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-/
import Everett.CarriedRoute

namespace Everett.carried_chain
open fractional carried_route

/-! Finite compiled one-edge chains. Coherence relates every retained target to
the next parent exactly. This is not the C++ three-origin topology or a proof
about encoded identity admission. -/

inductive chain where
  | leaf (entries : List occurrence)
  | link (stored : edge) (next : chain)
  deriving Repr

def entries : chain → List occurrence
  | .leaf source => source
  | .link stored _ => stored.parent

/-- Every edge comes from the concrete builder and targets exactly the next
immutable entries. Equal lengths or hashes do not suffice. -/
def coherent (K : Nat) : chain → Prop
  | .leaf source => key_sorted source
  | .link stored next => ∃ native,
      stored = build_edge native (entries next) K ∧ key_sorted native ∧ coherent K next

def build_chain (K : Nat) (layers : List (List occurrence)) (terminal : List occurrence) : chain :=
  match layers with
  | [] => .leaf terminal
  | native :: rest =>
    let next := build_chain K rest terminal
    .link (build_edge native (entries next) K) next

theorem build_chain_coherent (K : Nat) (layers : List (List occurrence))
    (terminal : List occurrence) (terminal_sorted : key_sorted terminal)
    (layers_sorted : ∀ native ∈ layers, key_sorted native) :
    coherent K (build_chain K layers terminal) := by
  induction layers with
  | nil => exact terminal_sorted
  | cons native rest ih =>
    exact ⟨native, rfl, layers_sorted native (by simp),
      ih (by intro source member; exact layers_sorted source (by simp [member]))⟩

theorem entries_sorted (K : Nat) (tree : chain) (valid : coherent K tree) : key_sorted (entries tree) := by
  induction tree with
  | leaf source => exact valid
  | link stored next ih =>
    obtain ⟨native, same, native_sorted, rest_valid⟩ := valid
    subst stored
    exact build_parent_sorted native (entries next) K native_sorted (ih rest_valid)

/-- An empty constructed parent cannot conceal a nonempty sampled target. -/
theorem empty_parent_target (native child : List occurrence) (K : Nat)
    (empty : build_parent native child K = []) : child = [] := by
  have lengths := congrArg List.length empty
  simp only [build_parent, augment_length, native_entries, borrowed_samples, samples,
    List.length_map, List.length_nil] at lengths
  have sample_empty : sample_positions child K = [] := List.length_eq_zero_iff.mp (by omega)
  cases child with
  | nil => rfl
  | cons head tail =>
    have present : 0 ∈ sample_positions (head :: tail) K := by simp [mem_sample_positions]
    simp [sample_empty] at present

/-- Nonempty nodes use an aligned existing group. Empty nodes carry [0,0)
and take an explicit path without directory access. -/
def window_valid (source : List occurrence) (K lo hi query : Nat) : Prop :=
  lo ≤ hi ∧ hi ≤ source.length ∧ lo % K = 0 ∧
    (source ≠ [] → lo < source.length) ∧
    ∀ answer, predecessor source query = some answer → lo ≤ answer ∧ answer < hi

theorem root_window (source : List occurrence) (K query : Nat) :
    window_valid source K 0 source.length query := by
  refine ⟨by omega, by omega, by simp, ?_, ?_⟩
  · intro nonempty; exact List.length_pos_iff.mpr nonempty
  · intro answer found
    have inside := (predecessor_spec source query answer found).1
    omega

theorem empty_window (K query : Nat) : window_valid [] K 0 0 query := by
  simpa using root_window [] K query

theorem sampled_window (source : List occurrence) (K query : Nat) (positive : 0 < K)
    (sorted : key_sorted source) :
    window_valid source K (route source K query) (min (route source K query + K) source.length) query := by
  have inside := route_inside source K query
  have route_info := route_member_or_zero source K query
  refine ⟨by omega, Nat.min_le_right _ _, ?_, ?_, ?_⟩
  · rcases route_info with zero | valid
    · simp [zero]
    · exact valid.2.1
  · intro nonempty
    rcases route_info with zero | valid
    · rw [zero]; exact List.length_pos_iff.mpr nonempty
    · exact valid.1
  · exact fun answer found => predecessor_bracket source K query positive sorted answer found

theorem aligned_ordinal (K lo : Nat) (aligned : lo % K = 0) : lo / K * K = lo := by
  have division := Nat.mod_add_div lo K
  rw [aligned, Nat.zero_add, Nat.mul_comm K (lo / K)] at division
  exact division

theorem valid_group (source : List occurrence) (K lo hi query : Nat) (positive : 0 < K)
    (valid : window_valid source K lo hi query) (nonempty : source ≠ []) :
    lo / K < (navigation.classes occurrence.borrowed source K).length := by
  have inside := valid.2.2.2.1 nonempty
  have coverage := navigation.groups_cover source.length K positive
  simp only [navigation.classes, List.length_map, List.length_range]
  apply (Nat.div_lt_iff_lt_mul positive).mpr
  omega

/-- Empty nodes never request nonexistent group zero. Links use only their
stored directory, carried parent window and retained target/sample stream. -/
def walk (query : Nat) : chain → Nat → Nat → Option (List (Option Nat))
  | .leaf source, lo, hi => some [local_predecessor source lo hi query]
  | .link stored next, lo, hi => do
    let later ← if stored.parent = [] then walk query next 0 0 else do
      let child_lo ← transfer_at_group stored (lo / stored.spacing) hi query
      walk query next child_lo (min (child_lo + stored.spacing) stored.target.length)
    return local_predecessor stored.parent lo hi query :: later

def full_search (query : Nat) : chain → List (Option Nat)
  | .leaf source => [predecessor source query]
  | .link stored next => predecessor stored.parent query :: full_search query next

/-- Coherence and the incoming bracket suffice for every node. The executable
walk never independently routes through a child's complete sample stream. -/
theorem walk_correct (tree : chain) (K lo hi query : Nat) (positive : 0 < K)
    (coherence : coherent K tree) (valid : window_valid (entries tree) K lo hi query) :
    walk query tree lo hi = some (full_search query tree) := by
  induction tree generalizing lo hi with
  | leaf source =>
    have local_result := local_predecessor_correct source lo hi query valid.1 valid.2.1 valid.2.2.2.2
    simp [walk, full_search, local_result]
  | link stored next ih =>
    obtain ⟨native, same, native_sorted, next_coherent⟩ := coherence
    subst stored
    have child_sorted := entries_sorted K next next_coherent
    have parent_local := local_predecessor_correct (build_parent native (entries next) K)
      lo hi query valid.1 valid.2.1 valid.2.2.2.2
    by_cases empty : build_parent native (entries next) K = []
    · have child_empty := empty_parent_target native (entries next) K empty
      have next_window : window_valid (entries next) K 0 0 query := by
        rw [child_empty]; exact empty_window K query
      have result := ih 0 0 next_coherent next_window
      simp [walk, full_search, build_edge, empty, result, parent_local,
        local_predecessor, window, predecessor, qualifying_indices]
    · have aligned := aligned_ordinal K lo valid.2.2.1
      have group := valid_group (build_parent native (entries next) K) K lo hi query positive valid empty
      have transfer_result := transfer_at_group_correct native (entries next) K (lo / K) hi query
        native_sorted child_sorted group (by simpa [aligned] using valid.1) valid.2.1
        (by simpa [aligned] using valid.2.2.2.2)
      have next_window := sampled_window (entries next) K query positive child_sorted
      have result := ih _ _ next_coherent next_window
      simp only [walk, build_edge, empty, ↓reduceIte]
      change (do
        let child_lo ← transfer_at_group (build_edge native (entries next) K) (lo / K) hi query
        let later ← walk query next child_lo (min (child_lo + K) (entries next).length)
        pure (local_predecessor (build_parent native (entries next) K) lo hi query :: later)) = _
      rw [transfer_result]
      simp [result, full_search, build_edge, parent_local]

/-- Only the root starts with a complete source window. -/
def search (query : Nat) (tree : chain) : Option (List (Option Nat)) :=
  walk query tree 0 (entries tree).length

theorem search_correct (tree : chain) (K query : Nat) (positive : 0 < K)
    (coherence : coherent K tree) : search query tree = some (full_search query tree) :=
  walk_correct tree K 0 _ query positive coherence (root_window _ K query)

/-- The constructor discharges coherence; callers provide only sorted source
layers and a positive sampling spacing. -/
theorem search_built (K query : Nat) (layers : List (List occurrence))
    (terminal : List occurrence) (positive : 0 < K) (terminal_sorted : key_sorted terminal)
    (layers_sorted : ∀ native ∈ layers, key_sorted native) :
    search query (build_chain K layers terminal) =
      some (full_search query (build_chain K layers terminal)) :=
  search_correct _ K query positive (build_chain_coherent K layers terminal terminal_sorted layers_sorted)

/-- Coherence preserves the exact retained target, not just its size. -/
theorem target_exact (stored : edge) (next : chain) (K : Nat)
    (valid : coherent K (.link stored next)) : stored.target = entries next := by
  obtain ⟨native, same, _, _⟩ := valid
  subst stored
  rfl

/-- Every nonempty-path recursive window is bounded in entries. This does not
count list traversal or merge the two local scans performed by the reference walk. -/
theorem link_window_budget (stored : edge) (next : chain) (K lo : Nat)
    (valid : coherent K (.link stored next)) :
    (window (entries next) lo (min (lo + stored.spacing) stored.target.length)).length ≤ K := by
  obtain ⟨native, same, _, _⟩ := valid
  subst stored
  exact child_window_budget (entries next) K lo

def height : chain → Nat
  | .leaf _ => 1
  | .link _ next => height next + 1

theorem full_search_length (tree : chain) (query : Nat) :
    (full_search query tree).length = height tree := by
  induction tree with
  | leaf source => rfl
  | link stored next ih => simp [full_search, height, ih]

/-- Exactly one answer is retained per node, including empty descendants. -/
theorem answer_count (tree : chain) (K query : Nat) (positive : 0 < K)
    (valid : coherent K tree) : (search query tree).map List.length = some (height tree) := by
  rw [search_correct tree K query positive valid]
  simp [full_search_length]

-- Three nodes exercise two actual carried handoffs: route 3, then route 6.
def top_edge : edge :=
  ⟨[⟨0, true, 0⟩, ⟨2, true, 3⟩], [⟨0, true, 0⟩, ⟨2, true, 3⟩],
   example_parent, 3, [2], [0]⟩

theorem top_edge_built : top_edge = build_edge [] example_parent 3 := by
  simp only [build_edge, build_parent, native_entries, augment, List.map_nil, List.nil_merge]
  decide

def example_chain : chain := .link top_edge (.link example_edge (.leaf example_child))

theorem example_chain_coherent : coherent 3 example_chain := by
  refine ⟨[], ?_, ?_, ?_⟩
  · change top_edge = build_edge [] example_edge.parent 3
    rw [example_edge_built]
    exact top_edge_built
  · intro i j hi; simp at hi
  · refine ⟨[⟨0, false, 10⟩, ⟨2, false, 11⟩, ⟨8, false, 12⟩], rfl, ?_, ?_⟩
    · exact sorted_keys _ (by decide)
    · exact sorted_keys _ (by decide)

example : search 2 example_chain = some [some 1, some 4, some 6] := by
  unfold example_chain
  rw [example_edge_built]
  decide
example : search 0 example_chain = some [some 0, some 0, none] := by
  unfold example_chain
  rw [example_edge_built]
  decide
example : search 99 example_chain = some [some 1, some 5, some 7] := by
  unfold example_chain
  rw [example_edge_built]
  decide
example : search 2 example_chain = some (full_search 2 example_chain) :=
  search_correct example_chain 3 2 (by decide) example_chain_coherent

-- Empty descendants are traversed without any nonexistent group-zero lookup.
example : search 7 (build_chain 3 [[], []] []) = some [none, none, none] := by
  simp [search, build_chain, entries, walk, build_edge, build_parent, native_entries,
    borrowed_samples, samples, sample_positions, augment, local_predecessor, window,
    predecessor, qualifying_indices]

-- Retargeting an edge to a different next parent breaks coherence.
example : ¬ coherent 3 (.link example_edge (.leaf [])) := by
  intro valid
  have exact_target := target_exact example_edge (.leaf []) 3 valid
  change example_child = [] at exact_target
  contradiction

-- Equal lengths still do not certify the retained target.
example : (List.replicate example_child.length (⟨1, false, 0⟩ : occurrence)).length =
    example_child.length := by simp
example : ¬ coherent 3 (.link example_edge
    (.leaf (List.replicate example_child.length ⟨1, false, 0⟩))) := by
  intro valid
  have exact_target := target_exact example_edge
    (.leaf (List.replicate example_child.length ⟨1, false, 0⟩)) 3 valid
  change example_child = List.replicate example_child.length ⟨1, false, 0⟩ at exact_target
  have different : example_child ≠ List.replicate example_child.length ⟨1, false, 0⟩ := by decide
  exact different exact_target

end Everett.carried_chain
