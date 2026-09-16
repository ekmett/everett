Byte-profile GPU merge
======================

This optional experiment merges real byte-profile `KV02` files on Metal. I keep
its supported grammar narrow: the tagless built-in optional-string sort under
`storage_policy<>`, replacement composition, and 15-record codec blocks and
sampling. `gpu_registry<Policy, Compose>` enables only that exact combination.
The existing bit-profile shader contract is unchanged.

Both inputs are immutable, strictly sorted, unique-key files. The older input
comes first. Equal keys retain the newer encoded value; a tombstone remains a
record. The output is an ordinary native `.kv` file with the normal 96-byte
header, 128-byte directory, five sections, canonical Elias–Fano arrays, padding
and CRC32C. It is checked byte for byte against the CPU batch writer, then opened
and scanned through the ordinary mapped reader.

[Initial measurements](report.md) retain complete-file times, process spread and correctness evidence.

Pipeline
--------

1. Import the source mappings without copying or decoding records on the CPU.
   A constant-size packet describes their existing Elias–Fano sections.
2. The GPU selects block offsets and parses each block's byte LEB128 counts,
   FC literals and optional-value tags. Descriptors retain the shared bit-offset
   ABI, while input/output directory offsets and fixed strides count bytes.
3. Build the retained-prefix ownership tree and comparison cache, perform a
   partitioned merge, and compact older equal-key occurrences with a keep scan.
4. Detect common encoded value width, size and scan output records, and emit
   their byte FC payload into a fresh output mapping. Width seventeen means a
   one-byte presence tag and sixteen payload bytes.
5. Emit the complete Elias–Fano lows, highs, select samples and sparse exception
   words on the GPU. The CPU writes small layout metadata and the file checksum,
   then clips the mapping to its final file length.

The CPU reads a few counts, widths and terminal descriptors between phases. It
does not discover survivors, reconstruct input keys, or expand the input offset
directory before dispatch. The empty/empty case constructs a constant-sized
empty file directly. The prototype caps a source payload below $2^{28}$ bytes,
key/value lengths below $2^{27}$ bytes, and the merged count below $2^{24}$. Its
conservative maximum-record-size times count check also bounds temporary output
arithmetic below $2^{28}$ bytes. These are prototype support bounds.

The parser checks framing, tags, offset bounds and retained lengths. It is not
an admission proof for arbitrary untrusted files: comparisons trust the accepted
inputs' sorted order and uniqueness. Fixtures receive a full CPU scan before
measurement. Ordinary replacement preserves every distinct key, which lets
output suffixes borrow directly from their chosen source literals. Strong-delete
cleanup and the typed conservative tombstone-depth contract require additional
work; this driver does not enable those shader modes.

Build and check
---------------

On macOS with Metal, DXC, SPIRV-Tools and SPIRV-Cross available:

```sh
cmake -S optional/byte_gpu_merge -B build-byte-gpu
cmake --build build-byte-gpu --parallel 1
ctest --test-dir build-byte-gpu --output-on-failure
```

`build.py` uses the shared HLSL compiler helper to produce and validate Vulkan
1.3 SPIR-V for every entry, translate it to MSL, and link a Metal library. This
checks the Vulkan shader artifacts; it does not execute a Vulkan host driver.
For the ASan/UBSan host build:

```sh
python3 optional/byte_gpu_merge/build.py --build build-byte-gpu-sanitize --sanitize
build-byte-gpu-sanitize/prototype build-byte-gpu-sanitize/kernels.metallib \
  build-byte-gpu-sanitize/checks check
```

The suite covers empty inputs and keys, proper prefixes and binary strings,
long shared prefixes and conservative FC across block cuts, fixed and variable input framing,
newer replacements and tombstones, stored empty values, asymmetric inputs,
random records, and variable inputs whose output becomes fixed width. Rejection
cases corrupt canonical counts, tags, payload lengths and EF samples while
retaining a coherent container checksum. Separate GPU EF tests cover sparse
exceptions, repeated offsets and packed-word boundaries.

Measurement boundary
--------------------

```sh
build-byte-gpu/prototype build-byte-gpu/kernels.metallib build-byte-gpu/bench bench 5
```

Fixture generation, input mapping/admission, pipeline compilation/warmup and
result verification are outside the timers. Each measured merge allocates fresh
scratch and a fresh mapped output; GPU import, parsing, all construction passes,
EF output, checksums, clipping, unmapping and scratch reclamation are inside.
`gpu_device_ms` is the sum of device command-buffer intervals; `gpu_complete_ms`
is the complete host-visible operation. The first row per case (`trial=-1`) is
warmup. CPU/GPU order alternates between trials. The benchmark requests and
verifies `USER_INITIATED` QoS on its own thread, with relative priority zero;
it does not pin a CPU or control clock frequency.

The CPU baseline uses the production encoded native merge, writes its sections
directly to a fresh mapping, computes checksums and clips it. Fixed-width cases
use the width proved by both input headers; other timed cases retain variable
framing. Every timed CPU and GPU result must match the same complete canonical
file. This is a one-pass CPU baseline, without a hidden preprocessing pass.
For correctness fixtures where replacements turn variable inputs into a
fixed-width output, the CPU first performs an encoded width-discovery merge;
those fixtures are outside the timing set.

There is no `fsync`, durable catalog publication, fractional-index construction
or runtime dispatch in these measurements. This is a native-file construction
experiment, not a claimed application speedup or an automatic GPU cutoff.
