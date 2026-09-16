/-
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-/
import Std

namespace Everett.framing

/-! Arithmetic behind the absolute-prefix admission bound. All positions use
one policy's units. Controls, literals and values occupy nonnegative extents;
retained prefix units were already present in the predecessor. This is not a
proof that the C++ parser decodes these fields correctly. -/

structure frame where
  retained : Nat
  literal : Nat
  values : Nat
  controls : Nat

def frame.key_length (f : frame) : Nat := f.retained + f.literal
def frame.encoded_units (f : frame) : Nat := f.controls + f.literal + f.values

/-- A checked absolute prefix cannot produce a key longer than the admitted
physical frame end. Relative frames inherit the same premise from their
predecessor. Values need not have fixed size. -/
theorem key_within_frame (f : frame) (start : Nat) (retained : f.retained ≤ start) :
    f.key_length ≤ start + f.encoded_units := by
  simp only [frame.key_length, frame.encoded_units]
  omega

def valid_chain : Nat → List frame → Prop
  | _, [] => True
  | previous, f :: rest => f.retained ≤ previous ∧ valid_chain f.key_length rest

def encoded_units : List frame → Nat
  | [] => 0
  | f :: rest => f.encoded_units + encoded_units rest

def final_length : Nat → List frame → Nat
  | previous, [] => previous
  | _, f :: rest => final_length f.key_length rest

/-- Sequential framing preserves key length ≤ physical position. Starting at
zero proves every real retained prefix fits before its physical frame. -/
theorem chain_length_bound (frames : List frame) (previous start : Nat)
    (initial : previous ≤ start) (valid : valid_chain previous frames) :
    final_length previous frames ≤ start + encoded_units frames := by
  induction frames generalizing previous start with
  | nil => simpa [final_length, encoded_units] using initial
  | cons f rest ih =>
    obtain ⟨retained, tail⟩ := valid
    have here := key_within_frame f start (Nat.le_trans retained initial)
    have later := ih f.key_length (start + f.encoded_units) here tail
    simpa [final_length, encoded_units, Nat.add_assoc] using later

/-- Once the enclosing extent has an admitted bit size, a bounded frame's key
conversion also fits. Multiplication uses mathematical naturals here. -/
theorem key_bits_bound (f : frame) (start extent units limit : Nat)
    (retained : f.retained ≤ start)
    (payload : start + f.encoded_units ≤ extent)
    (admitted : extent * units ≤ limit) :
    f.key_length * units ≤ limit := by
  exact Nat.le_trans (Nat.mul_le_mul_right units
    (Nat.le_trans (key_within_frame f start retained) payload)) admitted

end Everett.framing
