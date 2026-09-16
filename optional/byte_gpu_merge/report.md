Byte-profile Metal merge measurements
====================================

I measured complete native-file merges on an Apple M2 Max. At 131,072 records
per input, the GPU took about half the CPU time in both tested value-size
families. Small merges favored the CPU. The intermediate variable-value case
was close, and its trial ranges overlapped.

These are structured, common-prefix string fixtures. They establish an initial
size range worth pursuing, not a general GPU dispatch rule. There are no
hash-like, cold-cache, multi-sort or durable-publication measurements here.

Complete merge time
-------------------

Times are milliseconds. Each entry is the median of three process medians,
with five measured trials per process. The ratio is CPU time divided by GPU
time: values greater than one favor the GPU.

| Records per input | Values | Output records | Complete output bytes | CPU ms | GPU ms | CPU/GPU |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 4,096 | fixed 16 bytes | 6,839 | 137,568 | 0.635 | 3.005 | 0.21 |
| 32,768 | fixed 16 bytes | 54,712 | 1,098,640 | 3.146 | 3.447 | 0.91 |
| 131,072 | fixed 16 bytes | 218,881 | 4,394,392 | 11.908 | 5.934 | 2.01 |
| 4,096 | variable 0–512 bytes | 6,839 | 1,631,720 | 1.098 | 2.995 | 0.37 |
| 32,768 | variable 0–512 bytes | 54,712 | 13,050,112 | 7.129 | 6.583 | 1.08 |
| 131,072 | variable 0–512 bytes | 218,881 | 52,212,272 | 36.048 | 17.429 | 2.07 |

The large variable-value CPU result has substantial spread: its three process
medians were **56.465, 36.048 and 29.157 ms**, versus **17.505, 17.391 and
17.429 ms** for the GPU. Across every measured trial, the CPU ranged from
28.083 to 81.850 ms and the GPU from 16.996 to 18.060 ms. I retain all of those
observations; the table's 2.07 ratio does not describe every process.

The fixed-size 131K case was steadier: CPU process medians ranged from 11.896
to 11.938 ms, and GPU medians from 5.584 to 5.941 ms. At 32K, the variable-value
GPU result is only about 8% faster in the table, with overlapping trial ranges.
131K per input is the first tested size with a clear practical advantage in
both families; the sweep does not locate an exact crossover.

What is being compared
----------------------

Both paths produce the **same complete `KV02` file**, including its optional
value tags, retained tombstones, FC payload, native offset directory, select
samples and exceptions, padding, file header and checksums. Every trial checks
complete byte equality against an independent batch encoding and scans the GPU
file through the production reader. Final file hashes independently confirm
CPU, GPU and oracle identity in each of the eighteen process/case combinations.

The keys share a 24-byte prefix, including an embedded zero, followed by an
eight-byte big-endian identifier. About 33% of newer keys coincide with older
keys. Replacements retain the newer value; the variable family also includes
tombstones and present-empty strings. Both inputs are individually sorted and
unique. The exact generator is in [prototype.mm](prototype.mm).

The CPU uses the production encoded merge and writes its sections directly to
a fresh mapping. It neither reconstructs whole keys nor takes a second
width-discovery pass in these timings. In the fixed family, both source headers
prove the common seventeen-byte encoded value width. The variable family uses
ordinary variable framing. The GPU determines the output width itself. These
rules happen to produce the same canonical files throughout this measured set;
we separately test variable inputs whose output becomes fixed width, using an
honest CPU discovery pass for that correctness oracle.

The GPU imports the original mapped files. It selects their offsets and parses
records on the device, builds a levelwise prefix-ownership tree and comparison
cache, merges and compacts references, computes output sizes and scans, and
emits FC plus all Elias–Fano sections. The CPU reads only small scheduling/layout
results and writes the envelope and checksums. This experiment uses the
levelwise tree, not the optional tiled tree from the bit experiment.

