/-
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-/
import Everett.Fractional

namespace Everett.navigation

/-! Executable list-level succinct navigation. The high vector below contains
actual unary bits, not a select oracle. Packed words, SIMD reductions, sparse
select accelerators, finite-width arithmetic and parser admission remain separate
refinement obligations. -/

/-- Zero-based selection of a set bit, returning its physical bit position. -/
def select_one : List Bool → Nat → Option Nat
  | [], _ => none
  | false :: rest, ordinal => (select_one rest ordinal).map (· + 1)
  | true :: _, 0 => some 0
  | true :: rest, ordinal + 1 => (select_one rest ordinal).map (· + 1)

theorem select_zero_prefix (zeros : Nat) (bits : List Bool) (ordinal : Nat) :
    select_one (List.replicate zeros false ++ bits) ordinal =
      (select_one bits ordinal).map (zeros + ·) := by
  induction zeros with
  | zero => simp [select_one]
  | succ zeros ih =>
    simp only [List.replicate_succ, List.cons_append, select_one, ih, Option.map_map]
    congr 1
    funext value
    dsimp [Function.comp_def]
    omega

/-- Unary quotient gaps. Each quotient contributes one set bit; equal quotients
contribute adjacent set bits. The initial previous quotient is zero. -/
def unary_high : Nat → List Nat → List Bool
  | _, [] => []
  | previous, quotient :: rest =>
    List.replicate (quotient - previous) false ++ true :: unary_high quotient rest

/-- Every input quotient emits exactly one set bit, even when quotients repeat.
This counting fact does not require ordering. -/
theorem unary_high_population (quotients : List Nat) (previous : Nat) :
    ((unary_high previous quotients).filter id).length = quotients.length := by
  induction quotients generalizing previous with
  | nil => rfl
  | cons q rest ih => simp [unary_high, List.filter_append, ih]

/-- Logical high-vector length, before any machine-word padding. The final
quotient determines the zero-bit budget; each record contributes one set bit. -/
theorem unary_high_length (quotients : List Nat) (previous last : Nat)
    (sorted : quotients.Pairwise (· ≤ ·))
    (lower : ∀ q ∈ quotients, previous ≤ q) (last_value : quotients.getLast? = some last) :
    (unary_high previous quotients).length = last - previous + quotients.length := by
  induction quotients generalizing previous with
  | nil => simp at last_value
  | cons q rest ih =>
    obtain ⟨head_le, tail_sorted⟩ := List.pairwise_cons.mp sorted
    have previous_le := lower q (by simp)
    cases rest with
    | nil =>
      have same : q = last := by simpa using last_value
      subst last
      simp [unary_high]
    | cons r tail =>
      have tail_last : (r :: tail).getLast? = some last := by simpa using last_value
      have q_le : q ≤ last := head_le last (List.mem_of_getLast? tail_last)
      have tail_length := ih q tail_sorted head_le tail_last
      rw [unary_high]
      simp only [List.length_append, List.length_replicate, List.length_cons]
      rw [tail_length]
      simp only [List.length_cons]
      omega

