# Doxygen metadata and declaration ownership

Everett keeps its file documentation blocks at the end of each public header.
The `\file` command attaches that block to its containing file; it does not
attach the block to the last namespace, structure or function. Doxygen's
[structural-command documentation](https://www.doxygen.nl/manual/docblocks.html#structuralcommands)
describes this explicit association. Everett also checks the generated XML
instead of relying on that convention alone.

## Configuration

The license commands come from
[`ekmett/ein`'s Doxygen configuration](https://github.com/ekmett/ein/blob/50e9533700c065790a0c532fd69edafbd2a49932/doc/Doxyfile.in#L128):

```text
ALIASES = "license=@code{.spdx}"
ALIASES += "endlicense=@endcode"
```

These aliases preserve the three SPDX notice lines as a code block. The author
and file brief remain separate metadata. They do not change any license terms.
`ein` supplies the command definitions; its representative `src/ein/wait.hpp`
puts the file block at the top, so Everett's fixture comparison independently
checks the end-of-file placement. Doxygen describes alias expansion in its
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
- The SPDX code block ends before the author and brief, and file metadata does
  not appear in namespace, structure or member descriptions.
- Namespace functions and class members have the expected qualified owners,
  source files and declaration lines. Concrete cases include
  `multiverse<P>::open_object`, `mapped_file::open`, both `mapped_slice::bytes`
  ref-qualified overloads, `profile_view<P, Role>::reconstruct_at`,
  `file_detail::get` and `crc32c`. Template parameters are checked as well.
- Two files with same-named functions, same-named classes in distinct namespaces,
  overloads and distinct documentation markers retain identical ownership and
  descriptions when the file blocks move from before to after the declarations.
  The source locations must change by precisely the actual movement.
- A baseline without the aliases emits exactly the two expected unknown-command
  warnings per header. The configured run must emit no warnings.

On 2026-09-15, Doxygen 1.9.8 passed these checks for all 18 public headers,
seven real function/overload cases and twelve fixture symbols. The unconfigured
baseline had 36 warnings, exclusively for `\license` and `\endlicense`.

This verifies file metadata and the tested lexical associations. It does not
claim complete prose documentation for every API. `EXTRACT_ALL=YES` exposes
declarations for inspection; ordinary `//` implementation comments do not
automatically become Doxygen member descriptions. The reference includes
private declarations to make ownership inspectable; that does not make them
public API. The check does not parse CMake or Python source footers.