For both paths, the complete timer includes fresh allocations and mapping,
construction, copying/emission, checksums, final clipping, unmapping and scratch
reclamation. It excludes input fixture creation, the initial input mapping and
admission scan, shader/pipeline compilation, verification, `fsync`, catalog
publication and fractional-index construction. Thus this measures native-file
construction from accepted mapped inputs, not a complete database operation.
The benchmark requests and verifies `USER_INITIATED` QoS on its own thread;
CPU affinity and clock frequency are not fixed.

Where the GPU spends time
------------------------

For the 131K fixed case, the device command-buffer intervals total 2.660 ms
against 5.934 ms complete time. For the variable case they total 8.259 ms
against 17.429 ms complete time. Device-only timings omit host work and waits;
they are not substitutes for the complete results above.

In the large variable case, the output-emission phase takes about 7.808 ms and
assembly about 4.533 ms. Assembly includes the EF planning round trip, output
mapping, small metadata and the final checksums. The byte emitter currently
binary-searches record offsets for each output word. That is a concrete
optimization opportunity; these results do not assume it has been removed.

Correctness and scope
---------------------

The release CTest target and the ASan/UBSan host executable both passed on the
actual Metal device:

- **40 complete-file merge cases:** empty inputs, sole empty keys, binary and
  proper-prefix keys, long prefixes, conservative FC across 15-record cuts,
  equal-key updates, tombstones versus empty values, asymmetric inputs,
  randomized records, and fixed/variable framing transitions.
- **Four rejection cases:** noncanonical LEB128, invalid optional-value tag,
  a suffix exceeding its block and a malformed EF sample, with coherent file
  checksums so the shader parser must reject the corruption itself.
- **Ten EF output cases:** repeated offsets, packed-word boundaries, short final
  blocks and sparse-exception thresholds, compared with the CPU wire encoder.

The shader toolchain produced and validated Vulkan 1.3 SPIR-V for all eighteen
entries before translating them to Metal. No Vulkan execution is measured.
The bit-profile tombstone-cleanup shader is explicitly disabled for this byte
wrapper: its tag grammar differs. This cut implements ordinary replacement,
not strong-delete cleanup, a typed conservative tombstone-depth guarantee,
fractional-index rebuilding or runtime GPU dispatch.

Evidence and reproduction
-------------------------

The measured source is `0ae73ea`, integrated as `a234b89`. The optional driver
and shader hashes agree; [reproduction metadata](results/2026-09-16-m2max/reproduction.json)
records the integration's separate common-default header changes. The retained
[header patch](results/2026-09-16-m2max/measured-headers.patch) reconstructs the
measured headers from that integration commit without needing the worker branch.
For exact remeasurement, use a separate checkout of `a234b89`, apply the patch
from this report's checkout, and commit that reconstruction locally before
running the collector's clean-tree check. Its commit identity will differ, but
the recorded source hashes will match. The original
measured header closure, compiler, executable, Metal library and SPIR-V hashes
remain in [metadata.json](results/2026-09-16-m2max/metadata.json).

I retained all **90 measured observations and eighteen excluded warmups** in
[observations.csv](results/2026-09-16-m2max/observations.csv), alongside original
per-process CSVs. [summary.json](results/2026-09-16-m2max/summary.json) preserves
process medians and full trial ranges. [identities.json](results/2026-09-16-m2max/identities.json)
records actual complete input/output file sizes and hashes;
[qualification.json](results/2026-09-16-m2max/qualification.json) and the
[correctness log](results/2026-09-16-m2max/correctness.log) record validation.
The [manifest](results/2026-09-16-m2max/sha256.json) authenticates the retained
artifacts.

After the [standalone build](README.md#build-and-check), reproduce the bounded
collection on an otherwise idle device:

```sh
python3 optional/byte_gpu_merge/collect.py --build build-byte-gpu \
  --output build-byte-gpu-results --work build-byte-gpu/retained \
  --processes 3 --trials 5
python3 optional/byte_gpu_merge/analyze.py build-byte-gpu-results
```

The collector requires a clean tracked source tree and a new results directory.
There is one warmup per process/case; CPU/GPU order alternates between trials.
The analyzer verifies the retained hashes before recomputing its summary.
