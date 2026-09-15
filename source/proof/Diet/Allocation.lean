/-
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-/
import Diet.Snapshots

namespace Diet

universe u

/-- Issued IDs are below a monotone watermark, even after their entries disappear. -/
structure storage (meaning : Type u) where
  objects : catalog meaning
  next_id : Nat
  unissued : ∀ id, next_id ≤ id → objects id = none

namespace storage

variable {meaning : Type u}

def empty : storage meaning := ⟨fun _ => none, 0, fun _ _ => rfl⟩

def allocate (S : storage meaning) (b : blob meaning) : storage meaning where
  objects := catalog.install S.objects S.next_id b
  next_id := S.next_id + 1
  unissued := by
    intro id h
    have different : id ≠ S.next_id := by omega
    simp [catalog.install, different, S.unissued id (by omega)]

def reclaim (S : storage meaning) (dead : List Nat) : storage meaning where
  objects := catalog.reclaim S.objects dead
  next_id := S.next_id
  unissued := by
    intro id h
    simp [catalog.reclaim, S.unissued id h]

theorem allocate_extends (S : storage meaning) (b : blob meaning) :
    catalog.extends_catalog (allocate S b).objects S.objects :=
  catalog.install_fresh_extends S.objects S.next_id b (S.unissued _ (Nat.le_refl _))

/-- This relation records allocation history. GC eligibility remains a separate
pin obligation; monotonicity holds regardless of which entries are removed. -/
inductive evolves : storage meaning → storage meaning → Prop
  | refl (S) : evolves S S
  | alloc {S T} (prior : evolves S T) (b : blob meaning) : evolves S (allocate T b)
  | gc {S T} (prior : evolves S T) (dead : List Nat) : evolves S (reclaim T dead)

theorem watermark_monotone {S T : storage meaning} (h : evolves S T) :
    S.next_id ≤ T.next_id := by
  induction h with
  | refl => exact Nat.le_refl _
  | alloc prior b ih => simp only [allocate]; omega
  | gc prior dead ih => exact ih

/-- The next allocation cannot reuse any identifier issued before S. -/
theorem no_identifier_reuse {S T : storage meaning} (h : evolves S T)
    {id : Nat} (issued : id < S.next_id) : id ≠ T.next_id := by
  have monotone := watermark_monotone h
  omega

end storage
end Diet
