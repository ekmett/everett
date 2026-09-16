# Doxygen metadata and declaration ownership

Each public header starts with one Doxygen file comment containing its file
marker, author, brief and SPDX notices. Each field appears once, and no file
metadata footer follows the code. A blank comment paragraph separates the brief
from the following license block so Doxygen does not include the notices in the
brief. This follows the
[REUSE recommendation to place licensing information near the top](https://reuse.software/spec-3.3/#comment-headers).
The `\file` command attaches that comment to its containing file; it does not
attach the metadata to the first namespace, structure or function.
Doxygen's
[structural-command documentation](https://www.doxygen.nl/manual/docblocks.html#structuralcommands)
describes this explicit association. The generated XML is also checked to verify
that declarations retain the right owners.

## Markdown pages and math

The README is the main page. `AGENTS.md`, `docs/*.md`, `bench/*.md`, the proof README,
the optional GPU guides, `THIRD_PARTY.md`, and the vendored CRC provenance and license Markdown are
included as pages alongside the API reference.

Use `$...$` for inline math and `$$...$$` for display math in Markdown.
`tests/doxygen_markdown.py` adapts those delimiters to Doxygen's formula commands
without changing line counts or rewriting the source files. Code spans, fenced
and indented code, escaped dollars and ordinary currency remain literal. The
generated HTML uses MathJax; its default script is loaded from the configured CDN.
Use `\mathrm{rank}` for named functions in shared Markdown math: GitHub's
renderer rejects `\operatorname` in these documents.

GitHub-style heading IDs keep local section links usable. Doxygen 1.9.8 leaves
some links to headings in other Markdown files unresolved, and can link to an
empty file compound when a Markdown page starts with a notice. The build repairs
these links in HTML and XML using the generated page and section IDs. GitHub
numbers duplicate headings within a page; Doxygen also adds suffixes for titles
on other pages. The repair matches the page-local heading sequence to the actual
generated anchors, preserving explicit IDs and rejecting ambiguous targets.
Inline code and emphasis in headings remain part of their text. Unrecognized
title spellings keep their exact generated IDs instead of guessing an anchor.
The checks verify page inclusion, formula contents, heading targets and code literals.
Mermaid fences remain code in this Doxygen configuration.

Links to Lean files, source examples and license texts resolve relative to their
original Markdown file. The build copies these files under the HTML directory's
`source/` tree, checks their bytes and rewrites the links. No linked file may
escape the source tree. For extensionless names, write `./LICENSE` or
`./lean-toolchain` so Doxygen recognizes the link.

## Configuration

```text
ALIASES = "license=@code{.spdx}"
ALIASES += "endlicense=@endcode"
```

These aliases preserve the three SPDX notice lines as a code block. The author
and file brief remain separate metadata. The public-header notices record Everett's
`BSD-2-Clause OR Apache-2.0` license choice. Generated CRC kernels retain their
upstream notices; see [third-party components](../THIRD_PARTY.md).
The two-file fixture checks declaration ownership with this same top-header layout.
Doxygen describes alias expansion in its
[custom-command manual](https://www.doxygen.nl/manual/custcmd.html).

Documentation tooling is optional and requires Doxygen 1.9.8 or newer and
Python 3.9 or newer. It is not an installed-package dependency. Graphviz is not
required by this configuration.

```sh
cmake -S . -B build-docs -DEVERETT_BUILD_DOCS=ON
cmake --build build-docs --target everett_docs --parallel 4
ctest --test-dir build-docs -R '^everett[.]doxygen$' --output-on-failure
```

Open `build-docs/docs/reference/html/index.html` for the reference documentation.
Generated Doxyfiles, XML and diagnostic logs remain alongside it. The CTest
check uses a separate `docs-test` directory. With `EVERETT_BUILD_TESTS=OFF`, the
documentation target remains available but the CTest check is not registered.

## Publishing

I publish the checked HTML directly to the `gh-pages` branch. The site is
[ekmett.github.io/everett](https://ekmett.github.io/everett/). GitHub Pages uses
that branch's root directory; no Actions workflow generates the documentation.

Build `everett_docs`, then copy the contents of
`build-docs/docs/reference/html/` into a separate `gh-pages` worktree. Keep its
Git metadata, replace the previous generated site, and add an empty `.nojekyll`
file so GitHub serves Doxygen's underscored files unchanged. Commit with the
source revision and push `gh-pages`. Updating `main` alone does not update the
site. The publication should include the complete generated tree, including
search assets and bundled source files. The checker clears generated HTML/XML
before each run so removed declarations cannot leave stale published pages.

## What the check establishes

`tests/check_doxygen.py` runs Doxygen over the actual public headers and checks:

- Each file compound has its own exact brief, author and all three SPDX notices.
- One combined file comment precedes code and contains the file marker, author,
  brief and all SPDX fields. Each SPDX field appears exactly once in the source.
- The author and brief are outside the SPDX code block, and file metadata does
  not appear in namespace, structure or member descriptions.
- Namespace functions and class members have the expected qualified owners,
  source files and declaration lines. Concrete cases include
  `multiverse<P>::open_object`, `mapped_file::open`, both `mapped_slice::bytes`
  ref-qualified overloads, `profile_view<P, Role>::reconstruct_at`,
  `file_detail::get`, `crc32c`, `query_root<P>::build`,
  `query_root_builder<P>::finish`, the query cursor's `step` and `take_match`,
  prepared root adoption, shape-only profile construction, mapped pair binding,
  mapped profile scanning, section materialization and envelope encoding, native writer finalization,
  incremental merge steps, Elias–Fano selection, profile block offsets and carried
  cursor comparisons, native-array adoption, and SQLite reservation, save
  and reader acquisition.
  Template parameters are checked as well.
- Two files with same-named functions, same-named classes in distinct namespaces,
  overloads and distinct documentation markers retain their expected ownership,
  descriptions and exact source locations beneath combined top file comments.
- Negative fixtures reject footer-only or split file metadata, duplicate blocks,
  author/brief commands inside the license, SPDX fields outside the license and
  a missing license terminator.
- A baseline without the aliases emits exactly the two expected unknown-command
  warnings per header. The configured run must emit no warnings.

The checker reports the exact header and function/overload counts for the source
revision being documented. The unconfigured baseline must emit two warnings per
header, exclusively for `\license` and `\endlicense`; these are intentional
negative controls, not warnings accepted in the configured publication.

These checks verify file metadata and the tested lexical associations. Some
APIs still lack prose descriptions. `EXTRACT_ALL=YES` exposes declarations for
inspection; ordinary `//` implementation comments do not
automatically become Doxygen member descriptions. The reference includes
private declarations to make ownership inspectable; that does not make them
public API. The check does not parse CMake or Python source metadata and is not
a whole-project REUSE audit.
