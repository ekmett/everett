/-
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-/
import Everett.Category
import Everett.Snapshots

namespace Everett

universe u v

/-- A representation labels an actual typed history with its exact physical IDs. -/
def history_blob {object : Type u} {C : category.{u,v} object} {x y}
    (native index : Nat) (target : Option Nat) (h : history C x y) : blob (C.hom x y) :=
  ⟨native, index, target, history.eval h⟩

/-- Here the rewrite premise of independent adoption is discharged by the
proved adjacent-merge law. Neither equal signatures nor equal endpoints suffice. -/
theorem adopt_adjacent_merge {object : Type u} {C : category.{u,v} object}
    {a b c d e} (prior : history C a b) (f : C.hom b c) (g : C.hom c d)
    (suffix : history C d e) (S : catalog (C.hom a e)) (O : owners)
    (owner old_root new_root old_native old_index new_native new_index : Nat)
    (old_target new_target : Option Nat) (held : O owner = some old_root)
    (old_found : S old_root = some (history_blob old_native old_index old_target
      (history.append prior (.cons f (.cons g suffix)))))
    (new_found : S new_root = some (history_blob new_native new_index new_target
      (history.append prior (.cons (C.comp f g) suffix)))) :
    catalog.read S (owners.adopt O owner new_root) owner = catalog.read S O owner := by
  apply owners.independent_adoption S O owner old_root new_root _ _ held old_found new_found
  exact (history.merge_in_context prior f g suffix).symm

end Everett
