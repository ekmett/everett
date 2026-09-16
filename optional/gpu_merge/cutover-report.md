Header-based CPU/GPU cutover
============================

I calibrated the final Everett-named optimized merge on an Apple M2 Max. The frozen rule selects GPU when the **two inputs contain more than 51,200 records in total**, provided every header feature is inside the measured envelope and the device, backend and execution fingerprint match. Otherwise it selects CPU.

The threshold is a fitted split between sampled workloads, not a measured sharp crossover. Although this training set chose record count for its single decision, compressed payload bytes, file bytes, byte sizes per record, size imbalance and value-width metadata still constrain the admitted envelope. No record walk, key reconstruction or EF selection is needed to choose a backend.

The rule selected five of sixteen held-out cases. All five passed the predeclared margin: even the slowest GPU repetition was more than 10% faster than the fastest CPU repetition. Their median speedups range from **2.03× to 5.79×**. One additional robust GPU win was conservatively left on CPU. This is evidence for this workload sample, not a guarantee for every input inside the numeric envelope.

Frozen method and identity
--------------------------

- Source revision: `6c1bbd2e1e1bb364389dd9f2b6977caf02a92527`; the final renamed optional build passed all 13 CTests before collection.
- GPU/backend: Apple M2 Max / Metal; CPU Apple M2 Max; macOS 26.6.2. The observed GPU registry token is retained in the calibration identity and is not assumed stable across boots.
- Host SHA-256: `8206e2fd4a49e91438a752267177ddb8c26b599a52dc4d62eb8f25a5c4e827b2`.
- Shader-library SHA-256: `1af9ecde98985c57d861be3a6b261fdcf34269e2093131f6986a758ae6574763`.
- Fifty checked processes in fixed seeded shuffled order: 34 training and 16 held-out cases, one verified warm-up plus three alternating CPU/GPU measured pairs per case. All 150 measured pairs and 50 warm-up pairs passed complete canonical file equality, semantic scanning and CRC checks. All processes exited successfully; artifact hashes were unchanged after collection.
- The split, maximum tree depth, minimum GPU-leaf population and 10% margin were fixed before measurement. The training-only rule was written and hashed before held-out scoring. Held-out results did not change a threshold or prune a branch.
- Both complete totals start with existing warm compressed mapped inputs and finish with checksummed mapped output. They include resource allocation, GPU parse/tree/cache/order/count/scan/emit/output-EF work, host waits, framing and CRC. They exclude fixture generation, pipeline setup, correctness verification, final local-object destruction, fsync and catalog publication. CPU uses the existing serial compressed merger and canonical encoder. These are construction measurements, not durable-write throughput.

The generated header's decisions match the Python model on all 50 cases in a strict C++20 compile/run; all 50 wrong-identity checks select CPU. An unmatched binary, driver/OS fingerprint, GPU or backend, failed calibration validation, or features outside the training envelope selects CPU. The caller must first establish the supported sort/profile and immutable input ownership; this header-only selector does not validate a schema. The execution backend retains its independent grammar and resource checks.

Measured envelope
------------------

All sums below combine the two inputs. Imbalance is larger divided by smaller. Fixed-value bytes describe the encoded wire width, including framing; zero means unavailable for that feature. Terminal key bytes describe only the last key, not a maximum over all keys. Exact bounds are retained in the JSON model.

| Header feature | Minimum | Maximum |
|---|---:|---:|
| records | 512 | 524288 |
| payload_bytes | 8031 | 126940002 |
| file_bytes | 8760 | 127016152 |
| payload_bytes_per_record | 10.8804702759 | 516.437887753 |
| record_imbalance | 1 | 16 |
| byte_imbalance | 1 | 15.9983369639 |
| terminal_key_bytes | 8 | 264 |
| fixed_value_bytes | 0 | 514.5 |
| known_value_widths | 0 | 2 |

All 50 case summaries
---------------------

Times are milliseconds, median [minimum–maximum] over three measured pairs. N and ratio describe the larger input and larger/smaller record counts. The decision uses combined header counts and the full envelope, not the case labels. “Robust” is the predeclared slowest-GPU versus fastest-CPU margin.

