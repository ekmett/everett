Matched CPU bit and byte native merges
======================================

The candidate typed-byte path has lower complete-path medians in 11/12 cases. Baseline/candidate ratios span 0.996–1.184×; 8 process-median ranges are disjoint in its favor. The raw-bit and typed-bit controls are mixed, so these results do not establish a general EF-cursor speedup.

The frozen comparison is `5868b32afae9e78cd86a71289ca119670d6c3748` → `a760695c23c41deb4b8b64c2d7024e9919737b9a`. The candidate combines forward EF navigation, byte-writer changes, and the built-in byte-string transport. The later equal-key donor improvement is outside this measurement, whose inputs have no equal keys or cancellations.

All modes represent the same sixteen-byte logical keys and present values. The typed paths are real, distinct transports: bit KV03 versus byte KV02. Raw KV02 controls remove typed framing and keep exactly the same raw key/value bytes. This is native merge throughput, not a complete transaction or indexed-world benchmark.

Method and spread
-----------------

The collection contains 288 fresh processes, 864 timed iterations and 288 excluded warmups. Each case/mode/revision has three fresh processes with three measured iterations each. Tables show milliseconds: the median of those three process medians, followed by their minimum–maximum. Revision order alternates for each case/mode across rounds, with the unavoidable 2:1 first-slot split; mode and case order rotate. All trials are retained.

The complete timer includes merge setup on existing mapped inputs, native output allocation/encoding, EF construction, a fresh output mapping and direct section copy, body/header CRC32C, clipping and all return-time cleanup. Inputs are encoded, mapped and validated before timing. Fixture construction, sorted-record oracles and output validation are excluded. There is no `fsync`, `msync`, catalog publication or fractional-index construction. Inner phase diagnostics in the raw data omit some final destruction; the outer complete timer includes it.

| Mode | Lower candidate medians | Baseline/candidate ratio range | Process ranges: faster / slower / overlap | All-trial ranges: faster / slower / overlap |
|---|---:|---:|---:|---:|
| typed-byte | 11/12 | 0.996–1.184 | 8 / 0 / 4 | 5 / 0 / 7 |
| raw-byte | 11/12 | 0.996–1.180 | 8 / 0 / 4 | 3 / 0 / 9 |
| raw-bit | 6/12 | 0.977–1.092 | 1 / 1 / 10 | 0 / 0 / 12 |
| typed-bit | 7/12 | 0.969–1.068 | 0 / 1 / 11 | 0 / 0 / 12 |

typed-byte complete latency
---------------------------

| Keys | Values | Records/input | Baseline ms | Candidate ms | Baseline/candidate | Process ranges |
|---|---|---:|---:|---:|---:|---|
| structured | FF16 | 4,096 | 0.731 [0.728–0.799] | 0.620 [0.598–0.708] | 1.178× | candidate-faster |
| structured | FF16 | 65,536 | 8.471 [8.360–8.599] | 7.235 [7.158–7.361] | 1.171× | candidate-faster |
| structured | FF16 | 262,144 | 33.348 [33.333–33.412] | 28.154 [27.926–28.433] | 1.184× | candidate-faster |
| structured | FV 0–512 | 4,096 | 1.283 [1.156–1.293] | 1.199 [1.157–1.237] | 1.070× | overlap |
| structured | FV 0–512 | 65,536 | 18.276 [18.199–18.498] | 17.115 [16.731–18.941] | 1.068× | overlap |
| structured | FV 0–512 | 131,072 | 38.946 [37.047–41.873] | 34.646 [33.590–34.990] | 1.124× | candidate-faster |
| hash-like | FF16 | 4,096 | 0.860 [0.815–0.877] | 0.790 [0.780–0.833] | 1.088× | overlap |
| hash-like | FF16 | 65,536 | 10.530 [10.456–10.660] | 9.265 [9.172–9.270] | 1.137× | candidate-faster |
| hash-like | FF16 | 262,144 | 42.165 [41.038–42.424] | 37.028 [36.851–37.051] | 1.139× | candidate-faster |
| hash-like | FV 0–512 | 4,096 | 1.387 [1.317–1.589] | 1.393 [1.375–1.398] | 0.996× | overlap |
| hash-like | FV 0–512 | 65,536 | 21.429 [21.104–21.958] | 20.368 [20.151–20.934] | 1.052× | candidate-faster |
| hash-like | FV 0–512 | 131,072 | 43.660 [42.626–43.718] | 41.485 [40.951–42.144] | 1.052× | candidate-faster |

