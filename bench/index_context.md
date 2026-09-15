One incoming key context for index construction
===============================================

Reusing the sample decoder's current key removes the separate incoming and
pending key copies. In this M2 Max run, complete pipeline medians fall 5.1–9.2%
on 4096-byte shared prefixes; short-key medians range from 6.7% lower to 1.0%
higher. Every timing range overlaps. The allocation reduction is clearer than
the timing evidence: a separate 4096-record borrowed-only stage goes from about
4,400 C++ allocations to 330–340.

This compares `17e232d` with `57e3667`. Both already include the carried-LCP
index optimization and generic Output seam. It isolates the subsequent input
context change; it does not measure their combined gain over older builds.

State and failure behavior
--------------------------

The decoder holds the queued incoming key until that occurrence is consumed,
then keeps the preceding borrowed key until the next push. The builder retains
exact LCP scalars for merged-frontier comparisons, the previous borrowed cut
anchor, and the physical borrowed predecessor. The outgoing sample encoder
owns independent storage, so accepting another input cannot replace an unread
outgoing sample. Full-key input reserves capacity before replacing the changed
suffix; coded input already uses this reusable decoder storage.

Borrowed output is now appended during consumption. An output failure for the
last borrowed record therefore occurs in `step`, rather than in deferred
finalization. It poisons the builder and clears queued output. Invalid input or
a decoder growth allocation failure leaves the preceding accepted context
usable. This is not durable resume: uncertain file output remains private for
explicit reconciliation.

Other key contexts remain: native cursors, the outgoing encoder, and the
default in-memory borrowed writer still retain their own required state.

Complete pipeline measurements
------------------------------

Units are nanoseconds per augmented source occurrence. Each cell gives the
median and observed min/max of 15 values (five alternating fresh-process trials,
three rounds each). Negative change means less elapsed time. The rounds are
not independent statistical experiments, and the ranges are not confidence
intervals.

| Prefix bytes | Profile / fixture | Baseline median [min, max] | Candidate median [min, max] | Change |
|---:|---|---:|---:|---:|
| 0 | byte / interleaved | 43.50 [40.47, 68.26] | 42.73 [38.52, 59.43] | -1.8% |
| 0 | bit / interleaved | 75.51 [71.58, 97.27] | 76.26 [68.94, 97.72] | +1.0% |
| 0 | byte / duplicates | 42.41 [39.85, 53.76] | 39.58 [37.46, 49.52] | -6.7% |
| 0 | bit / duplicates | 70.68 [68.09, 79.67] | 66.95 [61.67, 72.57] | -5.3% |
| 4096 | byte / interleaved | 181.15 [171.07, 190.27] | 164.49 [157.68, 187.62] | -9.2% |
| 4096 | bit / interleaved | 206.44 [200.20, 222.55] | 195.82 [188.28, 214.00] | -5.1% |
| 4096 | byte / duplicates | 181.96 [167.22, 297.87] | 166.64 [159.18, 185.35] | -8.4% |
| 4096 | bit / duplicates | 209.12 [191.00, 229.07] | 191.18 [183.14, 203.71] | -8.6% |

All eight before/after ranges overlap. The unchanged sample traversal control
moves −3.1% to +3.7% on short prefixes and −2.6% to +0.9% on long prefixes;
its ranges also overlap. No rerun was selected, and these observations do not
establish a speedup on every workload or processor.

The unchanged [pipeline harness](sample_frontier.cc) and
[runner](sample_frontier.py) use 4096 native source records, byte/bit profiles,
interleaved or repeated borrowed keys (including false borrows), and three
output stages. The bit profile includes a partial-byte suffix. Timers include
pipeline construction, stepping, and final navigation construction. Fixture and
batch-oracle construction, full validation, and result destruction are outside
the timer. There is no file I/O in this timing. All 480 rows agree on source
occurrences, counters and output checksums. Original integer-key order/membership
oracles and untimed batch encodings check samples, borrowed payload, ranks,
cut LCPs and false flags.

