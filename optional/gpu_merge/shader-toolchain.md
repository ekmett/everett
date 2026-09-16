Everett shader toolchain
====================

`shader_compile.py` is a self-contained Python standard-library tool for the
optional GPU experiment. It compiles one explicit HLSL entry with DXC, validates
the SPIR-V, and optionally produces Metal source and reflected C++ bindings.
It requires no other project checkout.

Install the external tools independently and place them on `PATH`, or provide
`--dxc`, `--spirv-val`, and `--spirv-cross` paths. The helper does not download or
build them. A Vulkan-only invocation needs DXC and `spirv-val`; `spirv-cross` is
used only when Metal or reflected bindings are requested. Building Metal binaries
also requires Apple's SDK tools, invoked by the optional build driver.

```sh
python3 shader_compile.py --source kernels.hlsl --entry merge_order \
  --output-entry main --profile cs_6_0 --define COMPRESSED_INPUT=1 \
  --spv build/merge_order.spv
```

The Vulkan pipeline's entry name must match `--output-entry` (or `--entry` when
no renamed output entry is requested). `--msl-entry` names the Metal entry and is
checked against SPIRV-Cross reflection before generation.

The default pipeline uses HLSL 2021, shader model 6.0, Vulkan 1.3, and scalar
buffer layout. Validation uses the matching target and scalar-block-layout flag.
Metal generation uses MSL 3.2 and descriptor-decoration bindings by default. All
requested artifacts are compiled and validated before publication; individual
file replacement is atomic, while replacing several files is not a filesystem
transaction. A failed build must be retried before its outputs are consumed.

Licensing and attribution
-------------------------

The Python helper is copyright 2026 Edward Kmett and is distributed under Everett's
`BSD-2-Clause OR Apache-2.0` license. Its dependencies are Python standard-library
modules. No third-party source code, compiler binary, SDK, or generated artifact
is bundled by this helper.

DXC, SPIRV-Tools, SPIRV-Cross, Python, and platform SDKs retain their own licenses
and notices. Everett's source license does not change the licenses of those
separately installed tools or grant redistribution rights to their binaries.
