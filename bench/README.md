Benchmark evidence
==================

I keep source, fixture, command and result names consistent with the current
Everett API throughout pre-release development.

Timing and allocation samples retain their measured values. Recorded measurement
hashes identify the original source and binary bytes, before name normalization;
they do not claim that renamed source was remeasured. The
[artifact manifest](results/artifact_normalization.json) records original and
normalized artifact hashes separately.

The [snapshot helper](snapshot.py) extracts a pinned revision into the current
include layout and records both original and normalized source hashes. Use the
current runners with the revisions named in each report. Reproduction makes new
measurements with normalized source names; it does not replace the recorded
observations.

New measurements of Everett should record their own source hashes and results.
The [implementation ledger](../docs/implementation.md) links the accepted
measurements and describes the current library.

Optional GPU construction
-------------------------

The [GPU merge experiment](../optional/gpu_merge/design.md) reads compressed
mapped inputs and emits a complete compressed output file. Its
[measurements](../optional/gpu_merge/report.md) include parsing, merging,
navigation construction and checksums. The
[collision-rank comparison](../optional/gpu_merge/collision-report.md) explores
temporary cancellation bitmaps; the production rank formats stay unchanged.
The [calibration guide](../optional/gpu_merge/cutover.md) describes a measured
CPU/GPU choice using headers and file sizes.
The [M2 Max results](../optional/gpu_merge/cutover-report.md) retain the complete
50-case calibration and its five successful held-out GPU selections.
The [Vulkan qualification](../optional/gpu_merge/vulkan-qualification.md) records
150 successful native correctness runs on an RTX 4090; it contains no performance
claim.
The [tiled-prefix comparison](../optional/gpu_merge/prefix-tiles-report.md)
reduces tree-construction dispatches but has mixed complete-path results, so
the tiled path remains an explicit experiment.
The [output-plan comparison](../optional/gpu_merge/output-plan-report.md)
removes one host wait, with mixed complete-path results. It also remains opt-in.

This is a separate opt-in Metal program. It does not publish durable catalog
updates, and the ordinary library has no shader-toolchain dependency.

The [fixed-key experiment](../optional/fixed_gpu_merge/README.md) separately
constructs native `.ff` and `.fv` candidates, including scheduled cancellations,
actual payload copying and complete native Elias–Fano output. It uses an
experimental envelope and has its own correctness and timing boundaries.

Unadopted experiments
--------------------

- [Borrowed payload sharing](cola_payload.md): deterministic allocation savings
  for reused in-memory routes, with default ownership costs and no persisted-pair
  reuse. The report includes an archived patch for fresh-clone reproduction.
