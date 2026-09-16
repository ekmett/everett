/-
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-/
import Everett.Snapshots

namespace Everett.fractional

/-- Equal keys retain their complete occurrence labels and multiplicity.
Callers assign labels; distinctness is not assumed by this structure.
Natural keys model ordering, without asserting a string encoding. -/
structure occurrence where
  key : Nat
  borrowed : Bool
  ordinal : Nat
  deriving Repr, DecidableEq, Inhabited

def priority (a : occurrence) : Nat := if a.borrowed then 1 else 0

def before (a b : occurrence) : Bool :=
  decide (a.key < b.key ∨ a.key = b.key ∧ priority a ≤ priority b)

theorem before_trans (a b c : occurrence) (ab : before a b) (bc : before b c) :
    before a c := by
  simp only [before, decide_eq_true_eq] at *
  omega

theorem before_total (a b : occurrence) : before a b || before b a := by
  simp only [before, Bool.or_eq_true, decide_eq_true_eq]
  omega

/-- A stable merge; equal native occurrences precede borrowed occurrences. -/
def augment (native borrowed : List occurrence) : List occurrence :=
  native.merge borrowed before

/-- Permutation preserves multiplicity and complete identities, not just keys. -/
theorem augment_preserves_occurrences (native borrowed : List occurrence) :
    (augment native borrowed).Perm (native ++ borrowed) :=
  List.merge_perm_append before

theorem augment_length (native borrowed : List occurrence) :
    (augment native borrowed).length = native.length + borrowed.length := by
  simp [augment]

theorem augment_sorted (native borrowed : List occurrence)
    (hn : native.Pairwise (fun a b => before a b = true))
    (hb : borrowed.Pairwise (fun a b => before a b = true)) :
    (augment native borrowed).Pairwise (fun a b => before a b = true) :=
  List.sorted_merge before_trans before_total native borrowed hn hb