raw-byte complete latency
-------------------------

| Keys | Values | Records/input | Baseline ms | Candidate ms | Baseline/candidate | Process ranges |
|---|---|---:|---:|---:|---:|---|
| structured | FF16 | 4,096 | 0.669 [0.661–0.711] | 0.634 [0.607–0.636] | 1.055× | candidate-faster |
| structured | FF16 | 65,536 | 7.314 [7.296–7.346] | 6.601 [6.562–6.682] | 1.108× | candidate-faster |
| structured | FF16 | 262,144 | 29.114 [29.045–29.579] | 26.276 [26.070–26.614] | 1.108× | candidate-faster |
| structured | FV 0–512 | 4,096 | 1.234 [1.200–1.257] | 1.145 [1.109–1.178] | 1.077× | candidate-faster |
| structured | FV 0–512 | 65,536 | 18.335 [17.670–18.732] | 16.870 [16.397–16.895] | 1.087× | candidate-faster |
| structured | FV 0–512 | 131,072 | 40.112 [36.627–41.203] | 34.544 [34.136–37.314] | 1.161× | overlap |
| hash-like | FF16 | 4,096 | 0.855 [0.830–0.922] | 0.758 [0.729–0.849] | 1.127× | overlap |
| hash-like | FF16 | 65,536 | 10.410 [10.387–10.603] | 8.819 [8.660–8.830] | 1.180× | candidate-faster |
| hash-like | FF16 | 262,144 | 41.202 [40.554–41.639] | 35.475 [34.837–35.649] | 1.161× | candidate-faster |
| hash-like | FV 0–512 | 4,096 | 1.447 [1.380–1.489] | 1.275 [1.273–1.424] | 1.135× | overlap |
| hash-like | FV 0–512 | 65,536 | 20.908 [20.552–21.081] | 20.994 [20.086–21.205] | 0.996× | overlap |
| hash-like | FV 0–512 | 131,072 | 44.346 [42.732–48.234] | 41.372 [41.292–42.346] | 1.072× | candidate-faster |

raw-bit complete latency
------------------------

| Keys | Values | Records/input | Baseline ms | Candidate ms | Baseline/candidate | Process ranges |
|---|---|---:|---:|---:|---:|---|
| structured | FF16 | 4,096 | 0.999 [0.997–1.077] | 1.009 [0.972–1.026] | 0.990× | overlap |
| structured | FF16 | 65,536 | 12.100 [11.845–12.217] | 12.389 [12.289–12.463] | 0.977× | candidate-slower |
| structured | FF16 | 262,144 | 48.775 [47.856–49.084] | 48.933 [48.905–49.385] | 0.997× | overlap |
| structured | FV 0–512 | 4,096 | 2.099 [1.817–2.178] | 1.923 [1.854–1.955] | 1.092× | overlap |
| structured | FV 0–512 | 65,536 | 28.923 [28.540–31.710] | 28.952 [28.614–30.097] | 0.999× | overlap |
| structured | FV 0–512 | 131,072 | 59.407 [58.502–66.444] | 58.586 [58.174–65.307] | 1.014× | overlap |
| hash-like | FF16 | 4,096 | 1.347 [1.305–1.410] | 1.358 [1.318–1.400] | 0.992× | overlap |
| hash-like | FF16 | 65,536 | 18.545 [18.206–18.660] | 18.307 [18.267–18.414] | 1.013× | overlap |
| hash-like | FF16 | 262,144 | 73.568 [73.154–74.881] | 73.700 [72.123–74.440] | 0.998× | overlap |
| hash-like | FV 0–512 | 4,096 | 2.197 [2.182–2.240] | 2.175 [2.158–2.182] | 1.010× | candidate-faster |
| hash-like | FV 0–512 | 65,536 | 33.939 [33.585–34.993] | 33.338 [32.947–33.762] | 1.018× | overlap |
| hash-like | FV 0–512 | 131,072 | 68.475 [68.144–79.366] | 67.982 [67.945–68.511] | 1.007× | overlap |

typed-bit complete latency
--------------------------

