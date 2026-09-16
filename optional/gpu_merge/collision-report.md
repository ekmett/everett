Temporary collision rank: checked alternatives
==============================================

I compare six optional cancellation-bitmap paths with the checked cache/word-emitter merge. All seven paths start from the same compressed mapped input files and produce byte-identical canonical output. This is an exploratory comparison, not an automatic dispatcher or a universal winner.

The 512-bit rank plus compact-A path has disjoint faster observed ranges on the three balanced 32-byte-prefix fixtures. Other shapes have overlapping ranges, and its long-prefix median is slower. I retain the original path and all six alternatives; a metadata-based selector needs separate calibration and held-out checks.

Complete-path measurements
--------------------------

Times are milliseconds, median [minimum–maximum] of three trials. N is the larger input count, followed by prefix bytes, nominal tie percentage and input-size ratio. “Base” uses Merge Path, keep flags and a full survivor scan. “Direct” uses two independent lower-bound searches and corrected positions. “Tiled” walks groups of 32 records. “Compact” first compacts surviving older references, then merges those disjoint references with the newer input. The number denotes the temporary rank block size; no final rank15 representation changes.

| N / prefix / ties / ratio | Base | 512 direct | 512 tiled | 512 compact | 2048 direct | 2048 tiled | 2048 compact |
|---|---:|---:|---:|---:|---:|---:|---:|
| 16384 / 32 / 50 / 1 | 6.803 [6.684–7.524] | 6.638 [4.146–7.894] | 7.058 [4.643–7.765] | 5.124 [3.312–6.465] | 4.123 [3.300–7.203] | 5.387 [3.476–6.324] | 4.005 [2.981–5.383] |
| 65536 / 32 / 50 / 1 | 7.472 [6.668–8.126] | 5.649 [4.676–8.879] | 5.458 [4.691–8.479] | 5.587 [4.131–5.846] | 5.877 [3.975–5.975] | 7.895 [4.549–11.537] | 6.184 [5.133–8.518] |
| 262144 / 32 / 50 / 1 | 13.442 [12.784–14.985] | 8.020 [7.081–9.447] | 9.236 [8.761–13.162] | 8.412 [8.072–9.339] | 11.585 [7.720–14.748] | 9.800 [9.492–12.289] | 10.880 [8.949–12.569] |
| 65536 / 0 / 50 / 1 | 6.035 [4.440–6.733] | 5.396 [4.617–6.841] | 6.798 [3.646–9.068] | 4.746 [4.404–7.178] | 4.588 [4.268–4.736] | 3.868 [3.532–6.279] | 6.264 [5.393–6.455] |
| 65536 / 256 / 50 / 1 | 8.829 [7.765–11.768] | 9.387 [7.799–13.218] | 13.998 [11.132–14.080] | 9.753 [8.381–13.159] | 7.831 [7.503–13.085] | 10.671 [8.887–17.428] | 10.368 [10.019–16.281] |
| 65536 / 32 / 0 / 16 | 8.934 [5.320–10.036] | 6.582 [4.462–7.404] | 7.349 [7.285–8.223] | 5.847 [4.969–7.797] | 3.817 [3.404–4.251] | 6.837 [5.186–7.224] | 5.099 [4.365–7.384] |
| 65536 / 32 / 100 / 1 | 5.945 [4.815–8.769] | 5.084 [3.346–5.152] | 5.487 [5.398–7.599] | 5.939 [4.742–5.996] | 4.763 [3.787–6.145] | 5.998 [5.630–7.256] | 4.984 [4.755–7.956] |

At 262,144 records per input, the 512-direct median is 8.020 ms and the 512-compact median is 8.412 ms, versus 13.442 ms for the same-run base. The compact path's observed range is 8.072–9.339 ms, versus 12.784–14.985 ms. For the 256-byte-prefix fixture, compact's median is 9.753 ms versus base's 8.829 ms, with overlapping ranges. These short samples do not establish a reliable ranking among all six alternatives.

The 512-directory answers a temporary cancellation rank with at most two popcounts; the 2048-directory uses the existing 512-run populations and scans a partial run. The measured result includes constructing either directory, so a faster query does not automatically imply a faster complete merge. Tiled marking can spend substantial work crossing gaps between sparse incoming keys; it is not an unconditional imbalance optimization.

