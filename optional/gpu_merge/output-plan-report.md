Optional fused GPU output planning
==================================

The fused plan removes one blocking host/GPU handoff, and the combined sizing/framing phase has a lower median in 11 of 12 fixtures. Complete merge timings remain mixed: 5 lower medians and 7 higher, with 11 overlapping process-median ranges and one disjoint regression. I keep this path opt-in and leave the calibrated default unchanged.

What changed
------------

After computing record sizes and scanning bit offsets, a GPU kernel derives the total extent, actual common value width, EF universe and low width. It computes the exact sparse-group counts and scans them in that same command. The host reads the constant metadata and sparse total once, checks the metadata independently, and allocates the exact output mapping. The original path waits after size scanning, computes the scalar metadata on the CPU, then submits sparse planning and waits again. This change reduces the five blocking handoffs to four.

Scratch allocation count and final output allocation are unchanged; the existing status allocation grows from 4 to 20 bytes. There is no pool or retained scratch. The pre-scan conservative output bound remains, the status flag is preserved, and the host checks the returned plan before creating the output. The canonical output format and CPU CRC step are unchanged. Both measured paths use the original levelwise prefix tree.

`optimized-plan` runs the correctness corpus and `cutover-plan CASE TRIALS` runs one existing fixture; the corresponding modes without `-plan` preserve the separate plan. `optimized-tiles-plan` additionally exercises the independent tiled tree candidate, but this combination was not timed here. These are optional standalone construction experiments, not a durable-runtime dispatcher.

Correctness
-----------

All 16 optional CTests passed in 8.47 s. The new plan oracle checks 12 synthetic cases at both descriptor strides, including a nonzero first compacted reference, single/partial/full blocks, repeated offsets, fixed value stride, low widths through 29, and the exact dense/sparse threshold. The CPU EF builder independently supplies expected metadata, every sparse count and sparse start. Input, status and count guards remain unchanged. Plan-only and combined tiled/plan modes also pass the complete 81-case byte oracle, mapped scans, EF input/output, word emission and index-rank checks. DXC, SPIR-V validation and Metal compilation passed.

Paired complete-path results
----------------------------

The 12 fixture IDs and 72-process schedule were fixed before this comparison and match the preceding tiled experiment. Each of three fresh-process repetitions runs both modes in alternating order; each process checks one untimed warmup and three timed trials. Tables show median [minimum–maximum] of the three process medians. All 216 measured CPU/GPU pairs are retained. Every process passed the whole-file, scan and CRC oracle, and every fixture/result field agreed across modes.

| Case | Records A / B | Prefix bytes | Ties % | Value bytes | Separate total ms | Fused total ms | Fused change | Ranges |
|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 0 | 256 / 256 | 32 | 50 | 8 | 5.495 [2.202–5.610] | 2.740 [2.627–2.798] | 50.1% lower | overlap |
| 1 | 1024 / 1024 | 32 | 50 | 8 | 4.082 [2.743–4.917] | 4.119 [2.225–4.273] | 0.9% higher | overlap |
| 2 | 4096 / 4096 | 32 | 50 | 8 | 3.184 [2.849–4.381] | 3.368 [2.270–3.953] | 5.8% higher | overlap |
| 3 | 16384 / 16384 | 32 | 50 | 8 | 4.133 [3.178–5.972] | 3.177 [2.943–3.917] | 23.1% lower | overlap |
| 4 | 65536 / 65536 | 32 | 50 | 8 | 6.310 [3.868–7.483] | 6.477 [4.913–7.064] | 2.7% higher | overlap |
| 5 | 262144 / 262144 | 32 | 50 | 8 | 10.462 [9.076–12.647] | 7.905 [6.796–13.688] | 24.4% lower | overlap |
| 9 | 4096 / 4096 | 256 | 50 | 8 | 5.971 [5.088–7.270] | 5.073 [4.226–7.408] | 15.0% lower | overlap |
| 11 | 4096 / 4096 | 32 | 100 | 8 | 2.693 [2.451–2.812] | 3.421 [2.783–5.586] | 27.0% higher | overlap |
| 13 | 4096 / 4096 | 32 | 50 | 512 | 4.411 [4.191–4.474] | 5.108 [4.607–6.420] | 15.8% higher | disjoint |
| 34 | 2048 / 2048 | 64 | 25 | 32 | 5.948 [4.216–6.020] | 4.541 [2.727–6.712] | 23.7% lower | overlap |
| 38 | 8192 / 8192 | 64 | 25 | 32 | 3.108 [2.996–7.267] | 3.419 [3.174–4.253] | 10.0% higher | overlap |
| 44 | 32768 / 4096 | 0 | 0 | 512 | 6.681 [5.895–8.973] | 6.978 [6.880–7.055] | 4.4% higher | overlap |

