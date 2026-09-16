Optional tiled prefix-tree experiment
=====================================

The tiled builder is correct, but this comparison does **not** support replacing the existing default. Complete-path medians were lower in 7 of 12 fixtures and higher in 5; observed process-median ranges overlap in 9. The three disjoint ranges favor tiling. The existing default and its frozen cutover calibration remain unchanged.

What changed
------------

A workgroup constructs 256 prefix-min-tree leaves and all their interior heap nodes in shared memory. Further passes process 256 completed roots at a time. The resulting tree has exactly the same nodes and padding as the levelwise builder; inherited-prefix queries are unchanged. This reduces tree dispatches from `1 + log2(bit_ceil(input_records))` to `max(1, ceil(log2(bit_ceil(input_records))/8))`. No scratch pooling, input reconstruction, output approximation, or timing-scope change is introduced.

Use `optimized-tiles` for candidate correctness, `optimized-tiles-bench` for the original benchmark set, or `cutover-tiles CASE TRIALS` for one existing calibration fixture. The corresponding modes without `-tiles` retain the levelwise builder. These modes do not automatically select a production runtime backend.

Checks
------

All 14 optional CTests passed in 7.59 s: the existing 13 plus the tiled optimized corpus. The new whole-tree oracle runs 42 fixtures through both builders, comparing every node, padding, node-zero/tail guards, descriptor immutability and exact validation flags. It covers one-record inputs, both empty-side directions, source boundaries around 256, malformed retention/source labels, and 65,537 records requiring two upper reduction passes. The tiled complete merge also retains the 81 whole-file adversarial cases and existing EF, packed-word and index-rank oracles. DXC compilation, SPIR-V validation and Metal compilation passed.

Paired measurements
-------------------

Each cell is the median [minimum–maximum] of three fresh-process medians; each process contributes three measured trials after one verified warmup. The 12 fixture IDs were fixed before collection, pair order was shuffled, and which builder ran first alternated across repetitions. All 72 processes passed complete canonical output bytes, mapped scans and CRC checks. All 216 measured CPU/GPU pairs are retained in the raw case logs. Each process also checks one warmup, whose timing is excluded and not printed. Fixture/result fields agree across modes and repetitions.

| Case | Records A / B | Prefix bytes | Ties % | Value bytes | Levelwise total ms | Tiled total ms | Tiled change | Ranges |
|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 0 | 256 / 256 | 32 | 50 | 8 | 5.581 [5.036–5.614] | 2.720 [1.871–4.686] | 51.3% lower | disjoint |
| 1 | 1024 / 1024 | 32 | 50 | 8 | 5.276 [2.030–5.562] | 5.065 [3.067–5.534] | 4.0% lower | overlap |
| 2 | 4096 / 4096 | 32 | 50 | 8 | 4.243 [2.546–6.379] | 3.480 [2.477–3.489] | 18.0% lower | overlap |
| 3 | 16384 / 16384 | 32 | 50 | 8 | 5.366 [4.238–6.627] | 3.714 [3.004–4.118] | 30.8% lower | disjoint |
| 4 | 65536 / 65536 | 32 | 50 | 8 | 4.954 [4.691–8.155] | 6.691 [4.340–6.769] | 35.1% higher | overlap |
| 5 | 262144 / 262144 | 32 | 50 | 8 | 8.731 [8.306–10.661] | 9.032 [8.194–10.204] | 3.4% higher | overlap |
| 9 | 4096 / 4096 | 256 | 50 | 8 | 6.936 [4.020–8.154] | 6.025 [5.209–8.288] | 13.1% lower | overlap |
| 11 | 4096 / 4096 | 32 | 100 | 8 | 4.894 [4.215–5.856] | 3.605 [2.925–3.781] | 26.3% lower | disjoint |
| 13 | 4096 / 4096 | 32 | 50 | 512 | 5.500 [3.817–6.919] | 5.254 [4.469–5.530] | 4.5% lower | overlap |
| 34 | 2048 / 2048 | 64 | 25 | 32 | 3.016 [2.822–4.966] | 3.279 [3.157–3.962] | 8.7% higher | overlap |
| 38 | 8192 / 8192 | 64 | 25 | 32 | 3.399 [2.994–4.301] | 3.627 [3.222–6.996] | 6.7% higher | overlap |
| 44 | 32768 / 4096 | 0 | 0 | 512 | 6.122 [5.774–6.858] | 7.374 [6.190–10.048] | 20.4% higher | overlap |

The parse stage contains input import, descriptor/tree allocation, GPU framing/tree construction, completion waiting and validation. Its process-median comparisons are:

| Case | Levelwise parse ms | Tiled parse ms |
|---:|---:|---:|
| 0 | 1.100 [1.061–1.104] | 0.513 [0.492–1.108] |
| 1 | 1.116 [0.581–3.222] | 1.020 [0.943–1.052] |
| 2 | 1.226 [0.996–2.442] | 1.219 [0.947–1.317] |
| 3 | 1.677 [1.395–3.368] | 1.185 [1.044–2.325] |
| 4 | 1.142 [1.114–1.495] | 1.395 [1.073–1.733] |
| 5 | 2.777 [2.342–3.069] | 2.383 [2.253–2.400] |
| 9 | 1.203 [0.948–1.251] | 1.212 [1.139–1.248] |
| 11 | 2.090 [1.556–2.091] | 0.939 [0.841–1.393] |
| 13 | 1.469 [1.203–1.501] | 1.607 [1.349–2.034] |
| 34 | 1.062 [0.823–1.361] | 0.977 [0.943–1.180] |
| 38 | 1.038 [0.742–1.094] | 1.033 [1.025–1.300] |
| 44 | 1.725 [1.292–1.823] | 2.158 [1.221–2.730] |

These ranges describe observed runs, not confidence intervals. Unchanged later phases also vary; whole-path differences cannot be attributed solely to eliminated dispatch cost. GPU command timestamps cover command intervals, not isolated kernel instructions. Stage medians need not add to the total median.

Both paths start with the same warm compressed mmap inputs and end with a complete checksummed scratch file. Timing includes imports, all scratch/output allocation, GPU input parsing, prefix tree/cache, merge/compaction, exact sizing/scans, direct packed payload and EF output, all host waits, output mapping, directory and CRC. It excludes pipeline creation, fixture generation, correctness oracles, final local-object destruction, fsync and catalog publication. The five blocking host/GPU handoffs remain.

Identity and raw data
---------------------

Measured source: `ac6e05665946bd74827f6e97da6ea2911715df03`. One executable and one shader library contain both paths, so a mode flag is the only comparison selection.

- Executable SHA-256: `762a6a649207ff710e2d0baf2f6d2770100856b82aec876f33fd823fb7107886`
- Shader library SHA-256: `d452e21f9caa9ec6552ac85e30d16d273de36e1aa6b2105c304c551aaf34315c`
- Collection: `2026-09-16T06:09:29Z` to `2026-09-16T06:09:45Z` on the same Apple M2 Max used by the existing calibration.

[Raw rows](results/prefix-tiles/results.csv), [process summaries](results/prefix-tiles/summary.json) and [frozen source/artifact hashes](results/prefix-tiles/metadata.json) retain the measured values. The old 50-case rule has not been fitted to these 12 cases and does not identify this executable. Default CPU fallback on identity mismatch still applies.