| Keys | Values | Records/input | Baseline ms | Candidate ms | Baseline/candidate | Process ranges |
|---|---|---:|---:|---:|---:|---|
| structured | FF16 | 4,096 | 2.052 [1.959–2.140] | 2.019 [2.011–2.043] | 1.016× | overlap |
| structured | FF16 | 65,536 | 30.238 [29.250–30.255] | 29.844 [29.650–30.682] | 1.013× | overlap |
| structured | FF16 | 262,144 | 121.347 [121.006–121.421] | 122.144 [121.723–122.500] | 0.993× | candidate-slower |
| structured | FV 0–512 | 4,096 | 3.163 [2.898–3.280] | 2.962 [2.905–3.083] | 1.068× | overlap |
| structured | FV 0–512 | 65,536 | 47.301 [47.166–47.334] | 48.116 [45.911–49.447] | 0.983× | overlap |
| structured | FV 0–512 | 131,072 | 96.031 [95.716–99.223] | 94.728 [94.510–100.304] | 1.014× | overlap |
| hash-like | FF16 | 4,096 | 2.277 [2.222–2.332] | 2.304 [2.280–2.330] | 0.988× | overlap |
| hash-like | FF16 | 65,536 | 34.651 [34.419–35.218] | 34.231 [33.783–35.390] | 1.012× | overlap |
| hash-like | FF16 | 262,144 | 143.341 [140.661–146.616] | 140.463 [139.848–142.466] | 1.020× | overlap |
| hash-like | FV 0–512 | 4,096 | 3.224 [3.163–3.317] | 3.327 [3.199–3.399] | 0.969× | overlap |
| hash-like | FV 0–512 | 65,536 | 52.063 [50.956–52.228] | 53.107 [51.714–53.255] | 0.980× | overlap |
| hash-like | FV 0–512 | 131,072 | 105.415 [104.689–105.946] | 105.089 [104.558–106.312] | 1.003× | overlap |

Control interpretation
----------------------

The raw-bit structured FF16 case at 65,536/input has a 2.39% higher candidate median with disjoint process-median ranges. Its all-trial range classification is `overlap`.

The typed-bit structured FF16 case at 262,144/input has a 0.66% higher candidate median with disjoint process-median ranges. Its all-trial range classification is `overlap`.

The raw-bit results are a useful control for the generic profile navigation change. The unchanged typed-bit path also varies. The small regressions have not been causally isolated. Source inspection shows the forward EF cursor copies its complete view/state only at codec boundaries (once per fifteen records here), then commits that copy after parsing succeeds; it does not copy the whole cursor on every record. That is a possible follow-up cost to investigate, not a measured explanation. No additional experiment or selective rerun was used to remove these results.

Candidate bit versus byte
-------------------------

These cross-mode ratios use the same frozen candidate and logical records. They compare the actual complete formats, including their different framing and output sizes. They do not isolate one instruction or one wire-code feature.

| Keys | Values | Records/input | Typed bit ms | Typed byte ms | Typed bit / byte | Raw bit / byte |
|---|---|---:|---:|---:|---:|---:|
| structured | FF16 | 4,096 | 2.019 | 0.620 | 3.255× | 1.591× |
| structured | FF16 | 65,536 | 29.844 | 7.235 | 4.125× | 1.877× |
| structured | FF16 | 262,144 | 122.144 | 28.154 | 4.338× | 1.862× |
| structured | FV 0–512 | 4,096 | 2.962 | 1.199 | 2.471× | 1.679× |
| structured | FV 0–512 | 65,536 | 48.116 | 17.115 | 2.811× | 1.716× |
| structured | FV 0–512 | 131,072 | 94.728 | 34.646 | 2.734× | 1.696× |
| hash-like | FF16 | 4,096 | 2.304 | 0.790 | 2.915× | 1.792× |
| hash-like | FF16 | 65,536 | 34.231 | 9.265 | 3.695× | 2.076× |
| hash-like | FF16 | 262,144 | 140.463 | 37.028 | 3.793× | 2.078× |
| hash-like | FV 0–512 | 4,096 | 3.327 | 1.393 | 2.389× | 1.705× |
| hash-like | FV 0–512 | 65,536 | 53.107 | 20.368 | 2.607× | 1.588× |
| hash-like | FV 0–512 | 131,072 | 105.089 | 41.485 | 2.533× | 1.643× |

Exact encoded output sizes
--------------------------

Sizes are complete logical file bytes, including envelope, directory, EF, padding and checksums. Raw-bit, raw-byte and typed-bit files are byte-identical between revisions; only typed-byte wire changes. All input file sizes and hashes are also retained in the raw checks.