Sizing and framing together
---------------------------

Fusion moves sparse planning from the recorded assembly stage into the size stage. Their individual columns no longer measure the same work. For a comparable boundary below, first sum `size_ms + assembly_ms` within each trial, then aggregate process medians. This includes size/scan, sparse planning, output mapping, envelope and CPU CRC, but excludes the intervening packed GPU emission phase.

| Case | Separate size + assembly ms | Fused size + assembly ms |
|---:|---:|---:|
| 0 | 0.571 [0.450–1.225] | 0.339 [0.307–0.372] |
| 1 | 0.641 [0.529–1.008] | 0.338 [0.319–0.480] |
| 2 | 0.580 [0.539–0.594] | 0.357 [0.356–0.701] |
| 3 | 0.609 [0.606–1.475] | 0.431 [0.430–0.468] |
| 4 | 1.162 [0.828–1.490] | 0.711 [0.659–0.868] |
| 5 | 1.542 [1.519–2.442] | 1.137 [1.119–1.286] |
| 9 | 1.121 [0.504–1.221] | 0.359 [0.347–0.395] |
| 11 | 0.517 [0.495–0.526] | 0.355 [0.353–0.516] |
| 13 | 0.763 [0.758–0.783] | 0.697 [0.585–0.725] |
| 34 | 0.526 [0.512–0.897] | 0.410 [0.408–0.413] |
| 38 | 0.670 [0.508–0.713] | 0.437 [0.435–0.461] |
| 44 | 1.687 [1.679–1.922] | 1.788 [1.464–1.919] |

The observed ranges are not confidence intervals. Other unchanged phases varied enough that lower sizing/framing time did not consistently lower complete time. GPU command timestamps describe command intervals, not isolated kernel instructions. Stage medians need not sum to the total median. No fixture was dropped and no further runs were selected from these outcomes.

Both totals start from existing warm mapped compressed inputs and finish with a complete checksummed scratch file. They include imports, all allocation, GPU parsing/tree/cache, merge/compaction, exact scans/planning/emission, output mapping, waits, envelope and CRC. They exclude pipeline creation, fixture generation, correctness verification, final local-object destruction, fsync and catalog publication. The existing single-thread CPU merger and complete-file baseline remain in each trial.

Identity and artifacts
----------------------

Measured source: `79a6e7ab7c390ee91dde954027450dda6bb5cd7a`. Both paths use the same newly qualified executable and shader library; only the optional mode differs.

- Executable SHA-256: `d39e8612fb9dcabc4281f2bdd4ed393dcdf0db7fc4636579005857d9d5ee8ef1`
- Shader library SHA-256: `7884bc0d7aa1f4047e1a0356f14844f334c5d3755c693d91130d88e632762084`
- Collection: `2026-09-16T06:16:11Z` to `2026-09-16T06:16:28Z` on `Apple M2 Max|registryID=4294968459`.

[Raw rows](results/output-plan/results.csv), [process summaries](results/output-plan/summary.json), and [source/artifact provenance](results/output-plan/metadata.json) retain the original values. Source and artifact hashes were unchanged across collection. The previous 50-case cutover rule was not refitted and does not identify these new artifacts; CPU fallback on identity mismatch remains.
