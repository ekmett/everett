Matched fixed-key and front-coded Metal merges
=============================================

I use this standalone experiment to compare the two complete Metal merge paths
on identical logical inputs. It does not change the library or the existing GPU
calibration. [The retained matched results](report.md) include all twelve cases.
Both formats merge two sorted, unique streams with no cancellations.
The [integration record](results/integration.json) identifies a public commit
whose complete measured source closure matches the original hashes.

Fixtures
--------

Each key is four unsigned 32-bit lanes. KV03 receives those lanes as sixteen
big-endian bytes, preserving the fixed format's ordering. Both paths receive
exactly the same present value bytes. The fixed format omits the optional-value
and sort framing required by KV03; that difference is part of the formats being
compared.

The twelve cases combine structured or hash-like keys with:

- FF: sixteen-byte values, 4,096 / 65,536 / 262,144 records per input.
- FV: values of zero through 512 bytes, 4,096 / 65,536 / 131,072 records per input.

The structured keys come directly from the fixed experiment's shared fixture
header. The hash-like keys apply a bijective 64-bit mixer to each logical ID for
the high half and a second mixed half, then sort outside timing. Uniqueness follows
from the high-half bijection. This is a deterministic hash-like distribution,
not a cryptographic hash workload. Canonical logical input/output files record
all key and value bytes, and the collector checks their SHA-256 equality across
both formats and every fresh process.

The existing KV parser's conservative maximum-record-size times record-count
bound excludes the largest FV case at 262,144 records per input. I retain that
guard and stop at 131,072 for both formats.

Measurement boundary
--------------------

The comparable `complete_ms` timer includes fresh output creation/mapping, Metal
imports of existing mapped inputs, temporary allocation, the complete GPU merge,
CPU constant metadata work, body and header CRC32C, file clipping, and return-time
resource cleanup. Fixed output CRCs use previously unused header words 19 and 20;
the header checksum sees its own field as zero. This wrapper adds integrity work
without changing the fixed merge kernels or their layout. KV03 retains its normal
checksums. Neither path calls `fsync` or `msync`, publishes a catalog root, or
constructs a fractional index.

Fixture generation, input encoding/mapping, CPU correctness oracles, shader and
pipeline loading, and full output verification are outside timing. One excluded
warmup precedes three measured iterations in every fresh process. Every output
must equal the corresponding CPU encoder's entire canonical file, including
checksums. The CPU oracle is also checked against the shared logical records.

The original internal timers are retained as diagnostic columns. Their scopes
are different: the fixed timer starts with imported inputs and output, while the
KV timer includes its own fresh output mapping and checksum. Only the outer
complete timers are the matched comparison. Fixed checksum time is separately
available; KV checksum time remains included in its complete timer.

The KV configuration is the previously calibrated optimized default: mapped
compressed inputs, GPU frame parsing and prefix-owner reconstruction, eight-byte
prefix cache, word emitter, and GPU output EF. The tree remains levelwise and the
output plan separate. The measured tiled-tree and fused-plan experiments remain
opt-in after mixed complete-path results. This comparison does not claim to find
the fastest possible KV implementation. Fixed uses its existing no-cancellation
fast path and Merge Path 32 ordering.

Reproduction
------------

Use the [C++26 module toolchain](../../docs/modules.md), Ninja and an installed
`simd` package with exceptions enabled. CMake builds the host and links Everett
from this source checkout; Python handles shader artifacts and manifests.


First build the two existing optional packages and supply their compiled Metal
libraries. Shader sources must match the retained artifact provenance. The
wrappers include the existing host implementations verbatim and compile only new
program entry points; they do not rebuild or modify the shaders.

```sh
cmake -G Ninja -DCMAKE_PREFIX_PATH=/path/to/simd -S optional/fixed_kv_compare -B build-fixed-kv \
  -DEVERETT_KV_METALLIB=/path/to/kv/kernels.metallib \
  -DEVERETT_FIXED_METALLIB=/path/to/fixed/kernels.metallib
cmake --build build-fixed-kv --parallel 1
ctest --test-dir build-fixed-kv --output-on-failure --parallel 1
python3 optional/fixed_kv_compare/collect.py \
  --build build-fixed-kv --output /path/to/new-results
```

Use the host GPU resource gate around the serial checks and collection. Commit a
clean source checkpoint before collecting. The fixed schedule uses three paired
fresh processes per case, alternating format order across rounds (a necessary
2:1 first-slot split with three rounds), and rotates case order by four each
round. No result-dependent reruns or case selection are part of the collector.
