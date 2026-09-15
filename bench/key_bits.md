# Key prefixes, bit copies, and framing

I compared the key/bit changes against a fixed baseline on an Apple M2. The
byte-prefix helper uses bounded NEON vectors on this host, with SSE2 and portable
word paths for other builds. Shifted bit copies and framing use words and masked
bytes; these are not SIMD kernels.

## Reproduce

The [runner](key_bits.py) compiles one snapshotted fixture twice. The measurements
below use the fixture from `b208f18` (SHA-256
`e6d0ce2b434f7b79dfbf86b6ba11c5d9f133ada2bddc064e82846640a0959d59`). Its baseline is
`62ead3fab9d0ee5bda1b47780b7905a45aae1182`; the measured candidate headers come
from `d027162`. Only `profile.h`, `front.h`, and `key_detail.h` are overlaid.
Every dependency remains at the baseline revision, so this comparison excludes
concurrent rank/select changes.

Run these commands under the host's CPU/build-directory resource lease:

```sh
python3 bench/key_bits.py --candidate d027162 --harness b208f18 \
  --isolate-key-headers --trials 5 \
  --work 8388608 --output build-key-bits/results.csv
python3 bench/key_bits.py --candidate d027162 --harness b208f18 \
  --isolate-key-headers --sanitize --trials 1 \
  --work 65536 --output build-key-bits/check.csv
```

The runner needs Python's standard library, Git, and a C++20 compiler. It makes
no network requests. `CXX` selects the compiler. It can compare complete
header snapshots or the working tree, normalizing their names consistently.
Use the explicit candidate and harness above for this comparison. The
explicit `--isolate-key-headers` switch preserves this historical experiment's
three-header overlay. `--harness` selects the source revision independently and
is required above to reproduce the recorded fixture. New JSON files record both
resolved revisions, whether either snapshot used working-tree files, exact
source/header hashes, flags, and host information.

The retained [fixture](key_bits.cc) uses typed profile comparisons when the
untyped front-code header is absent. Its `front_order_and_lcp` row retains the
label so output comparisons remain possible, but those runs are a different
API path; they do not replace the historical results below. Complete-header
comparisons can also include changes outside the three measured key helpers.

The checked-in [CSV](key_bits_m2.csv) contains every trial; the
[metadata](key_bits_m2.json) identifies the measured sources. I used Apple
Clang 21, `-O3 -DNDEBUG`, macOS 26.6.2, and five alternating baseline/candidate
process orders. Each process requests interactive QoS. No other gated CPU-heavy
job ran concurrently with timing. This is an ordinary operating-system
measurement, not a real-time latency guarantee.

## What is measured

- Key comparisons request both order and common-prefix length through the
  existing public functions. Late mismatches occur in the last meaningful bit;
  separate first-bit mismatch cases exercise short-circuit behavior. The front
  byte-string case similarly requests both answers.
- Copy cases use 8-byte, 64-byte, and 4096-byte keys. Source offsets are 0 or 3 bits;
  the shifted cases include one extra meaningful tail bit. Destinations begin at
  bit 0 or bit 3. Buffers are reused across iterations.
- Count cases encode values up to 4, 32, or 64 bits with the unchanged exponential-
  Golomb 0 format. The 64-bit set includes `UINT64_MAX`, whose codeword occupies
  129 bits. Each stream begins at bit 0 or bit 3.
- Profile fixtures contain 1024 sorted records with an 8-byte or 128-byte shared
  prefix, a four-byte ordinal, and a five-byte value. Bit profiles append one
  meaningful bit to each key. Native LPFC uses restart factor 18. Build includes
  construction and destruction of each result; reconstruction includes result
  allocation/destruction and requests the full key.

The larger key fixture is a large *key*, not a cache-sized or disk-sized data
set. These tests use a small, repeatedly accessed working set. They do not
measure page faults, cold mmap traversal, or storage-device I/O.

The harness validates copy results with independent bit extraction, validates
profile reconstruction against the input records, and checks count values and
consumed positions. Every trial must match baseline output checksums and exact
encoded-byte digests. Validation, hashing, and formatting happen outside timed
intervals. Profile digests cover encoded record bytes; the broader integration
benchmark separately checks the complete blob/index encoding.

## Results

Times are nanoseconds per operation, shown as median [minimum, maximum] over
five trials. The ratio is baseline/candidate; values above 1 mean less time in
the candidate. The `bits` column is key length for primitive cases, value width
for counts, and shared-prefix length for profile cases. Source offset is in bits.

