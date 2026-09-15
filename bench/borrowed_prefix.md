# Reusing borrowed-key prefixes

Keeping the previous key's actual common prefix reduces complete incremental
borrowed-profile construction time by 26.0–46.2% in these measurements. The
candidate produces exactly the same encoded payload, metadata and EF directory
as the batch writer. It keeps one private previous-key buffer across appends.

## Change and exception guarantee

Previously, every append allocated and copied a new complete predecessor key.
The candidate reserves enough private predecessor capacity before touching
encoded output. After every potentially allocating output write succeeds, it
resizes within that capacity and copies only the new suffix. Byte profiles
retain whole bytes of the actual common prefix, allowing aligned suffix copies;
bit profiles retain the exact common-bit count.

The retained predecessor prefix is independent of an optional encoding prefix
ceiling. For example, a ceiling of zero still emits the complete key while the
private predecessor retains its actual shared prefix. Callers' temporary input
views remain valid only for the duration of `append`.

Reservation can change private capacity, but leaves logical predecessor content
unchanged. Output failures restore the old output length and offset count.
The final predecessor resize cannot allocate: its validated size is within the
reserved capacity, and `std::byte` initialization cannot throw. The suffix
subview and destination bounds follow from the already computed common-prefix
length. Resize clears the final padding before the bounded suffix copy, which
preserves those unused bits. No fallible operation remains after output commits.

The new allocation-injection test fails each allocation reached by an append,
then checks a copy of the rejected state, retries, and compares the completed
output with the batch encoder. It covers initial/interior/block-boundary
appends, predecessor growth, both unit profiles, ordinary coding and ceiling
zero. Other fixtures cover repeated keys, shrink/growth transitions, long
prefixes, partial final bytes, shifted input views and immediate input-scratch
destruction. Existing profile tests cover rejection of unsorted or invalid-unit
input. The focused CMake ASan/UBSan profile and borrowed-writer tests and the
installed-package consumer passed, 3/3; see the
[check record](results/borrowed_prefix_checks.json).

## Measurement

The baseline is `1967b4911ab74e46cf944efa4990db7e75033a1c`; the measured
candidate is `2b71fd2`. The [shared harness](borrowed_prefix.cc) and
[runner](borrowed_prefix.py) snapshot both header trees, then compile one
identical harness source against each. The runner records resolved revisions,
source and header hashes, compiler commands and execution order.

The change is available on main at `f2b92af`; reproduction uses that reachable
revision. The benchmark harness is available on main at `fdcad44`.

Keys are a repeated prefix followed by an eight-byte big-endian integer.
Bit-profile keys append three meaningful bits. Integer order establishes the
input order independently of the comparison being measured. Each process
constructs an independent batch reference and warms the incremental writer.
The timer covers all appends and `finish()`, including output allocation,
EF finalization and destruction of the finished writer. Result destruction and correctness checks are outside the
timer. The writer uses ordinary FC with no explicit prefix ceilings in this
measurement; ceiling behavior is covered by the correctness tests.

Every warm and timed result is compared directly with the reference's payload,
metadata and every EF array/sample. A sequential cursor reconstructs every key
and compares its full canonical bytes and bit length with the input. The runner
also requires identical payload sizes and framed wire digests across variants.
The digest consumes every encoded section after timing; it is a comparison aid,
not a cryptographic commitment.

Each case uses 16,384 records, five alternating baseline/candidate process
trials, and three timed rounds per process. Both profiles use `K=W=15` and zero
borrowed value width. Native M2 Max, AppleClang 21, C++20, `-O3 -DNDEBUG`,
user-initiated QoS. Compilation and timing hold the exclusive CPU/build-directory
resource lease. Core assignment and frequency are controlled by the OS.

## Results

Times below are median nanoseconds per record, including amortized finalization,
over the 15 rounds in each variant/case. A negative change means less time.

| Profile | Prefix bytes | Baseline | Candidate | Change |
| --- | ---: | ---: | ---: | ---: |
| byte | 0 | 63.151 | 35.258 | -44.17% |
| byte | 64 | 64.613 | 36.982 | -42.76% |
| byte | 4,096 | 369.789 | 198.959 | -46.20% |
| bit | 0 | 91.408 | 66.828 | -26.89% |
| bit | 64 | 92.662 | 68.532 | -26.04% |
| bit | 4,096 | 599.798 | 434.242 | -27.60% |

Baseline and candidate round ranges are disjoint in all six cases. These ranges
are averages over full construction rounds, not individual-record tail
latencies. All 180 release rows passed their exact-section, full-key and
cross-variant checks. The [CSV](results/borrowed_prefix_m2max.csv) and
[metadata](results/borrowed_prefix_m2max.json) preserve every result.

A separate 257-record ASan/UBSan run used the same baseline/candidate harness
for all three prefix lengths. All 12 rows passed; its
[CSV](results/borrowed_prefix_sanitizer.csv) and
[metadata](results/borrowed_prefix_sanitizer.json) are separate from release
timing evidence.

The comparison still reads the common prefix to find its length. Predecessor
capacity may still grow when longer keys arrive. The performance fixture has
fixed key lengths within each case; it does not establish the same gain for
varying lengths, whole index pipelines, mapped reads or native x86. Inputs and
output are resident in memory, with no disk I/O or durable checkpoint claim.

## Reproduction

Run under the host's CPU/build-directory resource gate.

```sh
python3 bench/borrowed_prefix.py --baseline 1967b49 --candidate f2b92af \
  --build-dir build-borrowed-prefix-release --records 16384 \
  --prefix 0 64 4096 --rounds 3 --trials 5

python3 bench/borrowed_prefix.py --baseline 1967b49 --candidate f2b92af \
  --build-dir build-borrowed-prefix-sanitize --sanitize --records 257 \
  --prefix 0 64 4096 --rounds 1 --trials 1
```
