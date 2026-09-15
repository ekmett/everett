/-
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-/
import Everett

open Everett Everett.examples

def main : IO Unit := do
  IO.println s!"disjoint updates: {observation (mutation.apply_two initial increment enable)}"
  IO.println s!"same-key chronology: {observation (mutation.apply_two initial increment next)}"
  IO.println s!"reversed same-key chronology: {observation (mutation.apply_two initial next increment)}"
  IO.println s!"snapshot after independent adoption: {catalog.read new_catalog (owners.adopt held_roots 0 3) 1}"
  IO.println s!"old exact pair: {repr (new_catalog 1)}"
  IO.println s!"new exact pair: {repr (new_catalog 3)}"
  IO.println "All theorem declarations and executable examples were checked by lake build."