Ordering-stage medians
----------------------

This stage includes prefix-cache construction, marking/ordering, rank or keep scans, all output-reference construction and completion waits. In the collision paths it includes the scalar collision-count read and second command submission. Scratch allocations before the stage remain included in the complete total. These stage times are diagnostic and already part of the total.

| N / prefix / ties / ratio | Base | 512 direct | 512 tiled | 512 compact | 2048 direct | 2048 tiled | 2048 compact |
|---|---:|---:|---:|---:|---:|---:|---:|
| 16384 / 32 / 50 / 1 | 1.636 | 1.868 | 2.378 | 1.780 | 1.821 | 2.360 | 1.649 |
| 65536 / 32 / 50 / 1 | 4.317 | 2.251 | 2.886 | 1.582 | 1.935 | 3.038 | 1.321 |
| 262144 / 32 / 50 / 1 | 6.758 | 2.898 | 3.637 | 2.621 | 3.875 | 3.361 | 4.032 |
| 65536 / 0 / 50 / 1 | 1.175 | 1.477 | 1.615 | 1.340 | 1.488 | 1.196 | 1.395 |
| 65536 / 256 / 50 / 1 | 5.002 | 6.346 | 10.774 | 6.763 | 5.052 | 8.025 | 7.432 |
| 65536 / 32 / 0 / 16 | 1.907 | 3.885 | 2.533 | 1.397 | 1.262 | 3.968 | 2.053 |
| 65536 / 32 / 100 / 1 | 2.445 | 1.406 | 2.113 | 2.373 | 1.786 | 2.547 | 2.179 |

Method and correctness
----------------------

- One otherwise idle Apple M2 Max, seven fresh processes in the recorded fixed mixed order, one process per mode. Each process prewarms its pipelines, retains a 4,096-record warm-up, then runs the same seven fixtures with three alternating CPU/GPU trial orders. This gives 147 measured CPU/GPU pairs plus seven warm-ups. Mode order is not counterbalanced across processes, and ranges are descriptive, not confidence intervals. All trials remain in the logs.
- The complete GPU total includes metadata inspection, mapped imports, all allocation, GPU input EF selection/parsing, inherited-prefix tree, cache, cancellation or base ordering, length and offset scans, mapped payload and output EF construction, host waits, final framing and CRC. It excludes pipeline compilation, fixture generation, correctness oracles, final local-object destruction, fsync and catalog publication. The CPU reference remains the existing serial compressed merger and complete canonical encoder plus mapped-output copy.
- Every measured output matches the CPU file byte for byte, passes semantic scan and CRC, and has identical survivor count, payload-bit extent and file size across all seven modes. No tombstone is discarded; newer values, including absence, win ties.
- All nine CTests passed: the existing three suites and six collision variants. Each collision variant passed all 81 deterministic full-file adversarial fixtures and both 60-map temporary-rank suites. The rank suites check every query position including EOF, exact packed directory bytes, counts, untouched bitmap and guards. They include zero length, full/empty/sparse/random maps and 32/512/2048-bit boundaries. Actual atomic marking and corrected placement are covered by the complete-file fixtures.
- All six collision modes also passed with a strict O2 ASan/UBSan-instrumented CPU host. These sanitizers instrument host code, not shader memory accesses. SPIR-V validation and actual Metal execution passed for every kernel.

Reproduction and provenance
---------------------------

Build the optional target, then run `prototype kernels.metallib scratch collision512-compact` for correctness or append `-bench` for this sweep. Other modes are `collision512`, `collision512-tiled`, `collision2048`, `collision2048-tiled` and `collision2048-compact`. `optimized-bench` is the retained comparison path. The default header-only library and production rank formats are unchanged.

[Raw rows](results/collision-rows.json), individual `collision-*.log` files and the [source/artifact manifest](results/collision-manifest.json) retain the exact measured identity. The manifest records original pre-normalization source hashes, all included library headers and both host binaries plus the shader library. Later public name normalization does not turn those hashes into hashes of a different build. The exact measured source and executable artifacts remain archived separately. These results must not be compared with earlier separate-run base medians as isolated optimization attribution.