/-- Selecting a constructed unary vector recovers quotient + ordinal. No
assumed select implementation appears in the statement. -/
theorem unary_high_select (quotients : List Nat) (previous ordinal : Nat)
    (sorted : quotients.Pairwise (· ≤ ·))
    (lower : ∀ q ∈ quotients, previous ≤ q) (inside : ordinal < quotients.length) :
    select_one (unary_high previous quotients) ordinal =
      some (quotients[ordinal] - previous + ordinal) := by
  induction quotients generalizing previous ordinal with
  | nil => simp at inside
  | cons q rest ih =>
    obtain ⟨head_le, tail_sorted⟩ := List.pairwise_cons.mp sorted
    have previous_le : previous ≤ q := lower q (by simp)
    cases ordinal with
    | zero => simp [unary_high, select_zero_prefix, select_one]
    | succ ordinal =>
      have tail_inside : ordinal < rest.length := by simpa using inside
      have q_le : q ≤ rest[ordinal] := head_le _ (List.getElem_mem tail_inside)
      rw [unary_high, select_zero_prefix, select_one,
        ih q ordinal tail_sorted head_le tail_inside]
      simp only [Option.map_some', List.getElem_cons_succ]
      congr 1
      omega

theorem quotient_mono (a b base : Nat) (order : a ≤ b) : a / base ≤ b / base := by
  cases base with
  | zero => simp
  | succ base =>
    apply (Nat.le_div_iff_mul_le (Nat.succ_pos base)).mpr
    exact Nat.le_trans (Nat.div_mul_le_self a (base + 1)) order

theorem unary_high_select_outside (quotients : List Nat) (previous ordinal : Nat)
    (outside : quotients.length ≤ ordinal) :
    select_one (unary_high previous quotients) ordinal = none := by
  induction quotients generalizing previous ordinal with
  | nil => rfl
  | cons q rest ih =>
    cases ordinal with
    | zero => simp at outside
    | succ ordinal =>
      rw [unary_high, select_zero_prefix, select_one, ih q ordinal (by simpa using outside)]
      rfl

/-- The embedded high positions are strictly increasing, including equal input
values. Adding the ordinal is what makes unary select unambiguous. -/
theorem high_positions_strict (values : List Nat) (base i j : Nat)
    (sorted : values.Pairwise (· ≤ ·)) (hi : i < values.length)
    (hj : j < values.length) (order : i < j) :
    values[i] / base + i < values[j] / base + j := by
  have le := List.pairwise_iff_getElem.mp sorted i j hi hj order
  have divided := quotient_mono _ _ base le
  omega

structure ef where
  base : Nat
  low : List Nat
  high : List Bool
  deriving Repr

/-- `base = 2^width` gives Elias–Fano; correctness is independent of the chosen
width. Low fields are natural remainders here, not packed machine words. -/
def encode (base : Nat) (values : List Nat) : ef :=
  ⟨base, values.map (· % base), unary_high 0 (values.map (· / base))⟩

def decode (encoded : ef) (ordinal : Nat) : Option Nat := do
  let position ← select_one encoded.high ordinal
  let low ← encoded.low[ordinal]?
  return (position - ordinal) * encoded.base + low

theorem quotient_sorted (values : List Nat) (base : Nat)
    (sorted : values.Pairwise (· ≤ ·)) :
    (values.map (· / base)).Pairwise (· ≤ ·) := by
  apply List.pairwise_map.mpr
  exact sorted.imp (by intro a b le; exact quotient_mono _ _ base le)

/-- Each low field fits the chosen base. With base 2^w, this is precisely
its w-bit range; base 1 gives the legitimate zero-width field containing zero. -/
theorem low_field_bound (values : List Nat) (base ordinal : Nat)
    (positive : 0 < base) (inside : ordinal < (encode base values).low.length) :
    (encode base values).low[ordinal] < base := by
  have valid : ordinal < values.length := by simpa [encode] using inside
  simpa [encode] using Nat.mod_lt values[ordinal] positive

theorem low_bit_field_bound (values : List Nat) (width ordinal : Nat)
    (inside : ordinal < (encode (2 ^ width) values).low.length) :
    (encode (2 ^ width) values).low[ordinal] < 2 ^ width :=
  low_field_bound values (2 ^ width) ordinal (Nat.two_pow_pos width) inside

theorem encoded_high_population (values : List Nat) (base : Nat) :
    ((encode base values).high.filter id).length = values.length := by
  simpa [encode] using unary_high_population (values.map (· / base)) 0

/-- The C++ EF envelope uses the final value as its universe. This is the exact
logical high-vector length; low/high word rounding and sparse samples are separate. -/
theorem encoded_high_length (values : List Nat) (base extent : Nat)
    (sorted : values.Pairwise (· ≤ ·)) (last_value : values.getLast? = some extent) :
    (encode base values).high.length = extent / base + values.length := by
  have last_quotient : (values.map (· / base)).getLast? = some (extent / base) := by
    simp [last_value]
  simpa [encode] using unary_high_length (values.map (· / base)) 0 (extent / base)
    (quotient_sorted values base sorted) (by intro q member; omega) last_quotient

theorem encoded_high_empty (base : Nat) : (encode base []).high.length = 0 := rfl

/-- Every valid ordinal decodes to the original offset, through executable
selection of the constructed high bits. Repeated offsets and width zero work. -/
theorem decode_encode (values : List Nat) (base ordinal : Nat)
    (_positive : 0 < base) (sorted : values.Pairwise (· ≤ ·))
    (inside : ordinal < values.length) :
    decode (encode base values) ordinal = some values[ordinal] := by
  have selected := unary_high_select (values.map (· / base)) 0 ordinal
    (quotient_sorted values base sorted) (by intro q member; omega) (by simpa using inside)
  simp only [List.getElem_map, Nat.sub_zero] at selected
  simp only [decode, encode, selected, List.getElem?_map, List.getElem?_eq_getElem inside,
    Option.map_some', Option.some_bind, Nat.add_sub_cancel]
  change some ((values[ordinal] / base + ordinal - ordinal) * base + values[ordinal] % base) =
    some values[ordinal]
  rw [Nat.add_sub_cancel]
  congr 1
  have division := Nat.mod_add_div values[ordinal] base
  rw [Nat.mul_comm base (values[ordinal] / base)] at division
  omega

theorem decode_encode_outside (values : List Nat) (base ordinal : Nat)
    (outside : values.length ≤ ordinal) : decode (encode base values) ordinal = none := by
  have selected := unary_high_select_outside (values.map (· / base)) 0 ordinal (by simpa using outside)
  simp [decode, encode, selected]

/-- An empty EF has no implicit sentinel. Owners append EOF as a real value. -/
theorem decode_empty (base ordinal : Nat) : decode (encode base []) ordinal = none := by
  simp [decode, encode, unary_high, select_one]

/-- An owner stores a record ordinal alongside its physical offset. Both are
in the owner's declared units; bit conversion is outside this model. -/
structure boundary where
  ordinal : Nat
  physical : Nat
  deriving Repr, DecidableEq

def residual (width : Nat) (b : boundary) : Nat := b.physical - b.ordinal * width

def stride_fits (width : Nat) (boundaries : List boundary) : Prop :=
  ∀ b ∈ boundaries, b.ordinal * width ≤ b.physical

/-- Physical growth covers the common fixed-width values between boundaries. -/
def stride_ordered (width : Nat) (boundaries : List boundary) : Prop :=
  boundaries.Pairwise (fun a b => a.physical + b.ordinal * width ≤ b.physical + a.ordinal * width)

theorem residuals_sorted (width : Nat) (boundaries : List boundary)
    (fits : stride_fits width boundaries) (ordered : stride_ordered width boundaries) :
    (boundaries.map (residual width)).Pairwise (· ≤ ·) := by
  apply List.pairwise_map.mpr
  apply List.pairwise_iff_getElem.mpr
  intro i j hi hj ij
  have le := List.pairwise_iff_getElem.mp ordered i j hi hj ij
  have left := fits _ (List.getElem_mem hi)
  have right := fits _ (List.getElem_mem hj)
  dsimp [residual]
  omega

def physical_select (encoded : ef) (width ordinal index : Nat) : Option Nat :=
  (decode encoded index).map (· + ordinal * width)

/-- Removing common value stride before EF encoding and adding it after select
recovers the exact physical boundary. The final EOF entry uses the actual record
count, not a rounded block count. -/
theorem physical_select_correct (boundaries : List boundary) (width base index : Nat)
    (positive : 0 < base) (fits : stride_fits width boundaries)
    (ordered : stride_ordered width boundaries) (inside : index < boundaries.length) :
    physical_select (encode base (boundaries.map (residual width))) width
      boundaries[index].ordinal index = some boundaries[index].physical := by
  rw [physical_select, decode_encode _ base index positive
    (residuals_sorted width boundaries fits ordered) (by simpa using inside)]
  simp only [List.getElem_map, Option.map_some', residual]
  have valid := fits _ (List.getElem_mem inside)
  congr 1
  omega

/-- Number of real groups. There is no endpoint population or stored total. -/
def group_count (count spacing : Nat) : Nat :=
  count / spacing + if count % spacing = 0 then 0 else 1

theorem groups_cover (count spacing : Nat) (positive : 0 < spacing) :
    count ≤ group_count count spacing * spacing := by
  have division := Nat.mod_add_div count spacing
  have remainder := Nat.mod_lt count positive
  unfold group_count
  split <;> simp only [Nat.add_zero, Nat.add_mul, Nat.one_mul] <;>
    rw [Nat.mul_comm (count / spacing) spacing] <;> omega

theorem group_start_inside (count spacing group : Nat) (positive : 0 < spacing)
    (inside : group < group_count count spacing) : group * spacing < count := by
  have division := Nat.mod_add_div count spacing
  rw [Nat.mul_comm spacing (count / spacing)] at division
  by_cases exact_group : count % spacing = 0
  · simp only [group_count, exact_group, ↓reduceIte, Nat.add_zero] at inside
    have product := Nat.mul_lt_mul_of_pos_right inside positive
    omega
  · simp only [group_count, exact_group, ↓reduceIte] at inside
    have le : group ≤ count / spacing := by omega
    have product := Nat.mul_le_mul_right spacing le
    omega

/-- Real block boundaries precede EOF; the final boundary uses the actual count.
The list contains one explicit EOF even when the owner is empty. -/
def block_ordinals (count spacing : Nat) : List Nat :=
  (List.range (group_count count spacing)).map (· * spacing) ++ [count]

theorem block_ordinals_length (count spacing : Nat) :
    (block_ordinals count spacing).length = group_count count spacing + 1 := by
  simp [block_ordinals]

theorem block_ordinals_eof (count spacing : Nat) :
    (block_ordinals count spacing)[group_count count spacing]? = some count := by
  simp [block_ordinals, List.getElem?_append]

theorem block_ordinals_select (count spacing index : Nat) (positive : 0 < spacing)
    (inside : index ≤ group_count count spacing) :
    (block_ordinals count spacing)[index]? = some (min (index * spacing) count) := by
  by_cases interior : index < group_count count spacing
  · have before := group_start_inside count spacing index positive interior
    simp [block_ordinals, List.getElem?_append, interior, Nat.min_eq_left (Nat.le_of_lt before)]
  · have eof : index = group_count count spacing := by omega
    subst index
    rw [block_ordinals_eof]
    rw [Nat.min_eq_right (groups_cover count spacing positive)]

/-- An owner builds physical boundaries from its generated record ordinals.
`extent` is used at EOF; earlier offsets come from the supplied framing model. -/
def owner_boundaries (count spacing extent : Nat) (offset : Nat → Nat) : List boundary :=
  ((List.range (group_count count spacing)).map
    (fun group => ⟨group * spacing, offset (group * spacing)⟩)) ++ [⟨count, extent⟩]

theorem owner_eof (count spacing extent width base : Nat) (offset : Nat → Nat)
    (positive : 0 < base)
    (fits : stride_fits width (owner_boundaries count spacing extent offset))
    (ordered : stride_ordered width (owner_boundaries count spacing extent offset)) :
    physical_select
      (encode base ((owner_boundaries count spacing extent offset).map (residual width)))
      width count (group_count count spacing) = some extent := by
  have inside : group_count count spacing <
      (owner_boundaries count spacing extent offset).length := by simp [owner_boundaries]
  have eof : (owner_boundaries count spacing extent offset)[group_count count spacing] =
      ⟨count, extent⟩ := by simp [owner_boundaries, List.getElem_append_right]
  have recovered := physical_select_correct (owner_boundaries count spacing extent offset)
    width base (group_count count spacing) positive fits ordered inside
  simpa only [eof] using recovered

/-- The owner's complete in-range convention agrees with min(block*W,N),
including a partial block's EOF. Selecting EOF never expands the EF API. -/
theorem owner_select (count spacing extent width base index : Nat) (offset : Nat → Nat)
    (positive_base : 0 < base) (positive_spacing : 0 < spacing)
    (fits : stride_fits width (owner_boundaries count spacing extent offset))
    (ordered : stride_ordered width (owner_boundaries count spacing extent offset))
    (inside : index ≤ group_count count spacing) :
    physical_select
      (encode base ((owner_boundaries count spacing extent offset).map (residual width)))
      width (min (index * spacing) count) index =
        some (if index = group_count count spacing then extent else offset (index * spacing)) := by
  by_cases interior : index < group_count count spacing
  · have before := group_start_inside count spacing index positive_spacing interior
    have valid : index < (owner_boundaries count spacing extent offset).length := by
      simp only [owner_boundaries, List.length_append, List.length_map, List.length_range,
        List.length_cons, List.length_nil]; omega
    have entry : (owner_boundaries count spacing extent offset)[index] =
        ⟨index * spacing, offset (index * spacing)⟩ := by
      simp [owner_boundaries, List.getElem_append_left, interior]
    have recovered := physical_select_correct (owner_boundaries count spacing extent offset)
      width base index positive_base fits ordered valid
    simpa [entry, Nat.min_eq_left (Nat.le_of_lt before), Nat.ne_of_lt interior] using recovered
  · have eof : index = group_count count spacing := by omega
    subst index
    rw [Nat.min_eq_right (groups_cover count spacing positive_spacing)]
    simpa using owner_eof count spacing extent width base offset positive_base fits ordered

theorem sum_append (left right : List Nat) : (left ++ right).sum = left.sum + right.sum := by
  induction left with
  | nil => simp
  | cons head tail ih => simp [ih, Nat.add_assoc]

/-- Each class is a population of an actual K-entry window, including a short tail. -/
def population (p : α → Bool) (xs : List α) (K group : Nat) : Nat :=
  ((fractional.window xs (group * K) ((group + 1) * K)).filter p).length

/-- Populations fit the actual tail, not merely the full group capacity. -/
theorem population_window_bound (p : α → Bool) (xs : List α) (K group : Nat) :
    population p xs K group ≤ min K (xs.length - group * K) := by
  have bound := List.length_filter_le p (fractional.window xs (group * K) ((group + 1) * K))
  simpa [population, fractional.window, Nat.add_mul, Nat.add_sub_cancel_left] using bound

theorem population_le (p : α → Bool) (xs : List α) (K group : Nat) :
    population p xs K group ≤ K :=
  Nat.le_trans (population_window_bound p xs K group) (Nat.min_le_left _ _)

/-- K=2^bits−1 uses exactly the available class range, including population K. -/
theorem population_bits_fit (p : α → Bool) (xs : List α) (bits group : Nat) :
    population p xs (2 ^ bits - 1) group < 2 ^ bits := by
  have bound := population_le p xs (2 ^ bits - 1) group
  have positive := Nat.two_pow_pos bits
  omega

def classes (p : α → Bool) (xs : List α) (K : Nat) : List Nat :=
  (List.range (group_count xs.length K)).map (population p xs K)

theorem class_field_bound (p : α → Bool) (xs : List α) (bits group : Nat)
    (inside : group < (classes p xs (2 ^ bits - 1)).length) :
    (classes p xs (2 ^ bits - 1))[group] < 2 ^ bits := by
  simp only [classes, List.getElem_map, List.getElem_range]
  exact population_bits_fit p xs bits group

theorem population_prefix (p : α → Bool) (xs : List α) (K groups : Nat) :
    ((List.range groups).map (population p xs K)).sum = fractional.rank p xs (groups * K) := by
  induction groups with
  | zero => simp [fractional.rank]
  | succ groups ih =>
    rw [List.range_succ, List.map_append, sum_append]
    simp only [List.map_cons, List.map_nil, List.sum_cons, List.sum_nil, Nat.add_zero, ih]
    exact (fractional.rank_split p xs (groups * K) ((groups + 1) * K)
      (by simp [Nat.add_mul])).symm

theorem classes_prefix (p : α → Bool) (xs : List α) (K group : Nat)
    (inside : group ≤ (classes p xs K).length) :
    ((classes p xs K).take group).sum = fractional.rank p xs (group * K) := by
  have bound : group ≤ group_count xs.length K := by simpa [classes] using inside
  simp only [classes, ← List.map_take, List.take_range, Nat.min_eq_left bound]
  exact population_prefix p xs K group

/-- Checkpoint every C classes, only for an existing class. The last complete
checkpoint is not followed by an extra endpoint word. -/
def checkpoints (populations : List Nat) (C : Nat) : List Nat :=
  (List.range (group_count populations.length C)).map
    (fun checkpoint => (populations.take (checkpoint * C)).sum)

/-- Reject one-past-end group queries even if a mathematical prefix is defined. -/
def boundary_rank (populations directory : List Nat) (C group : Nat) : Option Nat :=
  if group < populations.length then
    directory[group / C]?.map (fun before =>
      before + (fractional.window populations (group / C * C) group).sum)
  else none

theorem boundary_rank_correct (populations : List Nat) (C group : Nat)
    (positive : 0 < C) (inside : group < populations.length) :
    boundary_rank populations (checkpoints populations C) C group =
      some ((populations.take group).sum) := by
  have coverage := groups_cover populations.length C positive
  have checkpoint_inside : group / C < group_count populations.length C :=
    (Nat.div_lt_iff_lt_mul positive).mpr (by omega)
  have lower : group / C * C ≤ group := Nat.div_mul_le_self group C
  have split := congrArg List.sum (fractional.take_split populations (group / C * C) group lower)
  simp only [sum_append] at split
  simp [boundary_rank, inside, checkpoints, List.getElem?_eq_getElem,
    checkpoint_inside, split]

/-- Stored group rank agrees with the origin rank used by fractional projection. -/
theorem grouped_rank_correct (p : α → Bool) (xs : List α) (K C group : Nat)
    (positive : 0 < C) (inside : group < (classes p xs K).length) :
    boundary_rank (classes p xs K) (checkpoints (classes p xs K) C) C group =
      some (fractional.rank p xs (group * K)) := by
  rw [boundary_rank_correct _ C group positive inside, classes_prefix p xs K group (by omega)]

theorem boundary_rank_rejects (populations directory : List Nat) (C group : Nat)
    (outside : populations.length ≤ group) :
    boundary_rank populations directory C group = none := by
  simp [boundary_rank, show ¬ group < populations.length by omega]

/-- The directory scan reads fewer than C classes, regardless of the tail. -/
theorem directory_scan_budget (populations : List Nat) (C group : Nat)
    (positive : 0 < C) :
    (fractional.window populations (group / C * C) group).length < C := by
  have division := Nat.mod_add_div group C
  have remainder := Nat.mod_lt group positive
  rw [Nat.mul_comm C (group / C)] at division
  simp only [fractional.window, List.length_take, List.length_drop]
  omega

/-- Fine rank combines a stored group boundary with a local scan, rather than
pretending that a group-population directory stores arbitrary-position rank. -/
theorem fine_rank (p : α → Bool) (xs : List α) (K C group cut : Nat)
    (positive : 0 < C) (inside : group < (classes p xs K).length)
    (lower : group * K ≤ cut) (upper : cut < (group + 1) * K) :
    (boundary_rank (classes p xs K) (checkpoints (classes p xs K) C) C group).map
      (· + ((fractional.window xs (group * K) cut).filter p).length) =
      some (fractional.rank p xs cut) ∧
    (fractional.window xs (group * K) cut).length < K := by
  rw [grouped_rank_correct p xs K C group positive inside]
  constructor
  · simp only [Option.map_some', fractional.rank_split p xs (group * K) cut lower]
  · simp only [fractional.window, List.length_take, List.length_drop]
    simp only [Nat.add_mul, Nat.one_mul] at upper
    omega

/-- Recover total from the last valid boundary and its population. The empty
case returns zero and stores no class or checkpoint. -/
def total (populations directory : List Nat) (C : Nat) : Option Nat :=
  if populations.isEmpty then some 0 else
    (boundary_rank populations directory C (populations.length - 1)).map
      (· + populations[populations.length - 1]!)

theorem total_correct (populations : List Nat) (C : Nat) (positive : 0 < C) :
    total populations (checkpoints populations C) C = some populations.sum := by
  cases populations with
  | nil => rfl
  | cons head tail =>
    have inside : (head :: tail).length - 1 < (head :: tail).length := by simp
    simp only [total, List.isEmpty_cons, Bool.false_eq_true, ↓reduceIte]
    rw [boundary_rank_correct _ C _ positive inside]
    simp only [Option.map_some', getElem!_pos (head :: tail) ((head :: tail).length - 1) inside]
    have split := congrArg List.sum (List.take_append_getElem inside)
    simp only [sum_append, List.sum_cons, List.sum_nil, Nat.add_zero] at split
    simpa using congrArg some split

/-- The derived total is exactly the full origin count, with no endpoint rank API. -/
theorem grouped_total_correct (p : α → Bool) (xs : List α) (K C : Nat)
    (groups_positive : 0 < K) (checkpoints_positive : 0 < C) :
    total (classes p xs K) (checkpoints (classes p xs K) C) C = some (xs.filter p).length := by
  rw [total_correct _ C checkpoints_positive]
  have all := population_prefix p xs K (group_count xs.length K)
  have coverage := groups_cover xs.length K groups_positive
  simpa [classes, fractional.rank, List.take_of_length_le coverage] using congrArg some all

set_option maxRecDepth 4096

-- A second checkpoint appears only when its first real class exists.
example : checkpoints (List.replicate 129 1) 128 = [0, 128] := by decide
example : boundary_rank (List.replicate 129 1) [0, 128] 128 128 = some 128 := by decide
example : boundary_rank (List.replicate 129 1) [0, 128] 128 129 = none := by decide
example : total (List.replicate 129 1) [0, 128] 128 = some 129 := by decide
example : checkpoints (List.replicate 128 1) 128 = [0] := by decide
example : classes id [true, false, true, false, true, true, true] 3 = [2, 2, 1] := by decide
example : total [2, 2, 1] [0] 128 = some 5 := by decide
example : boundary_rank ([] : List Nat) [] 128 0 = none := rfl
example : total [] [] 128 = some 0 := rfl
example : block_ordinals 0 15 = [0] := by decide
example : block_ordinals 17 15 = [0, 15, 17] := by decide
example : block_ordinals 30 15 = [0, 15, 30] := by decide
example : owner_boundaries 0 15 0 id = [⟨0, 0⟩] := by decide
example : owner_boundaries 17 15 76 (fun n => n * 4 + n / 3) =
    [⟨0, 0⟩, ⟨15, 65⟩, ⟨17, 76⟩] := by decide

-- Full populations use the top code; a partial tail still obeys its own length.
example : population id (List.replicate 16 true) 15 0 = 15 := by decide
example : population id (List.replicate 16 true) 15 1 = 1 := by decide
example : (encode 1 [0, 0, 3, 3]).low = [0, 0, 0, 0] := by decide
example : ((encode 1 [0, 0, 3, 3]).high.filter id).length = 4 := by decide
example : (encode 1 [0, 0, 3, 3]).high.length = 3 + 4 := by decide
example : (encode 8 [0, 7, 8, 8, 65]).high.length = 65 / 8 + 5 := by decide

-- Actual unary bits distinguish equal values instead of collapsing them.
example : (encode 1 [0, 0, 3, 3]).high =
    [true, true, false, false, false, true, true] := by decide
example : (List.range 4).map (decode (encode 1 [0, 0, 3, 3])) =
    [some 0, some 0, some 3, some 3] := by decide
example : (List.range 5).map (decode (encode 8 [0, 7, 8, 8, 65])) =
    [some 0, some 7, some 8, some 8, some 65] := by decide
example : decode (encode 1 []) 0 = none := rfl
-- W=15, 17 records: EOF ordinal is 17, not the rounded block ordinal 30.
example : physical_select (encode 4 ([⟨0, 0⟩, ⟨15, 65⟩, ⟨17, 76⟩].map
    (residual 3))) 3 17 2 = some 76 := by decide
-- An empty owner may explicitly encode its one real EOF offset.
example : decode (encode 1 [0]) 0 = some 0 := rfl

end Everett.navigation
