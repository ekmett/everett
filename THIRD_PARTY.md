<!--
SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
-->

Third-party components
======================

I use [Peter Cawley's fast-crc32](https://github.com/corsix/fast-crc32) to
generate Diet's portable and accelerated CRC32C implementations. I pin the
generator to commit `13f5289ceb6065d014f9e1e64f161ea7a926c3b7`.

The upstream generator and its generated output are available under **MIT OR
zlib**, at the recipient's choice. These components retain Peter Cawley's
copyright and their upstream terms; Diet's BSD-2-Clause OR Apache-2.0 choice
does not replace them. The unmodified upstream notices are included together:

- [License choice](third_party/fast-crc32/LICENSE.md)
- [MIT license](third_party/fast-crc32/LICENSE.MIT.md)
- [zlib license](third_party/fast-crc32/LICENSE.zlib.md)

The source copy is `third_party/fast-crc32/generate.c`. Generated implementations
live in `include/diet/detail/crc32c_*.inc`; their notices identify the upstream
revision, generation parameters and C++ adaptation. I keep the integration
wrapper and regeneration script under Diet's own license.

[Upstream provenance](third_party/fast-crc32/UPSTREAM.md) records the source
hashes and regeneration procedure. Ordinary source and installed consumers use
the checked-in generated implementations: they do not fetch this dependency or
run the generator. Installed packages carry this document and the upstream
notices under the configured data directory's `diet` subdirectory.