| Operation | Bits / offset | Baseline ns | Candidate ns | Ratio |
|---|---:|---:|---:|---:|
| `order_and_lcp` | 64 / 0 | 15.19 [14.64, 28.93] | 7.89 [7.78, 11.06] | 1.93× |
| `order_and_lcp` | 65 / 3 | 123.18 [120.97, 160.43] | 16.73 [15.88, 20.71] | 7.36× |
| `order_and_lcp` | 32768 / 0 | 4,632.89 [4,590.17, 4,805.10] | 247.40 [241.62, 330.81] | 18.73× |
| `order_and_lcp` | 32769 / 3 | 63,983.40 [62,964.40, 64,909.90] | 2,906.33 [2,830.97, 3,617.84] | 22.02× |
| `front_order_and_lcp` | 64 / 0 | 6.44 [6.20, 9.39] | 2.09 [2.06, 3.02] | 3.08× |
| `front_order_and_lcp` | 512 / 0 | 40.15 [39.73, 42.73] | 6.16 [5.88, 7.60] | 6.52× |
| `front_order_and_lcp` | 32768 / 0 | 2,646.24 [2,540.85, 2,811.77] | 243.33 [232.10, 318.20] | 10.88× |
| `order_and_lcp_early` | 64 / 0 | 3.08 [2.95, 5.52] | 3.73 [3.68, 5.15] | 0.83× |
| `order_and_lcp_early` | 65 / 3 | 3.60 [3.39, 4.02] | 3.82 [3.77, 4.61] | 0.94× |
| `copy_to_aligned` | 64 / 0 | 2.76 [2.64, 4.62] | 2.70 [2.66, 3.62] | 1.02× |
| `copy_to_aligned` | 512 / 0 | 2.76 [2.64, 3.38] | 3.45 [3.30, 4.13] | 0.80× |
| `copy_to_aligned` | 32768 / 0 | 51.59 [51.35, 60.38] | 50.70 [50.62, 66.65] | 1.02× |
| `copy_to_aligned` | 65 / 3 | 86.96 [83.82, 100.59] | 5.91 [5.87, 7.34] | 14.72× |
| `copy_to_shifted` | 32769 / 3 | 105,338.00 [99,465.90, 112,352.00] | 56.64 [55.75, 70.39] | 1859.76× |
| `read_count` | 4 / 0 | 12.17 [11.95, 12.67] | 4.37 [4.21, 5.10] | 2.79× |
| `write_count` | 4 / 0 | 24.09 [23.66, 25.07] | 5.49 [5.36, 6.02] | 4.39× |
| `read_count` | 32 / 0 | 118.90 [117.01, 122.07] | 6.56 [6.23, 7.25] | 18.11× |
| `write_count` | 32 / 0 | 276.00 [271.33, 280.70] | 13.33 [13.09, 13.97] | 20.71× |
| `read_count` | 64 / 0 | 254.37 [249.10, 257.71] | 4.27 [4.09, 4.48] | 59.61× |
| `write_count` | 64 / 0 | 565.04 [546.77, 572.14] | 19.61 [18.72, 20.49] | 28.82× |
| `byte_profile_build` | 64 / 0 | 59,994.60 [58,370.60, 61,222.10] | 38,566.70 [37,207.80, 40,030.90] | 1.56× |
| `byte_profile_reconstruct` | 64 / 0 | 1,417.29 [1,406.79, 1,441.51] | 1,414.69 [1,404.41, 1,432.83] | 1.00× |
| `bit_profile_build` | 64 / 0 | 149,619.00 [148,525.00, 152,800.00] | 61,893.60 [60,479.90, 64,763.70] | 2.42× |
| `bit_profile_reconstruct` | 64 / 0 | 4,548.16 [4,504.26, 4,565.08] | 4,199.93 [4,136.84, 5,054.81] | 1.08× |
| `byte_profile_build` | 1024 / 0 | 212,673.00 [207,601.00, 226,292.00] | 45,339.30 [43,482.10, 53,601.10] | 4.69× |
| `byte_profile_reconstruct` | 1024 / 0 | 12,675.30 [12,589.40, 12,914.00] | 12,763.30 [12,712.50, 12,840.40] | 0.99× |
| `bit_profile_build` | 1024 / 0 | 310,274.00 [290,321.00, 314,899.00] | 69,964.30 [64,470.30, 76,214.30] | 4.43× |
| `bit_profile_reconstruct` | 1024 / 0 | 46,639.90 [46,380.50, 46,886.70] | 41,673.10 [41,300.20, 42,014.40] | 1.12× |

Profile builds improve 1.56–4.69× in these fixtures. Byte-profile reconstruction
is essentially unchanged; bit-profile reconstruction improves 1.08–1.12×.
The vector/word byte-prefix path improves the separate front-code comparison
cases 3.08–10.88×.

This is not a win in every microcase. First-bit mismatches still cost roughly
0.2–0.7 ns more than the baseline. The 64-byte aligned copy costs 0.69 ns more;
8-byte and 4096-byte aligned copies are approximately unchanged. The explicit
short paths remove the larger regressions seen in the first candidate, while
retaining the improvements elsewhere.

The very large shifted-copy ratios measure a specific former cliff: one
meaningful tail bit previously sent the entire operation through a per-bit loop.
When source and destination shifts match, the new implementation can copy most
of the region with `memmove`. That does not imply an equivalent improvement in
arbitrary key workloads, storage throughput, or total query time.

## Correctness and boundaries

The code changes preserve the encoded format. The native ASan/UBSan suites for
front/profile, blobs, sampling, index builders, and pipelines passed. After the
short fast paths, the complete profile suite and baseline/candidate benchmark
sanitizer check passed again. Strict x86_64 builds of the front and profile
suites also passed under Rosetta, exercising SSE2. I do not infer native x86
performance from that emulated correctness check.

Independent tests cover all source/destination bit offsets 0–7; widths 0–64;
every first-differing bit in a 513-bit key; strict prefixes; untouched edge bits;
forward and backward overlapping copies; and self-append across storage growth.
Guard pages place the last physical source byte directly before inaccessible
memory, including zero-length views and the 129-bit maximum codeword. Every
truncation of that codeword and each invalid nonzero suffix bit is rejected,
with explicit consumed-position checks.

Loads use `memcpy` or explicitly unaligned vector operations and never require
padding beyond the supplied view. The copy path preserves both partial edge
bytes. A first-bit comparison and a byte-aligned `memmove` path keep common short
cases out of the more general machinery. ISA selection is compile-time; the
portable word path remains available when neither vector guard is enabled.
