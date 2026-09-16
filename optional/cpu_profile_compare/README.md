Matched CPU profile merges
==========================

This experiment compares native CPU merges on the exact logical
fixtures used by [the matched fixed-key Metal comparison](../fixed_kv_compare/README.md).
The [measured report](report.md) compares the frozen production revisions
`5868b32` and `a760695`, including complete latency and same-data file space.
The source supports four separate modes:

- `raw-bit`: KV02 opaque keys and values, bit controls and bit units.
- `raw-byte`: KV02 opaque keys and values, byte controls and byte units.
- `typed-bit`: KV03 ordinary optional-string sort, with its bit selector and
  typed value framing.
- `typed-byte`: KV02 built-in optional-string sort through the revision's actual
  typed key/value transport. Its before/after wire framing may differ.

The raw modes contain exactly sixteen key bytes and identical value bytes.
Sixteen-byte fixed values use a 128-bit width in the bit policy and a sixteen-byte
width in the byte policy. Variable values contain the same zero through 512 bytes.
No key bits or extra values are added to make one policy's fixture different.
The typed modes represent the same logical keys and present values through their
actual codecs. Every mode emits the same canonical logical input/output identity
files; keys are decoded and values checked outside timing.

The complete timer covers construction of the merge frontier on existing mapped
inputs, all native output allocation and encoding, EF construction, copying final
sections into a fresh output mapping, normal body/header CRC32C, file clipping,
and return-time cleanup. It does not include input creation, fixture preparation,
CPU sorted-record oracles, full output verification, or durable synchronization. No
catalog or fractional index is built. Inner merge/output phase times exclude
some final destructor work and are diagnostics; the outer timer includes it.

Each output is compared byte-for-byte against a canonical sorted-record encoding,
including the full directory, EF sections, padding and checksums. Raw profiles
use the ordinary native writer and its declared policy width, matching runtime
construction; they do not infer a common width with an extra whole-batch pass. Warmup outputs
are additionally decoded into the independent logical fixture. A complete CRC
scan follows every output outside timing.

```sh
cmake -S optional/cpu_profile_compare -B build-cpu-profile \
  -DEVERETT_HEADER_ROOT=/path/to/frozen/include
cmake --build build-cpu-profile --parallel 1
ctest --test-dir build-cpu-profile --output-on-failure --parallel 1
build-cpu-profile/cpu-profile raw-byte /path/to/scratch 0 3
```

The chosen header tree determines the implementation under test. Compile the
unchanged harness against each frozen revision, retain both source manifests,
and verify matching logical hashes before interpreting performance. The retained
collection uses three fresh processes per case, mode and revision, with one
excluded warmup and three timed iterations per process. `collect.py --help`
describes collection inputs; `analyze.py results` checks the retained raw evidence
and regenerates the report, summaries and exact space tables without running a
benchmark.

Space tables count complete file bytes and bits. They also separate the
front-coded record stream (keys, controls and values), EF sections, and remaining
metadata/alignment. All formats represent identical logical records; raw fixed
values declare their width, while typed optional values retain their actual
transport framing. These comparisons do not measure catalog publication,
fractional-index construction or durable synchronization.
