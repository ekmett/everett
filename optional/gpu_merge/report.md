Optional GPU merge: direct compressed input and output
======================================================

I keep this experiment in an optional directory. It leaves Everett's default header-only library unchanged and is not connected to the COLA scheduler, catalog or durable publication.

The current path imports existing compressed KV03 inputs without copying their records or reconstructing their keys on the CPU. GPU kernels select the input block offsets, parse frames, resolve inherited prefixes, merge, count exact output bits, scan offsets and emit the final packed payload and Elias–Fano sections into a mapped output. The CPU supplies constant-size metadata, allocates resources, submits work and writes the final directory/header and CRC.

On this Apple M2 Max, these complete-path measurements favor the GPU for the larger tested cases; the 16,384-record case has overlapping CPU/GPU ranges. Three exploratory trials per case do not establish a universal crossover or a production speedup.

Complete path (v5)
-----------------

Times are milliseconds, median [minimum–maximum]. N is the larger input count; ratio 1 means two N-record inputs. The keys contain an eight-byte ordered identifier after the stated common prefix. Values vary from 8 to 20 bytes and include tombstones. Trials alternate CPU/GPU order, with the unavoidable 2:1 split. A 4,096-record warm-up remains in the raw log and is excluded here.

| N | Prefix bytes | Nominal ties | Ratio | CPU complete output | GPU complete output | CPU/GPU median |
|---:|---:|---:|---:|---:|---:|---:|
| 16,384 | 32 | 50% | 1:1 | 6.752 [6.689–6.987] | 6.592 [4.131–7.283] | 1.02× |
| 65,536 | 32 | 50% | 1:1 | 27.071 [26.994–27.221] | 8.035 [4.196–8.239] | 3.37× |
| 262,144 | 32 | 50% | 1:1 | 109.339 [109.229–109.440] | 7.257 [6.778–9.690] | 15.07× |
| 65,536 | 0 | 50% | 1:1 | 26.997 [26.965–27.223] | 5.034 [4.795–5.829] | 5.36× |
| 65,536 | 256 | 50% | 1:1 | 27.633 [27.155–27.865] | 10.426 [8.123–13.271] | 2.65× |
| 65,536 | 32 | 0% | 16:1 | 16.153 [15.927–16.166] | 5.473 [4.001–7.283] | 2.95× |
| 65,536 | 32 | 100% | 1:1 | 20.087 [19.874–20.311] | 5.305 [4.266–7.224] | 3.79× |

The observed CPU/GPU ranges overlap at 16,384 records and are disjoint in the GPU's favor in the six larger v5 cases. GPU command and wall times vary substantially. I retain all trials and make no confidence-interval claim.

Both totals start from existing warm mapped KV03 inputs and end with a complete checksummed scratch file. The CPU uses Everett's existing single-thread `sort_profile_merge_builder`, canonical section encoder and mapped-output copy. GPU totals include metadata inspection and imports, all scratch/output allocation, GPU input parsing and prefix tree, eight-byte cache construction, ordering and duplicate compaction, exact sizing/scans, direct packed output and EF construction, host waits, directory construction and CRC. Neither total includes final local-object destruction, fixture generation, shader/pipeline compilation, correctness verification, fsync or catalog publication. The CPU baseline is the existing implementation, not a newly optimized parallel CPU merger.

| N / prefix / ties / ratio | Input import, GPU parse/tree | Scratch prepare | Cache, order and compact | Size and scan | Packed payload + EF emit | EF plan, envelope, CRC | GPU command time |
|---|---:|---:|---:|---:|---:|---:|---:|
| 16384 / 32 / 50 / 1 | 1.428 | 0.037 | 1.698 | 0.314 | 0.366 | 0.503 | 3.781 |
| 65536 / 32 / 50 / 1 | 1.209 | 0.042 | 1.698 | 0.614 | 0.701 | 0.818 | 4.309 |
| 262144 / 32 / 50 / 1 | 2.062 | 0.050 | 2.808 | 0.549 | 1.070 | 0.694 | 3.982 |
| 65536 / 0 / 50 / 1 | 1.669 | 0.044 | 1.310 | 0.663 | 0.468 | 0.647 | 2.254 |
| 65536 / 256 / 50 / 1 | 1.197 | 0.039 | 6.206 | 0.363 | 0.508 | 0.494 | 7.264 |
| 65536 / 32 / 0 / 16 | 1.346 | 0.036 | 1.672 | 0.390 | 0.411 | 0.606 | 3.100 |
| 65536 / 32 / 100 / 1 | 1.430 | 0.037 | 1.754 | 0.714 | 0.545 | 0.430 | 2.417 |

