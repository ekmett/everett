Fixed-key complete merges on Metal
==================================

I use this isolated experiment to measure the proposed
[fixed-key table layouts](../../docs/table-layouts.md). It produces a complete native output:
keys, live values, a small experimental envelope, and the complete Elias–Fano
directory for variable values. The output is checked byte for byte against a
CPU merge using the library's `elias_fano::build`.

These fixture formats are separate from Everett's persistent object format.
The experiment does not change the library, select a production crossover,
or reuse the string-merge calibration.

Inputs and meaning
------------------

Both inputs contain unique, sorted, 128-bit keys. I compare four unsigned
32-bit lanes lexicographically; the fixture wire representation is little
endian. `.ff` values occupy 16 bytes each. `.fv` values occupy a packed byte
region, with a native Elias–Fano sequence of all $N+1$ value boundaries.
Repeated boundaries represent empty values, and the final boundary is real
EOF. Input boundaries are decoded on the GPU during the measured merge.

A third mapped input contains **due scheduled cancellations**: strictly
increasing ordinals in the older input, that input's fixture identity and
record count, and the exact sum of the canceled payload extents. This is an
explicit input assumption. There is no CPU key-matching or survivor-discovery
pass before a GPU merge. Untimed fixture generation creates the logical
records and cancellation metadata. `fixture_encode_ms` reports the subsequent
encoding of the two table files, separately from merging.
Replacing a key means canceling its older record and inserting its new record.
An uncanceled equal key is an invalid schedule.

These are validated, generated fixtures, not an untrusted-file reader. The
bounded shader arithmetic uses 32-bit extents; the host limits file and
intermediate extents below $2^{30}$. The experimental identity is a fixture
tag, not a substitute for production content identity. Only cancellations due
at this two-input merge are modeled. Future cancellation sections, their
relative destinations, sort dispatch, categorical composition, fractional
index reconstruction, signatures, catalog installation, and durable sealing
are outside this experiment.

The pipeline
------------

1. Mark due targets, scan the keep flags, and compact the older ordinals.
   This entire pass is omitted when there are no cancellations.
2. Partition the two surviving sorted sequences by Merge Path. Each invocation
   owns a 32-record output interval and emits source references.
3. For `.fv`, decode both input directories, gather surviving lengths, and
   scan them to obtain packed output boundaries. Compare the final extent
   against the schedule's promised payload size.
4. Classify the output EF sample groups and scan their exception counts. The
   CPU reads only constant-sized totals to finalize the envelope.
5. Copy keys and actual value bytes into the final mapped output. A variable
   payload invocation owns four complete output words, amortizing its initial
   boundary search across them. Aligned interior words use word loads;
   fragments crossing value boundaries use bytes. Output word ownership
   prevents races between unaligned adjacent values. A one-word variant is
   retained for comparison.
6. Write the canonical EF low words, high words, select samples and sparse
   exceptions directly into that output. Clip the file to its logical extent.

The output count and payload universe are known from scheduled cancellation
metadata:

$$
N=N_A+N_B-N_C,\qquad U=U_A+U_B-U_C.
$$

That does **not** determine the exact EF exception extent. I reserve its worst
case, determine the actual number on the GPU, then clip the finished file.
The sample interval is 256; groups spanning at least 4096 high-vector bits
store explicit positions. All four EF sections match the native CPU codec,
including its 64-bit words, bit order, final padding and sparse sentinels.

For `.ff`, value addresses are arithmetic. It omits the input EF decoding,
value-length scans and output EF passes entirely.

Timing boundaries
-----------------

`gpu_total_ms` starts before temporary Metal buffers are allocated and ends
after commands finish and those buffers are reclaimed. It includes both
command submissions, cancellation, scans, partitioning, input EF selection,
payload copying, EF construction, constant-sized host checks and envelope
assembly. `gpu_device_ms` is the sum of the two Metal command-buffer intervals;
it is diagnostic, not the complete latency.

`cpu_ms` retains the initial single-thread baseline, using random-access EF
selection for each value's two boundaries. `cpu_forward_ms` uses a forward
high-bit cursor, clears its lowest set bit for each decoded boundary, skips
whole canceled words by popcount, and caches the shared boundary of adjacent
values. Both stream the same keys and due ordinals and write identical output.
Both include temporary offsets and EF allocation/reclamation and use the
production EF output encoder. Neither is a parallel CPU merge.

All three output mappings are reused. Fixture encoding, mapping/import setup,
pipeline compilation/loading, file clipping and output comparison are outside
the steady merge columns and reported separately. The setup column includes
copying generated fixture bytes into the initial mappings. Inputs and outputs
are resident after one warmup. Timed iterations rotate the order of both CPU
variants and the GPU; benchmark rows report medians and ranges of five iterations. Small
correctness rows use one iteration and are not crossover measurements.

There is **no `msync`, `fsync`, checksum, content-address computation or catalog
commit** in these times. The files are scratch outputs. Mapping page faults,
storage bandwidth and durable publication need their own end-to-end tests.

Building and checking
---------------------

Use the [C++26 module toolchain](../../docs/modules.md), Ninja and an installed
`simd` package with exceptions enabled. CMake builds the host and links Everett
from this source checkout; Python handles shader artifacts and manifests.


The standalone build follows the existing HLSL → validated SPIR-V → MSL →
Metal path. Install DXC, SPIRV-Tools and SPIRV-Cross and make their executables
available, or pass their paths as CMake cache variables:

`EVERETT_EXPERIMENT_ARCH=AUTO` selects NEON on an ARM host with the native
Everett archive, and scalar elsewhere. Select `AVX2` or `AVX512` explicitly
only on a matching x86 build host; configuration checks its required features.
`SCALAR` is always available. The experiment's policies carry that architecture
explicitly, and new collections record it in their build metadata. Existing
retained measurements describe their original sources and compiler settings.

```sh
cmake -G Ninja -DCMAKE_PREFIX_PATH=/path/to/simd -S optional/fixed_gpu_merge -B build-fixed-gpu \
  -DEVERETT_DXC=/path/to/dxc \
  -DEVERETT_SPIRV_VAL=/path/to/spirv-val \
  -DEVERETT_SPIRV_CROSS=/path/to/spirv-cross
cmake --build build-fixed-gpu -j 1
ctest --test-dir build-fixed-gpu --output-on-failure
build-fixed-gpu/prototype build-fixed-gpu/kernels.metallib \
  build-fixed-gpu/scratch bench > build-fixed-gpu/results.csv
```

Use a disposable scratch directory: the executable replaces `input-a.tmp`,
`input-b.tmp`, `due.tmp`, `cpu.tmp`, `forward.tmp` and `gpu.tmp` there. It requires a Metal
device. The build writes `source-hashes.json` next to the executable.

The correctness suite covers empty inputs, full cancellation, replacement
collisions, prefix and scattered cancellation sets, highly unequal sizes,
values of length zero through 512, output words crossing value boundaries,
all four key lanes, and a large payload gap forcing real EF sparse exceptions.
Both payload tile widths are checked. Every measured output, including the
forward CPU variant, is compared in full against the original CPU oracle.
Benchmark mode adds intermediate sizes, tiny values from zero through six
bytes, and larger values from 2048 through 4096 bytes. `fixtures.h` provides the
exact logical generator for comparisons with other physical formats.

-Edward Kmett