| Keys | Values | Records/input | Raw bit | Raw byte | Typed bit | Typed byte baseline | Typed byte candidate |
|---|---|---:|---:|---:|---:|---:|---:|
| structured | FF16 | 4,096 | 142,528 | 156,960 | 152,888 | 198,656 | 173,544 |
| structured | FF16 | 65,536 | 2,276,680 | 2,507,608 | 2,441,152 | 3,174,520 | 2,772,816 |
| structured | FF16 | 262,144 | 9,105,952 | 10,029,680 | 9,763,608 | 12,697,296 | 11,090,488 |
| structured | FV 0–512 | 4,096 | 2,130,192 | 2,137,376 | 2,125,240 | 2,174,584 | 2,145,584 |
| structured | FV 0–512 | 65,536 | 34,086,264 | 34,201,176 | 34,005,736 | 34,796,392 | 34,332,512 |
| structured | FV 0–512 | 131,072 | 68,171,576 | 68,401,400 | 68,010,440 | 69,591,824 | 68,664,072 |
| hash-like | FF16 | 4,096 | 277,376 | 271,048 | 287,712 | 312,592 | 287,512 |
| hash-like | FF16 | 65,536 | 4,370,152 | 4,262,496 | 4,534,352 | 4,926,640 | 4,525,832 |
| hash-like | FF16 | 262,144 | 17,350,048 | 16,880,552 | 18,005,776 | 19,536,120 | 17,933,976 |
| hash-like | FV 0–512 | 4,096 | 2,264,776 | 2,251,296 | 2,259,808 | 2,288,464 | 2,259,504 |
| hash-like | FV 0–512 | 65,536 | 36,175,696 | 35,953,440 | 36,094,904 | 36,547,840 | 36,084,776 |
| hash-like | FV 0–512 | 131,072 | 72,318,160 | 71,855,496 | 72,156,216 | 73,044,048 | 72,118,160 |

The new typed-byte transport reduces complete file size by 1.27–12.65% relative to the old typed-byte transport across these fixtures. Against typed-bit, the space tradeoff depends on the key distribution and value sizes:

- structured FF16: candidate typed-byte uses 13.511–13.590% more complete file space than typed-bit.
- structured FV 0–512: candidate typed-byte uses 0.957–0.961% more complete file space than typed-bit.
- hash-like FF16: candidate typed-byte uses 0.070–0.399% less complete file space than typed-bit.
- hash-like FV 0–512: candidate typed-byte uses 0.013–0.053% less complete file space than typed-bit.

The following ratios use complete output bytes for the same records; a ratio below one means the numerator occupies less space. Total file bits are exactly eight times the byte counts above. [The full space table](results/space.csv) also records those bit totals, logical key/value byte counts, and component sizes for all sixty distinct case/format/revision combinations.

| Keys | Values | Records/input | Candidate typed byte / typed bit bytes | Candidate / baseline typed byte bytes |
|---|---|---:|---:|---:|
| structured | FF16 | 4,096 | 1.135105 | 0.873591 |
| structured | FF16 | 65,536 | 1.135864 | 0.873460 |
| structured | FF16 | 262,144 | 1.135901 | 0.873453 |
| structured | FV 0–512 | 4,096 | 1.009573 | 0.986664 |
| structured | FV 0–512 | 65,536 | 1.009609 | 0.986669 |
| structured | FV 0–512 | 131,072 | 1.009611 | 0.986669 |
| hash-like | FF16 | 4,096 | 0.999305 | 0.919768 |
| hash-like | FF16 | 65,536 | 0.998121 | 0.918645 |
| hash-like | FF16 | 262,144 | 0.996012 | 0.917991 |
| hash-like | FV 0–512 | 4,096 | 0.999865 | 0.987345 |
| hash-like | FV 0–512 | 65,536 | 0.999719 | 0.987330 |
| hash-like | FV 0–512 | 131,072 | 0.999473 | 0.987324 |

Space components
----------------

These are exact physical section lengths from the retained file headers, with no new merge runs. The front-coded record stream includes key controls, key suffixes and values; it is not solely user payload. EF includes low/high words, select samples, sparse entries and their word padding. Other bytes comprise the envelope, directory, sort metadata where present, and section alignment. The three physical columns sum to the complete file size. The separate FC bit count excludes its final partial-byte padding.

The largest case for each key/value distribution is shown here: case 2 is structured FF16, case 5 structured FV, case 8 hash-like FF16, and case 11 hash-like FV. FF cases use 262,144/input and FV cases 131,072/input; the linked table contains every size.

