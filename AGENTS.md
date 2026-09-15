# Everett

Read README.md, docs/design.md and docs/implementation.md before changing the
architecture. Keep the design and implementation status distinct and current.

## Coding style

- C++20, with no modules or header units.
- Follow the lowercase, struct-first style of https://github.com/ekmett/bad
  (reference revision 978b8056ffafc992fd1b7300ccf7bd1219cd3a20), without importing
  its ISA assumptions or macro-dispatch machinery.
- Use lowercase/snake_case names for concrete types, functions, namespaces,
  variables and files. Short uppercase template parameters are welcome.
- Use struct with explicit private/protected sections, two-space indentation,
  .h headers with #pragma once, .cc implementations, and T const & spelling.
- Include public headers through everett/foo.h. Keep helpers with their sole
  consumer and extract them only for actual sharing.
- Prefer templates, CRTP and associated type families for policy specialization.
  Avoid virtual dispatch, PImpl and type-erased backend payloads.
- Use standard attributes and facilities where practical. Individual named
  modifier macros are permitted; do not add variadic modifier-dispatch macros.
- Retain author notices. Place SPDX-FileCopyrightText and
  SPDX-License-Identifier in a license comment before code (after a shebang,
  if present). The identifier is BSD-2-Clause OR Apache-2.0, at the recipient's
  choice; both complete texts are in LICENSES/ and the choice is stated in LICENSE.
  Do not change license terms without an explicit instruction.
- File documentation stays in the existing footer form. The optional Doxygen
  build uses the `ein` license aliases and checks declaration ownership; see
  docs/doxygen.md before changing those commands or their placement.

## Work and verification

The main checkout belongs to the integration owner. Workers use isolated Git
worktrees from a committed revision; record ownership and acceptance in the
implementation ledger. Preserve unrelated edits and retained worker branches.
Make reviewed local checkpoints. Publish only within the user's authorization.

Use the configured host resource gate for heavy builds when one is available;
host-specific paths and coordination tools stay outside this package. Limit
initial compile concurrency to four jobs. Build and test with CMake/CTest;
exercise ASan/UBSan where supported. Keep generated outputs in ignored build
folders. Validate the installed CMake package from a separate consumer.

Keep the library independent of application policy. A multiverse owns backing
storage, a world is a logical state, and a timeline is an ordered progression.
Do not label design-only codecs, schedulers or persistence as implemented.
