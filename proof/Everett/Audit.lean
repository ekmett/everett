/-
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-/
import Everett.Examples
import Everett.Allocation
import Everett.FractionalExamples
import Everett.Prefix
import Everett.Frontier
import Everett.Transfer
import Everett.NativeMerge
import Lean

open Lean Elab Command
/- Audit every kernel-safe declaration in this namespace, including dependencies.
Compiler-generated unsafe execution artifacts are not logical declarations.
Only Lean's standard logical foundations are allowed; no theorem placeholders
or additional axioms may hide behind imported helper declarations. -/
run_elab do
  let env ← getEnv
  let mut count := 0
  let mut found : Array Name := #[]
  for (name, info) in env.constants.toList do
    if Name.isPrefixOf `Everett name && !info.isUnsafe then
      count := count + 1
      for axiom_name in (← collectAxioms name) do
        unless #[`propext, `Classical.choice, `Quot.sound].contains axiom_name do
          throwError "unexpected axiom {axiom_name} in {name}"
        unless found.contains axiom_name do
          found := found.push axiom_name
  logInfo m!"Everett audit: {count} declarations; logical axioms: {found.toList}"