/-- Origin filtering recovers an exact input sequence in its original order. -/
theorem augment_filter (native borrowed : List occurrence) (p : occurrence → Bool)
    (hn : ∀ a ∈ native, p a = true) (hb : ∀ a ∈ borrowed, p a = false) :
    (augment native borrowed).filter p = native := by
  induction native generalizing borrowed with
  | nil =>
    simp only [augment, List.nil_merge]
    exact List.filter_eq_nil_iff.mpr (by intro a ha; simp [hb a ha])
  | cons a native ih =>
    induction borrowed with
    | nil =>
      simp only [augment, List.merge_right]
      exact List.filter_eq_self.mpr hn
    | cons b borrowed ihb =>
      have ha : p a = true := hn a (by simp)
      have hb' : p b = false := hb b (by simp)
      have hnt : ∀ x ∈ native, p x = true := by intro x hx; exact hn x (by simp [hx])
      have hbt : ∀ x ∈ borrowed, p x = false := by intro x hx; exact hb x (by simp [hx])
      simp only [augment, List.cons_merge_cons]
      split
      · simpa only [List.filter_cons, ha, Bool.true_eq, ↓reduceIte] using
          congrArg (List.cons a) (ih (b :: borrowed) hnt hb)
      · simpa only [List.filter_cons, hb', Bool.false_eq_true, ↓reduceIte] using ihb hbt

theorem equal_native_first (xs : List occurrence)
    (sorted : xs.Pairwise (fun a b => before a b = true))
    (i j : Nat) (hi : i < xs.length) (hj : j < xs.length)
    (native : xs[i].borrowed = false) (borrowed : xs[j].borrowed = true)
    (same_key : xs[i].key = xs[j].key) : i < j := by
  by_cases eq : i = j
  · subst j; simp [native] at borrowed
  · by_cases h : i < j
    · exact h
    have ji : j < i := by omega
    have order := List.pairwise_iff_getElem.mp sorted j i hj hi ji
    simp only [before, decide_eq_true_eq, priority, native, borrowed] at order
    simp only [Bool.false_eq_true, ↓reduceIte] at order
    omega

/-- Mathematical rank counts an origin predicate in a half-open prefix. -/
def rank (p : α → Bool) (xs : List α) (cut : Nat) : Nat :=
  ((xs.take cut).filter p).length

def window (xs : List α) (lo hi : Nat) : List α := (xs.drop lo).take (hi - lo)

def projection (p : α → Bool) (xs : List α) (lo hi : Nat) : List α :=
  ((xs.filter p).drop (rank p xs lo)).take (rank p xs hi - rank p xs lo)

theorem take_rank (p : α → Bool) (xs : List α) (cut : Nat) :
    (xs.filter p).take (rank p xs cut) = (xs.take cut).filter p := by
  calc
    (xs.filter p).take (rank p xs cut) =
        (((xs.take cut).filter p) ++ ((xs.drop cut).filter p)).take (rank p xs cut) := by
      rw [← List.filter_append, List.take_append_drop]
    _ = _ := by simp [rank]

theorem take_split (xs : List α) (lo hi : Nat) (ordered : lo ≤ hi) :
    xs.take hi = xs.take lo ++ window xs lo hi := by
  calc
    xs.take hi = xs.take (lo + (hi - lo)) := by congr 1; omega
    _ = xs.take lo ++ window xs lo hi := List.take_add

/-- Endpoint ranks recover the complete projected interval in source order. -/
theorem project_window (p : α → Bool) (xs : List α) (lo hi : Nat)
    (ordered : lo ≤ hi) : projection p xs lo hi = (window xs lo hi).filter p := by
  unfold projection
  rw [← List.drop_take, take_rank, take_split xs lo hi ordered, List.filter_append]
  simp [rank]

theorem rank_split (p : α → Bool) (xs : List α) (lo hi : Nat)
    (ordered : lo ≤ hi) :
    rank p xs hi = rank p xs lo + ((window xs lo hi).filter p).length := by
  simp only [rank, take_split xs lo hi ordered, List.filter_append, List.length_append]

theorem projected_length (p : α → Bool) (xs : List α) (lo hi : Nat)
    (ordered : lo ≤ hi) :
    (projection p xs lo hi).length = rank p xs hi - rank p xs lo := by
  rw [project_window p xs lo hi ordered, rank_split p xs lo hi ordered]
  omega

theorem complement_lengths (p : α → Bool) (xs : List α) :
    (xs.filter p).length + (xs.filter (fun x => !p x)).length = xs.length := by
  simpa [List.countP_eq_length_filter] using
    (List.length_eq_countP_add_countP p (l := xs)).symm

theorem rank_complement (p : α → Bool) (xs : List α) (cut : Nat) :
    rank p xs cut + rank (fun x => !p x) xs cut = min cut xs.length := by
  simpa [rank] using complement_lengths p (xs.take cut)

/-- The two source ranges share exactly one virtual-window entry budget. -/
theorem projected_lengths_sum (p : α → Bool) (xs : List α) (lo hi : Nat)
    (ordered : lo ≤ hi) (inside : hi ≤ xs.length) :
    (projection p xs lo hi).length +
      (projection (fun x => !p x) xs lo hi).length = hi - lo := by
  rw [project_window p xs lo hi ordered, project_window _ xs lo hi ordered,
    complement_lengths]
  simp only [window, List.length_take, List.length_drop]
  omega

theorem mem_projected_window (p : α → Bool) (xs : List α) (lo hi : Nat)
    (ordered : lo ≤ hi) (x : α) :
    x ∈ window xs lo hi ↔ x ∈ projection p xs lo hi ∨
      x ∈ projection (fun a => !p a) xs lo hi := by
  rw [project_window p xs lo hi ordered, project_window _ xs lo hi ordered]
  simp only [List.mem_filter, Bool.not_eq_true]
  cases hp : p x <;> simp [hp]

/-- This finite enumeration is a semantic specification, not a binary-search cost model. -/
def sample_positions (xs : List occurrence) (K : Nat) : List Nat :=
  (List.range xs.length).filter (fun i => i % K == 0)

def key_at (xs : List occurrence) (i : Nat) : Nat := xs[i]!.key

def samples (xs : List occurrence) (K : Nat) : List (Nat × occurrence) :=
  (sample_positions xs K).map (fun i => (i, xs[i]!))

theorem mem_sample_positions (xs : List occurrence) (K i : Nat) :
    i ∈ sample_positions xs K ↔ i < xs.length ∧ i % K = 0 := by
  simp [sample_positions]

theorem every_Kth (xs : List occurrence) (K j : Nat) (inside : j * K < xs.length) :
    j * K ∈ sample_positions xs K := by
  simp [mem_sample_positions, inside]

theorem only_Kth (xs : List occurrence) (K i : Nat)
    (member : i ∈ sample_positions xs K) : ∃ j, i = j * K := by
  have h := (mem_sample_positions xs K i).mp member
  exact ⟨i / K, by have := Nat.mod_add_div i K; rw [h.2, Nat.mul_comm K (i / K)] at this; omega⟩

theorem sample_identity (xs : List occurrence) (K i : Nat)
    (member : i ∈ sample_positions xs K) : (i, xs[i]!) ∈ samples xs K := by
  exact List.mem_map.mpr ⟨i, member, rfl⟩

def last_index : List Nat → Nat
  | [] => 0
  | i :: rest => max i (last_index rest)

theorem member_le_last (xs : List Nat) (i : Nat) (member : i ∈ xs) : i ≤ last_index xs := by
  induction xs with
  | nil => simp at member
  | cons a rest ih =>
    simp only [List.mem_cons] at member
    rcases member with rfl | member
    · exact Nat.le_max_left _ _
    · exact Nat.le_trans (ih member) (Nat.le_max_right _ _)

theorem last_le (xs : List Nat) (bound : Nat)
    (bounded : ∀ i ∈ xs, i ≤ bound) : last_index xs ≤ bound := by
  induction xs with
  | nil => simp [last_index]
  | cons a rest ih =>
    apply Nat.max_le.mpr
    exact ⟨bounded a (by simp), ih (by intro i hi; exact bounded i (by simp [hi]))⟩

theorem last_zero_or_mem (xs : List Nat) : last_index xs = 0 ∨ last_index xs ∈ xs := by
  induction xs with
  | nil => exact Or.inl rfl
  | cons a rest ih =>
    by_cases h : last_index rest ≤ a
    · right; simp [last_index, Nat.max_eq_left h]
    · have h' : a ≤ last_index rest := by omega
      rw [last_index, Nat.max_eq_right h']
      rcases ih with ih | ih
      · exact Or.inl ih
      · exact Or.inr (by simp [ih])

theorem last_mem (xs : List Nat) (nonempty : xs ≠ []) : last_index xs ∈ xs := by
  rcases last_zero_or_mem xs with zero | member
  · obtain ⟨a, rest, eq⟩ := List.exists_cons_of_ne_nil nonempty
    subst xs
    have le := member_le_last (a :: rest) a (by simp)
    have azero : a = 0 := by omega
    rw [zero]
    simp [azero]
  · exact member

/-- The route is selected only from sampled keys; zero bootstraps an empty/before-first search. -/
def route (xs : List occurrence) (K query : Nat) : Nat :=
  last_index ((sample_positions xs K).filter (fun i => decide (key_at xs i ≤ query)))

def qualifying_indices (xs : List occurrence) (query : Nat) : List Nat :=
  (List.range xs.length).filter (fun i => decide (key_at xs i ≤ query))

def predecessor (xs : List occurrence) (query : Nat) : Option Nat :=
  if qualifying_indices xs query = [] then none else some (last_index (qualifying_indices xs query))

def key_sorted (xs : List occurrence) : Prop :=
  ∀ i j, i < xs.length → j < xs.length → i ≤ j → key_at xs i ≤ key_at xs j

theorem sorted_keys (xs : List occurrence)
    (sorted : xs.Pairwise (fun a b => before a b = true)) : key_sorted xs := by
  intro i j hi hj ij
  by_cases eq : i = j
  · simp [eq]
  · have order := List.pairwise_iff_getElem.mp sorted i j hi hj (by omega)
    simp only [before, decide_eq_true_eq] at order
    simp [key_at, getElem!_pos, hi, hj]
    omega

theorem route_member_or_zero (xs : List occurrence) (K query : Nat) :
    route xs K query = 0 ∨
      (route xs K query < xs.length ∧ route xs K query % K = 0 ∧
       key_at xs (route xs K query) ≤ query) := by
  have h := last_zero_or_mem ((sample_positions xs K).filter
    (fun i => decide (key_at xs i ≤ query)))
  rcases h with zero | member
  · exact Or.inl zero
  · right
    simpa only [List.mem_filter, mem_sample_positions, decide_eq_true_eq, route,
      and_assoc] using member

theorem route_inside (xs : List occurrence) (K query : Nat) : route xs K query ≤ xs.length := by
  rcases route_member_or_zero xs K query with zero | ⟨inside, _⟩ <;> omega

/-- Every qualifying key is strictly before the next sample after the chosen route. -/
theorem route_upper (xs : List occurrence) (K query : Nat) (positive : 0 < K)
    (sorted : key_sorted xs) (i : Nat) (inside : i < xs.length)
    (qualifies : key_at xs i ≤ query) : i < route xs K query + K := by
  let start := i / K * K
  have start_le : start ≤ i := Nat.div_mul_le_self i K
  have start_inside : start < xs.length := by omega
  have start_key : key_at xs start ≤ query :=
    Nat.le_trans (sorted start i start_inside inside start_le) qualifies
  have member : start ∈ (sample_positions xs K).filter
      (fun j => decide (key_at xs j ≤ query)) := by
    simp [List.mem_filter, mem_sample_positions, start_inside, start_key, start]
  have before_route : start ≤ route xs K query := member_le_last _ _ member
  have remainder := Nat.mod_lt i positive
  have decomposition := Nat.mod_add_div i K
  rw [Nat.mul_comm K (i / K)] at decomposition
  change i % K + start = i at decomposition
  omega

theorem route_below_last (xs : List occurrence) (K query : Nat) :
    route xs K query ≤ last_index (qualifying_indices xs query) := by
  apply last_le
  intro i member
  simp only [List.mem_filter, mem_sample_positions, decide_eq_true_eq] at member
  apply member_le_last
  simp [qualifying_indices, member.1.1, member.2]

/-- The global rightmost key ≤ query is in the sampled half-open target window. -/
theorem predecessor_bracket (xs : List occurrence) (K query : Nat) (positive : 0 < K)
    (sorted : key_sorted xs) (i : Nat) (found : predecessor xs query = some i) :
    route xs K query ≤ i ∧ i < min (route xs K query + K) xs.length := by
  unfold predecessor at found
  split at found
  · contradiction
  · rename_i nonempty
    have eq : last_index (qualifying_indices xs query) = i := Option.some.inj found
    have member := last_mem (qualifying_indices xs query) nonempty
    rw [eq] at member
    have info : i < xs.length ∧ key_at xs i ≤ query := by simpa [qualifying_indices] using member
    have lower := route_below_last xs K query
    rw [eq] at lower
    exact ⟨lower, Nat.lt_min.mpr ⟨route_upper xs K query positive sorted i info.1 info.2, info.1⟩⟩

/-- Both projected ranges together contain at most K records, including a short final group. -/
theorem routed_projection_budget (xs : List occurrence) (K query : Nat) :
    (projection occurrence.borrowed xs (route xs K query)
      (min (route xs K query + K) xs.length)).length +
    (projection (fun a => !a.borrowed) xs (route xs K query)
      (min (route xs K query + K) xs.length)).length ≤ K := by
  rw [projected_lengths_sum]
  · omega
  · have := route_inside xs K query; omega
  · exact Nat.min_le_right _ _

/-- The second origin projection also retains its exact source order. -/
theorem augment_filter_right (native borrowed : List occurrence) (p : occurrence → Bool)
    (hn : ∀ a ∈ native, p a = false) (hb : ∀ a ∈ borrowed, p a = true) :
    (augment native borrowed).filter p = borrowed := by
  induction native generalizing borrowed with
  | nil =>
    simp only [augment, List.nil_merge]
    exact List.filter_eq_self.mpr hb
  | cons a native ih =>
    induction borrowed with
    | nil =>
      simp only [augment, List.merge_right]
      exact List.filter_eq_nil_iff.mpr (by intro a ha; simp [hn a ha])
    | cons b borrowed ihb =>
      have ha : p a = false := hn a (by simp)
      have hb' : p b = true := hb b (by simp)
      have hnt : ∀ x ∈ native, p x = false := by intro x hx; exact hn x (by simp [hx])
      have hbt : ∀ x ∈ borrowed, p x = true := by intro x hx; exact hb x (by simp [hx])
      simp only [augment, List.cons_merge_cons]
      split
      · simpa only [List.filter_cons, ha, Bool.false_eq_true, ↓reduceIte] using
          ih (b :: borrowed) hnt hb
      · simpa only [List.filter_cons, hb', Bool.true_eq, ↓reduceIte] using
          congrArg (List.cons b) (ihb hbt)

theorem mem_window (xs : List α) (lo hi : Nat) (ordered : lo ≤ hi) (x : α) :
    x ∈ window xs lo hi ↔
      ∃ (i : Nat) (inside : i < xs.length), lo ≤ i ∧ i < hi ∧ xs[i] = x := by
  simp only [window, List.mem_take_iff_getElem, List.length_drop]
  constructor
  · rintro ⟨j, hj, eq⟩
    refine ⟨lo + j, by omega, by omega, by omega, ?_⟩
    simpa only [List.getElem_drop] using eq
  · rintro ⟨i, inside, lower, upper, eq⟩
    refine ⟨i - lo, by omega, ?_⟩
    simpa only [List.getElem_drop, Nat.add_sub_of_le lower] using eq

/-- Searching both projected ranges can recover the global augmented predecessor.
Native equality lookup additionally needs `false_borrow_recovery` below. -/
theorem predecessor_in_projection (xs : List occurrence) (K query : Nat) (positive : 0 < K)
    (sorted : key_sorted xs) (i : Nat) (found : predecessor xs query = some i) :
    xs[i]! ∈ projection occurrence.borrowed xs (route xs K query)
        (min (route xs K query + K) xs.length) ∨
      xs[i]! ∈ projection (fun a => !a.borrowed) xs (route xs K query)
        (min (route xs K query + K) xs.length) := by
  have bracket := predecessor_bracket xs K query positive sorted i found
  have inside : i < xs.length := by omega
  have ordered : route xs K query ≤ min (route xs K query + K) xs.length := by omega
  apply (mem_projected_window _ _ _ _ ordered _).mp
  apply (mem_window _ _ _ ordered _).mpr
  exact ⟨i, inside, bracket.1, by omega, by simp [inside]⟩

theorem predecessor_none (xs : List occurrence) (query : Nat) :
    predecessor xs query = none ↔ ∀ i, i < xs.length → query < key_at xs i := by
  constructor
  · intro found
    have h : qualifying_indices xs query = [] := by
      unfold predecessor at found
      split at found
      · assumption
      · contradiction
    intro i inside
    have absent : i ∉ qualifying_indices xs query := by rw [h]; simp
    simp only [qualifying_indices, List.mem_filter, List.mem_range, decide_eq_true_eq] at absent
    omega
  · intro none
    have h : qualifying_indices xs query = [] := by
      apply List.eq_nil_iff_forall_not_mem.mpr
      intro i member
      have info : i < xs.length ∧ key_at xs i ≤ query := by simpa [qualifying_indices] using member
      have := none i info.1
      omega
    simp [predecessor, h]

theorem route_before_first (xs : List occurrence) (K query : Nat)
    (none : predecessor xs query = none) : route xs K query = 0 := by
  rcases route_member_or_zero xs K query with zero | ⟨inside, _, qualifies⟩
  · exact zero
  · have := (predecessor_none xs query).mp none _ inside
    omega

/-- The ordinal of a retained occurrence is its origin rank immediately before it. -/
theorem filter_rank_hit (p : α → Bool) (xs : List α) (i : Nat) (inside : i < xs.length)
    (hit : p xs[i] = true) : (xs.filter p)[rank p xs i]? = some xs[i] := by
  induction xs generalizing i with
  | nil => simp at inside
  | cons a rest ih =>
    cases i with
    | zero =>
      have ha : p a = true := hit
      simp [rank, ha]
    | succ i =>
      have inside' : i < rest.length := by simpa using inside
      have hit' : p rest[i] = true := hit
      have found := ih i inside' hit'
      cases h : p a <;> simpa [rank, h] using found

theorem rank_hit_succ (p : α → Bool) (xs : List α) (i : Nat) (inside : i < xs.length)
    (hit : p xs[i] = true) : rank p xs (i + 1) = rank p xs i + 1 := by
  unfold rank
  rw [List.take_succ_eq_append_getElem inside, List.filter_append]
  simp [hit]

def native (a : occurrence) : Bool := !a.borrowed

/-- A native key-value array has one occurrence per key. Borrowed duplicates remain unrestricted. -/
def unique_native_keys (xs : List occurrence) : Prop :=
  ∀ (i j : Nat) (_ : i < xs.length) (_ : j < xs.length),
    xs[i].borrowed = false → xs[j].borrowed = false → xs[i].key = xs[j].key → i = j

/-- No native occurrence can lie between a native key and any borrowed equal key. -/
theorem no_native_between (xs : List occurrence)
    (sorted : xs.Pairwise (fun a b => before a b = true))
    (unique : unique_native_keys xs) (i j : Nat) (hi : i < xs.length) (hj : j < xs.length)
    (hn : xs[i].borrowed = false) (hb : xs[j].borrowed = true)
    (same : xs[i].key = xs[j].key) :
    (window xs (i + 1) j).filter native = [] := by
  have ordered := equal_native_first xs sorted i j hi hj hn hb same
  apply List.filter_eq_nil_iff.mpr
  intro a member
  obtain ⟨k, hk, ik, kj, eq⟩ := (mem_window xs (i + 1) j (by omega) a).mp member
  intro hn'
  have native_k : xs[k].borrowed = false := by simpa [native, ← eq] using hn'
  have keys := sorted_keys xs sorted
  have lower := keys i k hi hk (by omega)
  have upper := keys k j hk hj (by omega)
  simp only [key_at, getElem!_pos, hi, hk, hj] at lower upper
  have same_k : xs[i].key = xs[k].key := by omega
  have equal := unique i k hi hk hn native_k same_k
  omega

/-- A true false-borrow flag witnesses a native equal key. Its exact occurrence is
the single native probe at rank(native, borrowed_position)-1, even when it lies
before the routed window. The subtraction cannot underflow. -/
theorem false_borrow_recovery (xs : List occurrence)
    (sorted : xs.Pairwise (fun a b => before a b = true))
    (unique : unique_native_keys xs) (i j : Nat) (hi : i < xs.length) (hj : j < xs.length)
    (hn : xs[i].borrowed = false) (hb : xs[j].borrowed = true)
    (same : xs[i].key = xs[j].key) :
    0 < rank native xs j ∧
      (xs.filter native)[rank native xs j - 1]? = some xs[i] := by
  have ordered := equal_native_first xs sorted i j hi hj hn hb same
  have empty := no_native_between xs sorted unique i j hi hj hn hb same
  have split := rank_split native xs (i + 1) j (by omega)
  rw [empty] at split
  have hit : native xs[i] = true := by simp [native, hn]
  have successor := rank_hit_succ native xs i hi hit
  have ranks : rank native xs j = rank native xs i + 1 := by simp_all
  constructor
  · omega
  · rw [ranks]
    simpa using filter_rank_hit native xs i hi hit

theorem predecessor_spec (xs : List occurrence) (query i : Nat)
    (found : predecessor xs query = some i) :
    i < xs.length ∧ key_at xs i ≤ query ∧
      ∀ j, j < xs.length → key_at xs j ≤ query → j ≤ i := by
  unfold predecessor at found
  split at found
  · contradiction
  · rename_i nonempty
    have eq : last_index (qualifying_indices xs query) = i := Option.some.inj found
    have member := last_mem (qualifying_indices xs query) nonempty
    rw [eq] at member
    have info : i < xs.length ∧ key_at xs i ≤ query := by simpa [qualifying_indices] using member
    refine ⟨info.1, info.2, ?_⟩
    intro j inside qualifies
    rw [← eq]
    exact member_le_last _ _ (by simp [qualifying_indices, inside, qualifies])

/-- Every native equality hit is either inside the native projected window or
recovered by one rank-derived probe from its borrowed augmented predecessor.
The latter case is exactly the semantic condition certified by a false-borrow bit. -/
theorem native_match_candidates (xs : List occurrence) (K query i j : Nat)
    (positive : 0 < K) (sorted : xs.Pairwise (fun a b => before a b = true))
    (unique : unique_native_keys xs) (hi : i < xs.length)
    (hn : xs[i].borrowed = false) (same : xs[i].key = query)
    (found : predecessor xs query = some j) :
    xs[i] ∈ projection native xs (route xs K query)
        (min (route xs K query + K) xs.length) ∨
      (xs[j]!.borrowed = true ∧ key_at xs j = query ∧ 0 < rank native xs j ∧
        (xs.filter native)[rank native xs j - 1]? = some xs[i]) := by
  have spec := predecessor_spec xs query j found
  have hj := spec.1
  have keys := sorted_keys xs sorted
  have ij : i ≤ j := spec.2.2 i hi (by simp [key_at, hi, same])
  have lower := keys i j hi hj ij
  have upper := spec.2.1
  simp only [key_at, getElem!_pos, hi, hj] at lower upper
  have same_j : xs[j].key = query := by omega
  cases hb : xs[j].borrowed with
  | false =>
    have eq := unique i j hi hj hn hb (by omega)
    subst j
    left
    have bracket := predecessor_bracket xs K query positive keys i found
    have ordered : route xs K query ≤ min (route xs K query + K) xs.length := by omega
    rw [project_window _ _ _ _ ordered]
    apply List.mem_filter.mpr
    exact ⟨(mem_window _ _ _ ordered _).mpr ⟨i, hi, bracket.1, by omega, rfl⟩,
      by simp [native, hn]⟩
  | true =>
    right
    have recovery := false_borrow_recovery xs sorted unique i j hi hj hn hb (by omega)
    exact ⟨by simpa [hj], by simp [key_at, hj, same_j], recovery.1, recovery.2⟩

/-- Any qualifying occurrence ensures that the global predecessor exists. -/
theorem predecessor_exists (xs : List occurrence) (query i : Nat)
    (inside : i < xs.length) (qualifies : key_at xs i ≤ query) :
    ∃ j, predecessor xs query = some j := by
  have member : i ∈ qualifying_indices xs query := by simp [qualifying_indices, inside, qualifies]
  have nonempty : qualifying_indices xs query ≠ [] := by intro eq; simp [eq] at member
  exact ⟨last_index (qualifying_indices xs query), by simp [predecessor, nonempty]⟩

theorem key_at_window (xs : List occurrence) (lo hi k : Nat)
    (inside : k < (window xs lo hi).length) :
    key_at (window xs lo hi) k = key_at xs (lo + k) := by
  have global : lo + k < xs.length := by simp only [window, List.length_take, List.length_drop] at inside; omega
  unfold key_at
  rw [getElem!_pos (window xs lo hi) k inside, getElem!_pos xs (lo + k) global]
  simp only [window, List.getElem_take, List.getElem_drop]

/-- A local predecessor search over the actual window returns an absolute ordinal. -/
def routed_predecessor (xs : List occurrence) (K query : Nat) : Option Nat :=
  (predecessor (window xs (route xs K query)
    (min (route xs K query + K) xs.length)) query).map (route xs K query + ·)

/-- The executable semantic local scan agrees with the global predecessor,
including no result, equality runs and the final partial window. -/
theorem routed_predecessor_correct (xs : List occurrence) (K query : Nat)
    (positive : 0 < K) (sorted : key_sorted xs) :
    routed_predecessor xs K query = predecessor xs query := by
  let lo := route xs K query
  let hi := min (lo + K) xs.length
  have lo_inside := route_inside xs K query
  have ordered : lo ≤ hi := by dsimp [hi, lo]; omega
  have last_inside : hi ≤ xs.length := Nat.min_le_right _ _
  have length : (window xs lo hi).length = hi - lo := by
    simp only [window, List.length_take, List.length_drop]; omega
  cases found : predecessor xs query with
  | none =>
    have absent := (predecessor_none xs query).mp found
    have local_none : predecessor (window xs lo hi) query = none := by
      apply (predecessor_none _ _).mpr
      intro k hk
      rw [key_at_window xs lo hi k hk]
      apply absent
      rw [length] at hk
      omega
    change (predecessor (window xs lo hi) query).map (lo + ·) = none
    simp [local_none]
  | some i =>
    have bracket := predecessor_bracket xs K query positive sorted i found
    have spec := predecessor_spec xs query i found
    have inside : i - lo < (window xs lo hi).length := by rw [length]; dsimp [lo, hi]; omega
    have relative_key : key_at (window xs lo hi) (i - lo) = key_at xs i := by
      rw [key_at_window xs lo hi (i - lo) inside]
      congr 1
      dsimp [lo]
      omega
    obtain ⟨j, local_found⟩ := predecessor_exists (window xs lo hi) query (i - lo)
      inside (by rw [relative_key]; exact spec.2.1)
    have local_spec := predecessor_spec (window xs lo hi) query j local_found
    have jl : lo + j < xs.length := by rw [length] at local_spec; omega
    have global_key : key_at xs (lo + j) ≤ query := by
      rw [← key_at_window xs lo hi j local_spec.1]
      exact local_spec.2.1
    have le := spec.2.2 (lo + j) jl global_key
    have ge := local_spec.2.2 (i - lo) inside (by rw [relative_key]; exact spec.2.1)
    have eq : lo + j = i := by dsimp [lo] at *; omega
    change (predecessor (window xs lo hi) query).map (lo + ·) = some i
    simp [local_found, eq]

/-- Fine rank is recovered from a stored boundary rank and a local prefix scan.
No arbitrary-position constant-time rank operation is assumed. -/
theorem rank_inside_route (p : occurrence → Bool) (xs : List occurrence) (K query j : Nat)
    (lower : route xs K query ≤ j)
    (upper : j < min (route xs K query + K) xs.length) :
    rank p xs j = rank p xs (route xs K query) +
      ((window xs (route xs K query) j).filter p).length ∧
      (window xs (route xs K query) j).length < K := by
  refine ⟨rank_split p xs _ j lower, ?_⟩
  simp only [window, List.length_take, List.length_drop]
  omega

/-- The flag's semantic contract witnesses an existing equal native occurrence. -/
def false_borrow_at (xs : List occurrence) (j : Nat) (inside : j < xs.length) : Prop :=
  xs[j].borrowed = true ∧ ∃ (i : Nat) (_ : i < xs.length),
    xs[i].borrowed = false ∧ xs[i].key = xs[j].key

/-- The semantic flag builder searches native occurrences without removing any borrow. -/
def false_borrow_flag (xs : List occurrence) (j : Nat) : Bool :=
  xs[j]!.borrowed && xs.any (fun a => !a.borrowed && a.key == xs[j]!.key)

theorem false_borrow_flag_correct (xs : List occurrence) (j : Nat) (inside : j < xs.length) :
    false_borrow_flag xs j = true ↔ false_borrow_at xs j inside := by
  simp only [false_borrow_flag, false_borrow_at, getElem!_pos xs j inside,
    Bool.and_eq_true, List.any_eq_true, Bool.not_eq_true, beq_iff_eq]
  constructor
  · rintro ⟨borrowed, a, member, native, same⟩
    obtain ⟨i, hi, eq⟩ := List.mem_iff_getElem.mp member
    exact ⟨borrowed, i, hi, by simpa [eq] using native, by simpa [eq] using same⟩
  · rintro ⟨borrowed, i, hi, native, same⟩
    exact ⟨borrowed, xs[i], List.getElem_mem hi, by simpa using native, same⟩

theorem false_borrow_flag_recovers (xs : List occurrence)
    (sorted : xs.Pairwise (fun a b => before a b = true))
    (unique : unique_native_keys xs) (j : Nat) (inside : j < xs.length)
    (flag : false_borrow_at xs j inside) :
    ∃ (i : Nat) (_ : i < xs.length), xs[i].borrowed = false ∧ xs[i].key = xs[j].key ∧
      0 < rank native xs j ∧ (xs.filter native)[rank native xs j - 1]? = some xs[i] := by
  obtain ⟨borrowed, i, hi, native, same⟩ := flag
  have recovery := false_borrow_recovery xs sorted unique i j hi inside native borrowed same
  exact ⟨i, hi, native, same, recovery.1, recovery.2⟩

/-- Resolve the literal target edge before sampling its exact payload. -/
def target_samples (S : catalog (List occurrence)) (id K : Nat) :
    Option (List (Nat × occurrence)) :=
  (S id).bind (fun source => source.target.bind (fun target =>
    (S target).map (fun b => samples b.payload K)))

theorem target_samples_exact (S : catalog (List occurrence)) (id target K : Nat)
    (source destination : blob (List occurrence))
    (found : S id = some source) (edge : source.target = some target)
    (target_found : S target = some destination) :
    target_samples S id K = some (samples destination.payload K) := by
  simp [target_samples, found, edge, target_found]

/-- A fork may add a different merged target without retargeting the old index. -/
theorem target_samples_extension {S T : catalog (List occurrence)}
    (extension : catalog.extends_catalog T S) (id target K : Nat)
    (source destination : blob (List occurrence))
    (found : S id = some source) (edge : source.target = some target)
    (target_found : S target = some destination) :
    target_samples T id K = target_samples S id K := by
  rw [target_samples_exact S id target K source destination found edge target_found,
    target_samples_exact T id target K source destination (extension _ _ found) edge
      (extension _ _ target_found)]

/-- Eligible reclamation retains both the pinned index and its exact sampled target. -/
theorem target_samples_reclaim {S : catalog (List occurrence)} {O : owners} {dead : List Nat}
    (safe : catalog.eligible S O dead) (id target K : Nat)
    (source destination : blob (List occurrence)) (pin : catalog.pinned S O id)
    (found : S id = some source) (edge : source.target = some target)
    (target_found : S target = some destination) :
    target_samples (catalog.reclaim S dead) id K = target_samples S id K := by
  have target_pin := catalog.target_is_pinned pin source found edge
  have source_kept : catalog.reclaim S dead id = some source := by
    rw [catalog.reclaim_preserves_pinned safe pin]; exact found
  have target_kept : catalog.reclaim S dead target = some destination := by
    rw [catalog.reclaim_preserves_pinned safe target_pin]; exact target_found
  rw [target_samples_exact S id target K source destination found edge target_found,
    target_samples_exact (catalog.reclaim S dead) id target K source destination
      source_kept edge target_kept]

/-- An abstract index stores selected occurrences and the exact target it sampled.
This is a list-level builder, not a parser for an encoded index file. -/
structure sampled_index where
  target_id : Nat
  entries : List (Nat × occurrence)
  deriving Repr, DecidableEq

def build_index (S : catalog (List occurrence)) (target K : Nat) : Option sampled_index :=
  (S target).map (fun b => ⟨target, samples b.payload K⟩)

def index_matches (S : catalog (List occurrence)) (index : sampled_index) (K : Nat) : Prop :=
  ∃ b, S index.target_id = some b ∧ index.entries = samples b.payload K

/-- The builder establishes sampled correspondence; it is not a correctness assumption. -/
theorem build_index_matches (S : catalog (List occurrence)) (target K : Nat)
    (index : sampled_index) (built : build_index S target K = some index) :
    index.target_id = target ∧ index_matches S index K := by
  unfold build_index at built
  cases found : S target with
  | none => simp [found] at built
  | some b =>
    have eq : (⟨target, samples b.payload K⟩ : sampled_index) = index := by simpa [found] using built
    subst index
    exact ⟨rfl, b, found, rfl⟩

theorem index_matches_extension {S T : catalog (List occurrence)}
    (extension : catalog.extends_catalog T S) (index : sampled_index) (K : Nat)
    (certified : index_matches S index K) : index_matches T index K := by
  obtain ⟨b, found, same⟩ := certified
  exact ⟨b, extension _ _ found, same⟩

theorem index_matches_reclaim {S : catalog (List occurrence)} {O : owners} {dead : List Nat}
    (safe : catalog.eligible S O dead) (index : sampled_index) (K : Nat)
    (pin : catalog.pinned S O index.target_id) (certified : index_matches S index K) :
    index_matches (catalog.reclaim S dead) index K := by
  obtain ⟨b, found, same⟩ := certified
  exact ⟨b, by rw [catalog.reclaim_preserves_pinned safe pin]; exact found, same⟩

/-- Route using the stored target-position/key samples, without resampling the target. -/
def index_route (index : sampled_index) (query : Nat) : Nat :=
  last_index ((index.entries.filter (fun entry => decide (entry.2.key ≤ query))).map Prod.fst)

theorem index_route_samples (index : sampled_index) (xs : List occurrence) (K query : Nat)
    (correspondence : index.entries = samples xs K) :
    index_route index query = route xs K query := by
  simp [index_route, correspondence, samples, List.filter_map, List.map_map, route, key_at,
    Function.comp_def]

/-- The builder's correspondence certificate connects stored samples to target routing. -/
theorem index_route_correct (S : catalog (List occurrence)) (index : sampled_index)
    (K query : Nat) (destination : blob (List occurrence))
    (found : S index.target_id = some destination) (certified : index_matches S index K) :
    index_route index query = route destination.payload K query := by
  obtain ⟨b, target_found, correspondence⟩ := certified
  have eq : b = destination := Option.some.inj (target_found.symm.trans found)
  subst b
  exact index_route_samples index destination.payload K query correspondence

def indexed_predecessor (index : sampled_index) (xs : List occurrence) (K query : Nat) : Option Nat :=
  let lo := index_route index query
  (predecessor (window xs lo (min (lo + K) xs.length)) query).map (lo + ·)

/-- Searching the exact target window selected from stored samples is equivalent
to a full augmented-sequence search. This is one cascade step, not a chain proof. -/
theorem indexed_predecessor_correct (S : catalog (List occurrence)) (index : sampled_index)
    (K query : Nat) (positive : 0 < K) (destination : blob (List occurrence))
    (found : S index.target_id = some destination) (certified : index_matches S index K)
    (sorted : key_sorted destination.payload) :
    indexed_predecessor index destination.payload K query = predecessor destination.payload query := by
  unfold indexed_predecessor
  rw [index_route_correct S index K query destination found certified]
  exact routed_predecessor_correct destination.payload K query positive sorted

end Everett.fractional
