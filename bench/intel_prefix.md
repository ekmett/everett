Intel paths for bounded 512-bit rank prefixes
===========================================

`rank_view::rank` now has compile-time Intel SIMD paths for complete readable
512-bit runs. The allocation check still requires eight words before selecting
a vector path. A shorter final allocation uses the bounded portable prefix;
an exact run boundary returns its directory answer before reading payload.
The 512-bit population builder is unchanged.

| Target features | Prefix implementation |
| :--- | :--- |
| AVX512F + AVX512VPOPCNTDQ | Masked qword load, boundary-word mask, vector popcount and reduction |
| AVX512F + AVX512BW | Same masked data, byte nibble lookup, SAD and reduction |
| AVX2 | Two unaligned 32-byte loads, qword masks, byte nibble lookup, SAD and reduction |
| ARM NEON / other targets | Existing NEON / portable paths |

VPOPCNTDQ takes precedence over BW, then AVX2. There is no runtime feature
dispatch and no new exported compiler flag. Compile each consumer for the
instruction set it can execute.

The direct helpers accept 0–512 prefix bits and require eight readable words.
For AVX2, each selected byte contributes at most eight; adding both vectors
before SAD gives at most sixteen per byte and 512 overall. AVX-512 selects full
qword lanes plus the boundary lane, then masks the boundary's remaining bits.
At 512 bits the full mask is `0xff` and the boundary mask is zero. At zero bits
the boundary mask clears the first loaded word. No vector reads beyond the
eight-word extent. Independent source review found no mask, reduction-width or
feature-guard defect.

Validation and limits
---------------------

The production checkpoint is `1b1446c`. The focused rank suite passes under
ASan/UBSan on native ARM and translated scalar x86-64. It includes independent
prefix oracles, all bit positions, partial tails and inaccessible guard pages.
The new direct-helper assertions cover every prefix from 0 through 512 at eight
uint64 alignments for zero, all-one and random words.

All seven compile profiles pass strict Apple Clang 21 checks:

* Native ARM, scalar x86-64 and AVX2 compile and link with ASan/UBSan.
* AVX512F alone, F+VPOPCNTDQ, F+BW, and F+BW+VPOPCNTDQ compile the complete rank
  test translation unit. DQ and VL are explicitly disabled; the VPOPCNTDQ-only
  profile also disables BW, and the BW-only profile disables VPOPCNTDQ.
* Separate optimized assembly probes cover each x86 profile. Together with
  the capability probe, the driver records fourteen successful compiler calls.

**The Intel SIMD implementations have not been executed or timed here.** The
local Rosetta capability probe advertises neither AVX2 nor AVX-512. Only the
scalar x86-64 executable runs under translation; AVX2 and AVX-512 tests compile
but do not run. Native Intel testing remains unavailable. No speedup is claimed
for these Intel paths, and the [stored-spacer M2 benchmark](rank_spacers.md)
measures an earlier revision without them.

The VPOPCNTDQ assembly contains a mask-zeroed `vmovdqu64`, boundary masking and
`vpopcntq`; Clang narrows the per-qword counts and reduces them with SAD. The
AVX2 and BW paths contain nibble shuffles and SAD reductions. Compilation with
DQ/VL disabled confirms those extra feature sets are not needed. Compiler
vectorization may also turn the bounded scalar tail loop into masked vector
loads; its source extent remains bounded by the number of contributing words.

The checked header and committed header differ only by the two-line comment
stating the prefix helper precondition. The artifact manifest records both
hashes and verifies that removing those comment lines reproduces the tested
header exactly. The existing `rank_bounds` benchmark adapter also passes its
bounded oracle run after changing it to build the old bitmap directory from
the original words; it no longer shares incompatible packed run bytes.

Evidence
--------

* [Compiler commands, header/test hashes, feature probe and runtime output](results/intel_prefix_checks.json).
* [Driver](results/intel_prefix_check.py.txt), [capability probe](results/intel_prefix_caps.cc.txt)
  and [assembly probe](results/intel_prefix_assembly.cc.txt), preserved as text artifacts.
* [AVX2 assembly](results/intel_prefix_avx2.s.txt), [AVX512BW assembly](results/intel_prefix_avx512bw.s.txt)
  and [VPOPCNTDQ assembly](results/intel_prefix_vpopcnt.s.txt).
* [Artifact hashes and tested/committed header comparison](results/intel_prefix_artifacts.json).

The driver uses at most two compiler jobs and was run under the host's CPU and
build-directory resource lease. All paths and commands used are in the manifest.
