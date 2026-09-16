# Everett

Read README.md, docs/design.md and docs/implementation.md before changing the
architecture. Keep the design and implementation status distinct and current.
Keep the README an approachable introduction to the common use case and working
APIs, with recommended byte/bit defaults and small usage examples. Put backend
details, tuning and unusual policies in docs/usage.md and the focused guides.
Write the documents in my voice: first person for my design
choices, and "we" when walking through an argument with the reader. Do not
describe me or my ideas from a distant third-person perspective. Keep factual
bibliographic attribution and copyright notices intact. Describe the current
API directly; omit internal prototype history and incidental development provenance.
Follow the approachable,
example-led style of my Haskell packages: setext headings, useful documentation
links, Contact Information and the final -Edward Kmett signoff. Demonstrate
tested features; keep detailed implementation tracking in docs/implementation.md.
Until the first release, keep names consistent across APIs, file signatures,
fixtures, benchmark labels and documentation. Update them together rather than
retaining aliases or compatibility for discarded development names. Preserve
measured results honestly: distinguish original measurement hashes from hashes
of artifacts whose names have been normalized.
Use `$...$` for inline Markdown math and `$$...$$` for display math. Include
the README and design Markdown in Doxygen, and verify that equations render.

## Coding style

- C++20, with no modules or header units.
- Follow the lowercase, struct-first style of https://github.com/ekmett/bad
  (reference revision 978b8056ffafc992fd1b7300ccf7bd1219cd3a20), without importing
  its ISA assumptions or macro-dispatch machinery.
- Use lowercase/snake_case names for concrete types, functions, namespaces,
  variables and files. Short uppercase template parameters are welcome.
- Use struct with explicit private/protected sections, two-space indentation,
  .h headers with `#pragma once`, .cc implementations, and T const & spelling.
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
- Put file author and brief metadata in the leading file comment alongside the
  SPDX notices, outside the license code block. The optional Doxygen build uses
  license aliases and checks declaration ownership; see docs/doxygen.md before
  changing those commands or their placement.

## Work and verification

The main checkout belongs to the integration owner. Workers use isolated Git
worktrees from a committed revision; record ownership and acceptance in the
implementation ledger. Preserve unrelated edits and retained worker branches.
Make reviewed local checkpoints. Publish only within my authorization.

Use the configured host resource gate for heavy builds when one is available;
host-specific paths and coordination tools stay outside this package. Limit
initial compile concurrency to four jobs. Build and test with CMake/CTest;
exercise ASan/UBSan where supported. Keep generated outputs in ignored build
folders. Validate the installed CMake package from a separate consumer.

Treat asymptotic improvements as the default requirement. Use the fractional
cascade to position range cursors rather than scanning preceding records.
Choose a worse asymptotic fallback only when measurements demonstrate clear
gains across a broad, practical range, and document that measured boundary.

Keep the library independent of application policy. A multiverse owns backing
storage, a world is a logical state, and a timeline is an ordered progression.
Do not label design-only codecs, schedulers or persistence as implemented.