| Case | Format | Revision | FC logical bits | FC storage bytes | EF bytes | Other bytes | Total bytes |
|---:|---|---|---:|---:|---:|---:|---:|
| 2 | raw-bit | candidate | 72,506,359 | 9,063,295 | 42,432 | 225 | 9,105,952 |
| 2 | raw-byte | candidate | 79,958,104 | 9,994,763 | 34,688 | 229 | 10,029,680 |
| 2 | typed-bit | candidate | 77,766,721 | 9,720,841 | 42,448 | 319 | 9,763,608 |
| 2 | typed-byte | baseline | 101,194,928 | 12,649,366 | 47,704 | 226 | 12,697,296 |
| 2 | typed-byte | candidate | 88,346,712 | 11,043,339 | 46,920 | 229 | 11,090,488 |
| 5 | raw-bit | candidate | 545,066,468 | 68,133,309 | 38,040 | 227 | 68,171,576 |
| 5 | raw-byte | candidate | 546,957,360 | 68,369,670 | 31,504 | 226 | 68,401,400 |
| 5 | typed-bit | candidate | 543,776,762 | 67,972,096 | 38,032 | 312 | 68,010,440 |
| 5 | typed-byte | baseline | 556,480,176 | 69,560,022 | 31,576 | 226 | 69,591,824 |
| 5 | typed-byte | candidate | 549,058,592 | 68,632,324 | 31,520 | 228 | 68,664,072 |
| 8 | raw-bit | candidate | 138,326,960 | 17,290,870 | 58,952 | 226 | 17,350,048 |
| 8 | raw-byte | candidate | 134,679,352 | 16,834,919 | 45,408 | 225 | 16,880,552 |
| 8 | typed-bit | candidate | 143,572,090 | 17,946,512 | 58,952 | 312 | 18,005,776 |
| 8 | typed-byte | baseline | 155,881,952 | 19,485,244 | 50,648 | 228 | 19,536,120 |
| 8 | typed-byte | candidate | 143,067,960 | 17,883,495 | 50,256 | 225 | 17,933,976 |
| 11 | raw-bit | candidate | 578,237,302 | 72,279,663 | 38,272 | 225 | 72,318,160 |
| 11 | raw-byte | candidate | 574,588,472 | 71,823,559 | 31,712 | 225 | 71,855,496 |
| 11 | typed-bit | candidate | 576,941,042 | 72,117,631 | 38,272 | 313 | 72,156,216 |
| 11 | typed-byte | baseline | 584,096,608 | 73,012,076 | 31,744 | 228 | 73,044,048 |
| 11 | typed-byte | candidate | 576,689,704 | 72,086,213 | 31,720 | 227 | 72,118,160 |

Validation and provenance
-------------------------

- All 288 processes passed exact whole-file equality with an independently ordered, full-record writer oracle after every iteration. Every output received a full CRC/semantic scan; warmup outputs and all inputs were independently decoded to the logical records.
- All canonical logical input/output SHA-256 values match across four modes, both revisions, all processes, and the earlier matched Metal fixtures. Only the typed-byte wire change is allowed across revisions.
- Independently checked 864 retained physical headers for native layout kind, record count, physical extent and header CRC32C. Full physical SHA-256 values are stable within each format/revision. Disposable full input/output files were removed after checks and header/hash capture.
- Both release builds passed sixteen serial CTests. The candidate also passed all sixteen strict O2 ASan/UBSan CTests. Only the O3 release binaries supplied the retained timings.
- The collector verified both immutable header revisions (82 files each), 5 harness/fixture sources, and both binary hashes before and after collection. Measured harness source: `40aecd43f21c8c4a16ebd3978a19042b61779d5f`.
- One preflight attempt stopped while reading sandbox-restricted CPU identity metadata, before any benchmark process. The identical frozen collector then ran with authorized read-only metadata access; no timed attempt was replaced.

Host: Apple M2 Max, macOS 26.6.2 (25G83). Collection date: 2026-09-16. C++20 with O3, NDEBUG and strict warnings. The existing fixture limit of 131,072/input for FV keeps the grid identical to the Metal comparison; it is not a CPU format-size limit.

See [raw rows](results/results.csv), [all summaries](results/summary.json), [source/artifact manifest](results/manifest.json), [logical/physical checks](results/checks.json), [schedule/host metadata](results/metadata.json), [correctness qualification](results/qualification.json), and [the method](README.md). Earlier Metal calibration and fixed-format measurements are unchanged.
