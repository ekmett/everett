# Doxygen metadata and declaration ownership

Each public header has its SPDX notices in a Doxygen file block before the code,
and its author and brief in a second file block at the end. Each SPDX field
appears once. This follows the
[REUSE recommendation to place licensing information near the top](https://reuse.software/spec-3.3/#comment-headers).
The `\file` command attaches each block to its containing file; it does not
attach the trailing block to the last namespace, structure or function.
Doxygen's
[structural-command documentation](https://www.doxygen.nl/manual/docblocks.html#structuralcommands)
describes this explicit association. The generated XML is also checked to verify
that declarations retain the right owners.

## Markdown pages and math

The README is the main page. `AGENTS.md`, `docs/*.md`, the proof README,
`THIRD_PARTY.md`, and the vendored CRC provenance and license Markdown are
included as pages alongside the API reference.

Use `$...$` for inline math and `$$...$$` for display math in Markdown.
`tests/doxygen_markdown.py` adapts those delimiters to Doxygen's formula commands
without changing line counts or rewriting the source files. Code spans, fenced
and indented code, escaped dollars and ordinary currency remain literal. The
generated HTML uses MathJax; its default script is loaded from the configured CDN.

GitHub-style heading IDs keep local section links usable. Doxygen 1.9.8 leaves
some links to headings in other Markdown files unresolved, and can link to an
empty file compound when a Markdown page starts with a notice. The build repairs
these links in HTML and XML using the generated page and section IDs. The checks
verify page inclusion, formula contents, heading targets and code literals.
Mermaid fences remain code in this Doxygen configuration.

## Configuration

```text
ALIASES = "license=@code{.spdx}"
ALIASES += "endlicense=@endcode"
```

These aliases preserve the three SPDX notice lines as a code block. The author
and file brief remain separate metadata. The public-header notices record Everett's
`BSD-2-Clause OR Apache-2.0` license choice. Generated CRC kernels retain their
upstream notices; see [third-party components](../THIRD_PARTY.md).
Our fixture comparison checks both end-of-file placement and the split layout.
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

## What the check establishes

`tests/check_doxygen.py` runs Doxygen over the actual public headers and checks:

- Each file compound has its own exact brief, author and all three SPDX notices.
- Each SPDX field appears exactly once in the source, in the leading file block
  before code. The final file block contains only author and brief metadata.
- The SPDX code block ends before the author and brief, and file metadata does
  not appear in namespace, structure or member descriptions.
- Namespace functions and class members have the expected qualified owners,
  source files and declaration lines. Concrete cases include
  `multiverse<P>::open_object`, `mapped_file::open`, both `mapped_slice::bytes`
  ref-qualified overloads, `profile_view<P, Role>::reconstruct_at`,
  `file_detail::get` and `crc32c`. Template parameters are checked as well.
- Two files with same-named functions, same-named classes in distinct namespaces,
  overloads and distinct documentation markers retain identical ownership and
  descriptions with combined file blocks before or after the declarations, and
  with SPDX notices before and author/brief after. The source locations must
  change by precisely the actual movement.
- A baseline without the aliases emits exactly the two expected unknown-command
  warnings per header. The configured run must emit no warnings.

On 2026-09-15, Doxygen 1.9.8 passed these checks for all 22 public headers,
seven real function/overload cases and twelve fixture symbols in all three
metadata layouts. The unconfigured baseline had 44 warnings, exclusively for
`\license` and `\endlicense`.

These checks verify file metadata and the tested lexical associations. Some
APIs still lack prose descriptions. `EXTRACT_ALL=YES` exposes declarations for
inspection; ordinary `//` implementation comments do not
automatically become Doxygen member descriptions. The reference includes
private declarations to make ownership inspectable; that does not make them
public API. The check does not parse CMake or Python source metadata and is not
a whole-project REUSE audit.