Stage medians need not sum to the total median. GPU command time is already a component of the host stages. The historical raw column `decode_ms` means CPU reconstruction in v1/v2, but metadata/import plus GPU parse/tree in v3/v4/v5; it is not a CPU decoding pass in the current path. The assembly phase includes the GPU EF sparse-plan scan and its wait as well as host framing and CRC.

Measured progression
--------------------

These are GPU complete-path medians from separate runs, not controlled attribution of each optimization. Each version used the same fixture parameters and passed the same complete-file byte oracle; the expanded adversarial corpus applies to v3 onward.

| N / prefix / ties / ratio | v1 CPU reconstruction | v2 + common-prefix skip | v3 GPU input parsing | v4 + GPU output EF | v5 + cache/word emit |
|---|---:|---:|---:|---:|---:|
| 16384 / 32 / 50 / 1 | 7.544 | 5.829 | 8.386 | 7.104 | 6.592 |
| 65536 / 32 / 50 / 1 | 27.162 | 19.831 | 9.212 | 9.130 | 8.035 |
| 262144 / 32 / 50 / 1 | 85.912 | 73.788 | 20.398 | 21.244 | 7.257 |
| 65536 / 0 / 50 / 1 | 17.262 | 17.423 | 7.561 | 14.098 | 5.034 |
| 65536 / 256 / 50 / 1 | 42.259 | 26.644 | 15.029 | 14.117 | 10.426 |
| 65536 / 32 / 0 / 16 | 11.274 | 10.884 | 10.127 | 8.143 | 5.473 |
| 65536 / 32 / 100 / 1 | 21.252 | 18.639 | 7.248 | 11.330 | 5.305 |

The early CPU-reconstructed path was a staging baseline, not the direct-mapped algorithm. Moving its serial reconstruction onto the GPU materially changes the larger-case results. The output-EF step eliminates another host loop, but these short measurements do not demonstrate a consistent latency improvement from that step alone. V5 caches eight bytes after the GPU-proved common prefix, then uses word comparisons with exact length handling and lazy fallback. Its emitter gathers grammar/literal/value segments a word at a time. V5 also removes a redundant host memset over the freshly truncated output mapping; unwritten alignment gaps remain zero. These are whole-version results, not separate attribution to the cache, emitter or zeroing change.

Correctness and boundaries
--------------------------

- All 21 measured pairs per version match the complete canonical CPU KV03 file byte for byte, including envelope, payload, EF and padding; outputs pass semantic scanning and CRC validation.
- The current correctness mode checks 81 deterministic adversarial cases against an independent chronological `std::map` oracle: empty/binary/proper-prefix keys, sub-byte LCPs, retention ramps, block/tree boundaries, skewed sizes, duplicate ties, fixed value widths, large binary values across word boundaries and newer tombstones. It independently checks inherited-bit ownership and the direct-source-suffix lemma.
- The self-contained build passed three GPU CTests. The optimized corpus also passed with a strict O2 AddressSanitizer/UndefinedBehaviorSanitizer host; these sanitizers instrument CPU code, not GPU memory accesses.
- GPU-parsed descriptors match CPU frame parsing in correctness mode, and a GPU checksum over every reconstructed key byte matches the independent CPU cursor. Neither oracle runs inside benchmark totals. The cached bytes have an independent CPU oracle, and the segment emitter has an independent per-bit extraction probe.
- Input EF probes cover dense and sparse samples, low widths up to 30, fields crossing 32/64-bit words, and corrupt metadata. Ten output-EF fixtures check exact CPU section bytes and guards, including the 4095/4096 sparse threshold, fixed stride and partial EOF.
- Every distinct key remains, including newer tombstones. This is replacement merge, not whole-history deletion cleanup. Inputs are trusted, already validated strictly sorted runs; malformed-case checks are bounded hardening, not a general hostile-file validation API.
- A GPU min tree finds inherited literal ownership. The global common prefix is proved on the GPU from input extrema. The output LCP cannot be smaller than the winner's retained source prefix, so its final suffix can be copied directly from the original mapped literal.
- Conservative bounds precede 32-bit output scans. Combined input count is below 2^24 and worst-case output below 2^31 bits. Each final 32-bit payload/EF word has one writer; no racing shared-word OR is used. This GPU entry rejects double-empty input.
- Eligibility is deliberately narrow: the explicit `string_policy` one-bit sort selector, string keys, optional string values, replacement composition, EG0 and W=15. Arbitrary C++ handlers and complete fractional-cascading index construction are not implemented on the GPU.
- Actual file-backed `MAP_SHARED` input/output and Everett's read-only input mappings imported through Metal `bytesNoCopy` on this machine: 16,384-byte pages, reported maxBufferLength 62,620,631,040 bytes, unified memory. This is local evidence, not a portable import guarantee. Owners remain alive through command completion; CPU result access waits for completion.