Separate allocation probe
-------------------------

The [allocation probe](index_context_allocations.cc) builds one borrowed-only
stage from 4096 already prepared keys/frames, once for each profile and input
mode. This is a different fixture from the timed pipeline. The two versions
produce matching complete index checksums, and each reconstructed key is
compared bit by bit with its original input. Values below are baseline →
candidate from one deterministic counting run per row; no instrumented elapsed
time is reported or used as performance evidence.

| Prefix bytes | Profile / input | Allocation calls | Requested bytes | Peak live requested bytes |
|---:|---|---:|---:|---:|
| 0 | byte / full | 4,437 → 342 | 91,944 → 75,564 | 49,680 → 49,676 |
| 0 | bit / full | 4,436 → 341 | 71,667 → 51,192 | 27,156 → 27,153 |
| 0 | byte / coded | 4,438 → 342 | 91,948 → 75,564 | 49,684 → 49,676 |
| 0 | bit / coded | 4,437 → 341 | 71,672 → 51,192 | 27,161 → 27,153 |
| 4096 | byte / full | 4,429 → 334 | 16,893,867 → 104,367 | 78,436 → 74,336 |
| 4096 | bit / full | 4,427 → 332 | 16,865,354 → 71,759 | 47,672 → 43,573 |
| 4096 | byte / coded | 4,430 → 334 | 16,897,967 → 104,367 | 82,536 → 74,336 |
| 4096 | bit / coded | 4,428 → 332 | 16,869,455 → 71,759 | 51,773 → 43,573 |

The stack-resident builder object is 912 → 832 bytes on this compiler/ABI.
Allocation counts cover construction, pushes, stepping and finalization;
fixture preparation, validation and destruction are outside the counting interval.
The probe intercepts single-thread C++ `new`/`delete`, including aligned forms,
and reports requested bytes. Peak is the maximum simultaneously live requested
storage from that interval. It excludes fixture storage, allocator headers,
rounding, stack bytes, mappings and allocations outside C++ `new`; it is not
RSS or the entire database's memory footprint.

Validation and reproduction
---------------------------

Strict Release/O3 ASan+UBSan passed sampling, index builder, index pipeline and
borrowed-writer CTests (4/4), the actual streamed-file matrix, and the focused
decoder allocation-rollback test. Coverage includes byte/bit policies, fixed and
variable native values, independent K/W, duplicates/empty keys, exact whole
`.index` parity, original-key rank/cut/query oracles, mapped pins/unlink,
stream-append/final-barrier poisoning, and custom Output
exceptions. New tests specifically push while output is unread, move the
builder, reject a descending replacement, and retry failed context growth.

Measured with Apple Clang 21.0.0 on local M2 Max/macOS26.6.2 arm64 under the
exclusive host CPU lease. Flags are `-std=c++20 -Wall -Wextra -Wpedantic -Werror
-O3 -DNDEBUG`; no CPU affinity was imposed. Exact commit IDs, complete header
hashes, source/runner hashes, compiler text, commands and trial order are in the
[timing metadata](results/index_context_m2max.json); raw rows are in the
[timing CSV](results/index_context_m2max.csv). Separate
[allocation metadata](results/index_context_allocations_m2max.json) and
[allocation CSV](results/index_context_allocations_m2max.csv) record the probe.

From a checkout retaining the pinned commits, under the local CPU resource gate:

```sh
CXX='ccache clang++' python3 bench/sample_frontier.py \
  --baseline 17e232d --candidate 57e3667 \
  --build-dir build-index-context-reproduce --records 4096 \
  --prefix 0 4096 --rounds 3 --trials 5
CXX='ccache clang++' python3 bench/index_context_allocations.py \
  --benchmark-build build-index-context-reproduce \
  --output build-index-context-allocations
```

The probe verifies the saved header hashes before compiling against each
snapshot. These measurements do not cover streamed-output latency, native x86,
cache-exceeding datasets, or interrupted-job continuation.

-Edward Kmett
