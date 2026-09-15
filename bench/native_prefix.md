# Reusing native-key prefixes

Reusing the native writer's previous-key buffer reduces complete construction
time by 17.9–43.7% in these measurements. Fixed-width and variable-width values
are measured separately. Every result has the same encoded payload, metadata
and EF directory as the batch encoder, and reconstructs every input key/value.

## Change and checks

`profile_native_writer::append` reserves predecessor capacity before changing
output. After key and value writes succeed, it keeps the actual common prefix
and copies only the new key suffix. Byte profiles preserve whole common bytes;
bit profiles preserve the exact common-bit count. The output encoding and value
framing are unchanged.

The old logical predecessor remains intact throughout fallible output writes.
A failed write restores the output length and residual-offset count. After
success, the predecessor resize stays within reserved capacity, and the
validated common-prefix bounds make the suffix update nonallocating and
nonthrowing. Resize clears tail padding before the bounded copy. Inputs may
refer to temporary decoding scratch and need only survive the append call.

The new allocation-injection fixture fails every allocation reached by an
append, including growth needed by keys and values. It covers initial,
interior and physical-block-boundary records, zero/common/variable value
widths, both unit profiles and key growth/shrink transitions. After failure,
one run finalizes the unchanged committed prefix; another retries and compares
the complete result with batch encoding. For tiny variable-width prefixes,
batch auto-detection can choose a different common width, so those rejected
prefixes are checked by decoded contents and the requested width metadata;
completed retries receive exact encoded-section checks. Separate fixtures use
shifted key/value input views and destroy caller scratch after each append.

The existing native-writer suite also covers policy-fixed widths, rejection,
move ownership, and query use. CMake ASan/UBSan native-writer and allocation
tests plus the installed-package consumer passed, 3/3. The
[check record](results/native_prefix_checks.json) preserves source hashes,
configuration and CTest output.

## Method

The baseline is `fdcad441a501d8edc6eded225c613b6e2bec009d`; the measured
candidate is `f986ee29a0ec4245ef2a30709ebafb74cbcff803`.
The code is available on main at `08ea0d3` and the harness at `87c8d82`;
reproduction uses those reachable revisions.
The [harness](native_prefix.cc) and [runner](native_prefix.py) compile one
captured source against both pinned header trees. Snapshot directories are
cleared before reuse. Metadata records all resolved revisions, header/source
hashes, compiler commands and process order.

Each key has a repeated prefix and an eight-byte big-endian integer, whose
order defines the fixture independently of the measured comparison. Bit-profile
keys add three meaningful bits. Values use the repeating bit pattern selected
by `(record_id + bit_position) % 3`, with canonical zero tail padding.

- Fixed byte values: 8 bytes each.
- Fixed bit values: 13 bits each.
- Variable values: `8 + record_id % 17` policy units, meaning bytes or bits.

Each fresh process constructs a batch reference and warms the incremental
writer. The timer covers all appends, value writes, output allocation,
`finish()` and its EF construction, and destruction of the finished writer.
Result destruction and correctness checks are outside the timer. Every warm
and timed result is compared directly with the batch payload, metadata, EF
arrays and samples. A sequential cursor checks every full reconstructed key
and every meaningful value bit against the input. The runner additionally
checks payload sizes and framed wire digests across variants.

Each case uses 16,384 records, five alternating baseline/candidate process
trials, and three rounds per process. All policies have `K=W=15`.
Native M2 Max, AppleClang 21, C++20, `-O3 -DNDEBUG`, requested user-initiated
QoS. Compilation and timing hold the exclusive CPU/build-directory lease.
The OS controls core placement and frequency.

## Results

Times are median nanoseconds per record over 15 rounds per variant/case,
including amortized finalization. A negative change means less time.

| Profile | Prefix bytes | Baseline | Candidate | Change |
| --- | ---: | ---: | ---: | ---: |
| byte, fixed 8 | 0 | 74.936 | 47.562 | -36.53% |
| byte, fixed 8 | 64 | 79.213 | 49.993 | -36.89% |
| byte, fixed 8 | 4,096 | 390.045 | 219.589 | -43.70% |
| byte, variable | 0 | 94.968 | 56.417 | -40.59% |
| byte, variable | 64 | 101.733 | 59.616 | -41.40% |
| byte, variable | 4,096 | 425.730 | 255.068 | -40.09% |
| bit, fixed 13 | 0 | 113.294 | 85.991 | -24.10% |
| bit, fixed 13 | 64 | 118.523 | 87.181 | -26.44% |
| bit, fixed 13 | 4,096 | 626.991 | 463.267 | -26.11% |
| bit, variable | 0 | 136.210 | 111.852 | -17.88% |
| bit, variable | 64 | 140.338 | 110.832 | -21.02% |
| bit, variable | 4,096 | 624.926 | 474.861 | -24.01% |

Round ranges are disjoint between variants in all twelve cases. These ranges
are averages over whole construction rounds, not per-record tail latencies.
All 360 release rows passed the exact-section, full-key/value and cross-variant
checks. The [CSV](results/native_prefix_m2max.csv) and
[metadata](results/native_prefix_m2max.json) contain every measurement.

A separate 257-record ASan/UBSan run used both revisions and all twelve
profile/prefix combinations. All 24 rows passed. Its
[CSV](results/native_prefix_sanitizer.csv) and
[metadata](results/native_prefix_sanitizer.json) are separate from the timing
evidence.

Logical predecessor contents remain only the current key; capacity can retain
the largest key allocation seen until finalization or destruction. The fixture
has fixed key lengths within each case and does not establish the same gain for
varying lengths. Full common-prefix comparison and value copying remain part
of construction. These resident-memory writer measurements do not cover
complete merge pipelines, mapped reads, disk I/O, durability or native x86.

## Reproduction

Run under the host's CPU/build-directory resource gate.

```sh
python3 bench/native_prefix.py --baseline fdcad44 --candidate 08ea0d3 \
  --build-dir build-native-prefix-release --records 16384 \
  --prefix 0 64 4096 --rounds 3 --trials 5

python3 bench/native_prefix.py --baseline fdcad44 --candidate 08ea0d3 \
  --build-dir build-native-prefix-sanitize --sanitize --records 257 \
  --prefix 0 64 4096 --rounds 1 --trials 1
```