Shared-input rank construction
------------------------------

These separate measurements begin with warm, existing mapped inputs and preallocated GPU output/scan buffers. They include command encoding, dispatch and completion waiting. The reused CPU paths likewise have preallocated outputs. Rank2048 uses the existing NEON/popcount512 directory algorithm; the owning builder additionally copies its bitmap and allocates outputs. Rank15 starts from already-computed class bytes, not a full origin bitmap; its CPU owning/reused loops validate and pack those classes. The GPU consumes trusted valid classes. The table uses five trials; every output matches the existing CPU serialized sections.

| Virtual bits | Input / output | CPU owning median | CPU reused median [range] | GPU reused median [range] | GPU command median |
|---:|---|---:|---:|---:|---:|
| 1,048,576 | bitmap2048 | 0.006 | 0.002 [0.002–0.002] | 0.179 [0.176–0.682] | 0.031 |
| 1,048,576 | class15 | 0.129 | 0.116 [0.113–0.121] | 0.216 [0.202–0.300] | 0.044 |
| 16,777,216 | bitmap2048 | 0.075 | 0.032 [0.026–0.039] | 0.244 [0.220–1.664] | 0.062 |
| 16,777,216 | class15 | 1.984 | 1.954 [1.889–2.017] | 0.675 [0.298–2.270] | 0.108 |
| 268,435,456 | bitmap2048 | 1.410 | 0.643 [0.603–0.799] | 0.823 [0.646–2.029] | 0.545 |
| 268,435,456 | class15 | 31.984 | 30.037 [29.945–30.187] | 3.242 [2.180–4.418] | 1.740 |

At 2^28 virtual bits, class15 means 17,895,698 input class bytes. Its GPU median is 9.26× faster than the reused CPU loop, with disjoint observed ranges. The class15 ranges overlap at 2^24; the small case favors CPU. The bitmap2048 reused CPU median remains faster at every measured size, with overlapping ranges at 2^28. These are construction comparisons, not lookup or full-merge timings.

A separate GPU pass packs both final rank15 routes directly from an augmented index-origin stream. Its output matches 39 synthetic tails/patterns and four actual CPU-built COLA graph fixtures; an invalid origin is rejected. The host-generated origin stream is test setup, not a claimed GPU index merger. The [checked collision-ranking alternatives](collision-report.md) are separate from this final rank15 format, and neither changes the production representation. The earlier bitmap-to-rank pilot remains in the retained results for context.

Reproduction and provenance
---------------------------

`build.py` uses DXC HLSL2021 → validated SPIR-V → SPIRV-Cross MSL3.2 → Metal/metallib pipeline. Tool paths are arguments or environment/PATH settings. The C++20/Objective-C++ host is built at O3. With other CPU/GPU workloads stopped, run `prototype kernels.metallib scratch-directory optimized` for the current correctness corpus and `optimized-bench` for v5's sweep. `rank-shared` runs the fairer standalone construction comparison. `complete`/`complete-bench` retain the uncached algorithm. `compressed`/`compressed-bench` retain the v3 CPU-output-EF variant; `merge`/`merge-bench` retain the staging input path. See `design.md` for format and synchronization details. A separate Windows Vulkan adapter has [correctness qualification](vulkan-qualification.md); it is not included in this package and has no performance qualification yet.

The `results` directory retains normalized timing/correctness logs, structured rows and SHA-256 source/binary manifests. `normalization.json` records both original and public log hashes; normalization removes only machine-local resource-wrapper log locations. Timing and correctness records are unchanged. Original raw logs, exact measured source snapshots and reconstruction patches remain in a private provenance archive. The source and binary manifests retain their original hashes; later source cleanup is not presented as the measured source. This is a short sample from one machine, not a cross-device result or a durability-throughput measurement.

The later public naming refresh uses Everett names and the current EVRT file signatures. [Public export hashes](results/public_export.json) identify both the frozen originals and these normalized copies. Existing measurement hashes retain their original meaning; the naming refresh is not a new performance measurement.
