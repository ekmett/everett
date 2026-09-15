<!--
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-->

fast-crc32 provenance
====================

I vendor the generator and license files from
[corsix/fast-crc32 at 13f5289ceb6065d014f9e1e64f161ea7a926c3b7](https://github.com/corsix/fast-crc32/tree/13f5289ceb6065d014f9e1e64f161ea7a926c3b7).
These four files are unchanged from that revision:

| File | SHA256 |
| --- | --- |
| `generate.c` | `d6980f19f0d814ffb9cf2a48692c13b2ae36d04325ff93f411498e5aa2329bf1` |
| `LICENSE.md` | `16627d1e08d5f2204b78394098eeec9c49a8a1de63c6dc144d87e6117facefb3` |
| `LICENSE.MIT.md` | `2a089b18ed6324886d134edbf9c5e9cde62e13458f30a10200a1f78089f90be7` |
| `LICENSE.zlib.md` | `715a5af28f2eb61eae78b857a77d796787ccc71b31ead1078aa5fc9e9727f900` |

The generator explicitly applies its MIT-or-zlib choice to both the program and
its output. I preserve those terms in the generated files; they are not
relicensed as Diet-authored code. I do not vendor the upstream benchmark
harness or its separate `third_party` collection.

Regeneration
------------

From an Diet source checkout, with Python 3 and a host C compiler available:

```sh
python3 tools/generate_crc32c.py
python3 tools/generate_crc32c.py --check
```

The script compiles the pinned generator in a temporary directory and selects
CRC32C's Castagnoli polynomial for each backend. Its adaptations give the C
output C++ header linkage and separate namespaces, use alias-safe scalar loads,
adjust C literals, and scope helper macros. Each generated file records its
exact ISA and algorithm arguments. Regeneration is a developer operation; it is
not part of configuration, compilation, installation or ordinary consumption.

I check in all generated backends under `include/diet/detail/` and install
them with the headers. The generator and script remain in the source checkout;
the installed package needs neither. This provenance document and all three
upstream license files are installed together so their relative links remain
valid.

The available backends include a portable fallback, ARM variants and x86
variants. Selection depends on the compiler and available target features;
shipping a generated backend does not establish that it has been executed or
benchmarked on every platform.
