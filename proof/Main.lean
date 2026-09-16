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
  IO.println s!"K=3 duplicate route: {fractional.route fractional_examples.duplicates 3 5}"
  IO.println s!"local augmented predecessor: {fractional.routed_predecessor fractional_examples.duplicates 3 5}"
  IO.println s!"false-borrow native recovery: {repr ((fractional_examples.duplicates.filter fractional.native)[fractional.rank fractional.native fractional_examples.duplicates 10 - 1]?)}"
  IO.println s!"EF equal-offset selection: {(List.range 4).map (navigation.decode (navigation.encode 1 [0, 0, 3, 3]))}"
  IO.println s!"W=15 owner boundaries for 17 records: {navigation.block_ordinals 17 15}"
  IO.println s!"group populations: {navigation.classes id [true, false, true, false, true, true, true] 3}"
  IO.println s!"last valid group rank / rejected endpoint: {navigation.boundary_rank [2, 2, 1] [0] 128 2} / {navigation.boundary_rank [2, 2, 1] [0] 128 3}"
  IO.println s!"derived group total: {navigation.total [2, 2, 1] [0] 128}"
  IO.println s!"carried sample route: {carried_route.transfer_at_group carried_route.example_edge 1 6 2}"
  IO.println s!"carried child predecessor: {carried_route.descend_at_group carried_route.example_edge 1 6 2}"
  IO.println s!"two carried handoffs: {carried_chain.search 2 carried_chain.example_chain}"
  IO.println s!"retained-floor extra literals: {retained_floor.extra 5 2}"
  IO.println s!"adjacent canceled-owner fragments: {(retained_floor.adjacent_fragments.map retained_floor.fragment.rescued).flatten}"
  IO.println "All theorem declarations and executable examples were checked by lake build."