| Case | Split | N | Ratio | Prefix bytes | Ties % | Value bytes / fixed | CPU ms | GPU ms | CPU/GPU | Choose GPU | Robust |
|---:|---|---:|---:|---:|---:|---|---:|---:|---:|:---:|:---:|
| 0 | train | 256 | 1 | 32 | 50 | 8 / no | 0.307 [0.282–0.392] | 5.041 [2.648–5.206] | 0.061× | False | False |
| 1 | train | 1024 | 1 | 32 | 50 | 8 / no | 0.643 [0.574–0.663] | 4.341 [2.819–5.367] | 0.148× | False | False |
| 2 | train | 4096 | 1 | 32 | 50 | 8 / no | 1.882 [1.843–1.891] | 2.515 [2.470–2.937] | 0.748× | False | False |
| 3 | train | 16384 | 1 | 32 | 50 | 8 / no | 6.941 [6.852–7.050] | 6.384 [4.133–6.402] | 1.087× | False | False |
| 4 | train | 65536 | 1 | 32 | 50 | 8 / no | 27.819 [27.392–28.460] | 8.036 [7.931–8.620] | 3.462× | True | True |
| 5 | train | 262144 | 1 | 32 | 50 | 8 / no | 109.157 [109.020–110.682] | 10.697 [8.592–12.346] | 10.204× | True | True |
| 6 | train | 4096 | 4 | 32 | 50 | 8 / no | 1.257 [1.254–1.345] | 3.198 [3.033–3.444] | 0.393× | False | False |
| 7 | train | 4096 | 16 | 32 | 50 | 8 / no | 1.168 [1.105–1.197] | 3.554 [3.261–3.875] | 0.329× | False | False |
| 8 | train | 4096 | 1 | 0 | 50 | 8 / no | 1.757 [1.744–1.806] | 4.181 [2.747–4.330] | 0.420× | False | False |
| 9 | train | 4096 | 1 | 256 | 50 | 8 / no | 1.814 [1.790–1.882] | 4.370 [4.078–6.758] | 0.415× | False | False |
| 10 | train | 4096 | 1 | 32 | 0 | 8 / no | 2.151 [2.118–2.276] | 4.940 [4.262–4.954] | 0.435× | False | False |
| 11 | train | 4096 | 1 | 32 | 100 | 8 / no | 1.477 [1.453–1.531] | 6.225 [5.845–6.284] | 0.237× | False | False |
| 12 | train | 4096 | 1 | 32 | 50 | 64 / no | 2.082 [1.961–2.244] | 5.033 [4.249–5.909] | 0.414× | False | False |
| 13 | train | 4096 | 1 | 32 | 50 | 512 / no | 3.051 [3.021–3.176] | 4.603 [3.978–4.756] | 0.663× | False | False |
| 14 | train | 4096 | 1 | 32 | 50 | 8 / yes | 1.667 [1.623–1.786] | 4.690 [3.990–5.818] | 0.356× | False | False |
| 15 | train | 65536 | 4 | 32 | 50 | 8 / no | 18.284 [17.839–18.813] | 5.407 [2.863–7.814] | 3.382× | True | True |
| 16 | train | 65536 | 16 | 32 | 50 | 8 / no | 15.943 [15.639–15.966] | 6.336 [4.008–7.195] | 2.516× | True | True |
| 17 | train | 65536 | 1 | 0 | 50 | 8 / no | 27.097 [26.799–27.815] | 6.955 [4.949–7.548] | 3.896× | True | True |
| 18 | train | 65536 | 1 | 256 | 50 | 8 / no | 27.832 [27.308–27.940] | 8.959 [6.876–9.683] | 3.107× | True | True |
| 19 | train | 65536 | 1 | 32 | 0 | 8 / no | 32.398 [31.573–32.548] | 7.206 [4.395–8.051] | 4.496× | True | True |
| 20 | train | 65536 | 1 | 32 | 100 | 8 / no | 20.173 [20.037–20.275] | 5.603 [5.322–6.137] | 3.600× | True | True |
| 21 | train | 65536 | 1 | 32 | 50 | 64 / no | 28.946 [28.682–32.891] | 6.290 [5.301–8.877] | 4.602× | True | True |
| 22 | train | 65536 | 1 | 32 | 50 | 512 / no | 45.242 [44.914–46.821] | 12.407 [12.352–12.761] | 3.646× | True | True |
| 23 | train | 65536 | 1 | 32 | 50 | 8 / yes | 24.345 [24.213–24.498] | 7.814 [5.060–8.486] | 3.116× | True | True |
| 24 | train | 262144 | 4 | 32 | 50 | 8 / no | 73.592 [73.295–73.678] | 6.913 [5.640–10.522] | 10.645× | True | True |
| 25 | train | 262144 | 16 | 32 | 50 | 8 / no | 64.261 [63.748–64.287] | 9.821 [7.559–9.967] | 6.543× | True | True |
| 26 | train | 262144 | 1 | 0 | 50 | 8 / no | 110.462 [109.722–111.339] | 10.723 [8.438–11.027] | 10.301× | True | True |
| 27 | train | 262144 | 1 | 256 | 50 | 8 / no | 110.437 [109.896–112.393] | 12.968 [11.494–16.395] | 8.516× | True | True |
| 28 | train | 262144 | 1 | 32 | 0 | 8 / no | 131.703 [131.034–132.256] | 8.803 [8.775–12.564] | 14.961× | True | True |
| 29 | train | 262144 | 1 | 32 | 100 | 8 / no | 82.313 [81.162–84.032] | 7.257 [6.473–7.911] | 11.343× | True | True |
| 30 | train | 262144 | 1 | 32 | 50 | 64 / no | 118.719 [116.965–119.226] | 12.105 [10.581–19.128] | 9.807× | True | True |
| 31 | train | 262144 | 1 | 32 | 50 | 256 / no | 145.438 [144.048–145.781] | 21.340 [20.415–30.980] | 6.815× | True | True |
| 32 | train | 262144 | 1 | 32 | 50 | 8 / yes | 98.151 [97.452–99.088] | 8.370 [7.597–11.426] | 11.726× | True | True |
| 33 | train | 65536 | 16 | 256 | 0 | 512 / yes | 28.275 [27.462–29.576] | 19.946 [16.502–20.227] | 1.418× | True | True |
| 34 | heldout | 2048 | 1 | 64 | 25 | 32 / no | 1.173 [1.079–1.216] | 6.052 [5.458–6.637] | 0.194× | False | False |
| 35 | heldout | 2048 | 3 | 128 | 75 | 128 / yes | 0.708 [0.686–0.768] | 4.376 [3.473–5.666] | 0.162× | False | False |
| 36 | heldout | 2048 | 8 | 0 | 0 | 512 / no | 1.158 [1.125–1.426] | 2.684 [2.541–3.267] | 0.431× | False | False |
| 37 | heldout | 2048 | 16 | 256 | 100 | 8 / yes | 0.588 [0.581–0.641] | 5.643 [4.977–5.872] | 0.104× | False | False |
| 38 | heldout | 8192 | 1 | 64 | 25 | 32 / no | 3.989 [3.792–4.054] | 6.288 [4.762–6.327] | 0.634× | False | False |
| 39 | heldout | 8192 | 3 | 128 | 75 | 128 / yes | 2.700 [2.495–2.832] | 5.355 [3.842–5.946] | 0.504× | False | False |
| 40 | heldout | 8192 | 8 | 0 | 0 | 512 / no | 4.328 [4.253–4.361] | 4.605 [4.301–4.847] | 0.940× | False | False |
| 41 | heldout | 8192 | 16 | 256 | 100 | 8 / yes | 1.911 [1.871–2.151] | 5.772 [5.442–6.379] | 0.331× | False | False |
| 42 | heldout | 32768 | 1 | 64 | 25 | 32 / no | 15.653 [15.646–15.728] | 7.692 [4.124–8.381] | 2.035× | True | True |
| 43 | heldout | 32768 | 3 | 128 | 75 | 128 / yes | 10.242 [9.830–10.484] | 8.945 [5.811–11.288] | 1.145× | False | False |
| 44 | heldout | 32768 | 8 | 0 | 0 | 512 / no | 16.191 [15.597–17.908] | 8.999 [6.509–9.051] | 1.799× | False | True |
| 45 | heldout | 32768 | 16 | 256 | 100 | 8 / yes | 6.769 [6.499–6.800] | 7.188 [6.497–7.788] | 0.942× | False | False |
| 46 | heldout | 131072 | 1 | 64 | 25 | 32 / no | 63.666 [62.399–64.479] | 11.001 [8.795–12.840] | 5.787× | True | True |
| 47 | heldout | 131072 | 3 | 128 | 75 | 128 / yes | 39.707 [37.944–40.386] | 11.770 [10.123–12.623] | 3.374× | True | True |
| 48 | heldout | 131072 | 8 | 0 | 0 | 512 / no | 61.858 [61.639–66.110] | 17.417 [15.845–19.251] | 3.552× | True | True |
| 49 | heldout | 131072 | 16 | 256 | 100 | 8 / yes | 26.997 [26.574–27.713] | 7.214 [5.907–13.484] | 3.742× | True | True |

Artifacts and use
-----------------

The optional [selector](cutover.h) is header-only. The generated [calibration header](results/cutover/calibration.h) records this exact identity, envelope and tree. It is a conservative construction-backend hint; it does not install a GPU backend into the durable runtime. Include the optional directory in the compiler include path when using the generated header.

[All raw trial rows](results/cutover/all-trials.csv), [all case summaries](results/cutover/cases.json), [the frozen training rule](results/cutover/training-rule.json), [the exact calibration](results/cutover/calibration.json), and the per-case raw CSVs remain available. [Qualification](results/cutover/qualification.json) records the new named build separately from [original public normalization](results/public_export.json). [Provenance](results/cutover/provenance.json) preserves original and public metadata hashes; public metadata omits local paths and hostname, while timing rows, artifact fingerprints and model values are unchanged.
