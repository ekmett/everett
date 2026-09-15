Encoded literals without full-key copies
=======================================

The encoded native merger reduces median complete-merge time in all twenty
M2 Max fixtures in this comparison. Shared-prefix cases improve by **3.68–14.96%**;
keys with 4 KiB changing literals improve by **22.32–37.33%**. These gains come
with a space tradeoff: a chain of tiny prefix extensions can retain substantially
more span metadata than the materialized key buffers it replaces.

I compare the materialized merger at `e042920590f01c864ecc8019f4e1fd864d483de5`
with `e21766bbde779fec2364bec2198f8b790ce70312`. Their complete header trees differ
only in `native_merge.h`. Both use format 2, and every output payload and
Elias–Fano section matches exactly. Concurrent changes to query parsing are
excluded from this comparison.

Complete merge timings
----------------------

Median nanoseconds per distinct output key are below. Five alternating process
trials each contain three measured rounds: the fifteen observations per variant
are not fifteen independent processes. Negative change means less time.

| Policy | Shared prefix | Changing tail | Before | Encoded | Change |
| --- | ---: | ---: | ---: | ---: | ---: |
| byte fixed | 0 | 0 | 59.255 | 56.691 | -4.33% |
| byte variable | 0 | 0 | 78.725 | 71.279 | -9.46% |
| bit fixed | 0 | 0 | 109.812 | 97.646 | -11.08% |
| bit variable | 0 | 0 | 137.248 | 132.192 | -3.68% |
| byte fixed | 64 | 0 | 56.803 | 53.945 | -5.03% |
| byte variable | 64 | 0 | 75.836 | 71.554 | -5.65% |
| bit fixed | 64 | 0 | 107.707 | 91.594 | -14.96% |
| bit variable | 64 | 0 | 136.230 | 127.085 | -6.71% |
| byte fixed | 4,096 | 0 | 59.753 | 55.349 | -7.37% |
| byte variable | 4,096 | 0 | 77.209 | 70.943 | -8.12% |
| bit fixed | 4,096 | 0 | 108.815 | 96.344 | -11.46% |
| bit variable | 4,096 | 0 | 136.688 | 129.517 | -5.25% |
| byte fixed | 0 | 4,096 | 559.662 | 433.757 | -22.50% |
| byte variable | 0 | 4,096 | 565.582 | 439.372 | -22.32% |
| bit fixed | 0 | 4,096 | 1865.448 | 1169.128 | -37.33% |
| bit variable | 0 | 4,096 | 1858.846 | 1211.070 | -34.85% |

The shared-prefix baseline/candidate ranges are disjoint for byte-fixed/prefix64,
bit-fixed/prefix64 and byte-variable/prefix4096. All other shared-prefix ranges
overlap, and prefix0 has large outliers in both variants. Among changing-tail
cases, byte-variable and both bit profiles have disjoint ranges; byte-fixed
ranges overlap. I retain all observations rather than removing those outliers.
These are resident-memory measurements on one host, not universal latency bounds.

The fragment fixture first grows an all-zero key one policy unit at a time,
then walks decreasing-prefix branches. It exposes both deep span stacks and
large prefix truncations:

| Policy | Shared prefix | Changing tail | Before | Encoded | Change |
| --- | ---: | ---: | ---: | ---: | ---: |
| byte fixed | 0 | 0 | 57.220 | 54.189 | -5.30% |
| byte variable | 0 | 0 | 75.205 | 69.051 | -8.18% |
| bit fixed | 0 | 0 | 92.275 | 78.156 | -15.30% |
| bit variable | 0 | 0 | 121.592 | 103.231 | -15.10% |

All four fragment observation ranges overlap. The
[shared-prefix rows](results/native_encoded_first_m2max.csv),
[changing-tail rows](results/native_encoded_first_tail_m2max.csv),
[fragment rows](results/native_encoded_first_fragments_m2max.csv), and
[minimum/median/maximum summary](results/native_encoded_summary.csv) preserve
all 600 measurements. Each corresponding JSON records compiler commands,
full revisions, every header hash, the shared source hash and execution order.

What changes
------------

Default replacement and value-only two-argument callbacks use an encoded cursor.
A key-aware three-argument callback keeps a materialized cursor, including when
a custom callback accepts both signatures. The timings above exercise default
replacement. All paths emit every distinct key and preserve older/newer
callback order.

The encoded path represents the current logical key as immutable literal spans.
That retains enough context to check strict input order when a new retained
boundary lies before the previous record's literal. It also handles deliberately
redundant front coding without a trusted ordinary-encoding flag. Source owners
pin the spans, and the output writer copies the selected literal and value
before advancing either source.

After finding the preceding span at the retained boundary, the merger compares
one policy unit directly. Ordinary front coding differs within that unit; an
equal unit falls through to the full fragment comparison for redundant encodings.
The comparison still returns exact agreement in bits. Empty keys and proper
prefixes bypass the unit loads. The existing merge frontier supplies the output
retained length, so output can trim and forward the current encoded literal.

This removes input changed-suffix copies and full-key buffers from the encoded
path. It retains output copies, value work, frame decoding and strict-order
checks. No tombstone elision or restart protocol is added here.

Space and actual literal work
-----------------------------

A long common prefix is different from a long changing literal. Across the two
source streams, the byte-policy prefix4096 fixture contains only 109,576 literal
bits; its changing-tail counterpart contains 178,990,088. The bit-policy totals
are 95,672 and 178,976,184 respectively. The output literal totals are 65,712
versus 134,250,672 bits for byte policy, and 53,298 versus 134,238,258 for bit
policy. Every raw row records these exact totals and the source/output payload
sizes. Nominal key length alone does not describe the copy work avoided.

I separately instrumented the first encoded implementation, `2c975b4`, to count
requested C++ allocation bytes. The first-unit followup changes comparison work,
not the descriptor representation. The allocation experiment uses a separate
binary; **none of its elapsed fields contribute to the timing tables**.

The fragment fixture has 4,096 distinct keys and maximum depth 2,048 policy
units. Each descriptor stores a 32-byte bit view and an 8-byte logical endpoint.
The two stacks retain peak vector capacity. Peak requested live bytes during the
complete build, with returned-output live bytes identical in both variants:

| Policy | Materialized peak | Span peak | Returned output live |
| --- | ---: | ---: | ---: |
| byte fixed | 144,384 | 303,104 | 90,408 |
| byte variable | 154,624 | 313,344 | 98,696 |
| bit fixed | 20,992 | 211,968 | 12,624 |
| bit variable | 27,136 | 214,016 | 16,800 |

For prefix0 integer keys, peak grows by 304 bytes for byte profiles and 1,262
bytes for bit profiles. With a shared 4096-byte prefix, peak falls by 7,888 and
6,930 bytes respectively. The changing-tail output allocations dominate its
25–50 MB peak, so the scratch-space savings are relatively small there.

These counts include requested `new` bytes, transient merger storage and returned
output, while excluding preexisting sources/expected output, allocator headers,
alignment padding and RSS. The tracker is intentionally single-threaded.
[Ordinary allocation rows](results/native_encoded_allocations_m2max.csv),
[changing-tail rows](results/native_encoded_tail_allocations_m2max.csv), and
[fragment rows](results/native_encoded_fragment_allocations_m2max.csv) also record
total requested bytes and allocation counts. A chain of one-unit extensions can
retain one descriptor per unit; there is no constant-space claim independent of
key length.

The complete timing executable's `__text` grows from 130,408 to 133,960 bytes
(+3,552, 2.72%). Its `__TEXT` segment grows from 147,456 to 163,840 bytes, and
its file size grows from 307,376 to 327,056 bytes. These are this harness's
instantiations, not every consumer's size change. The
[size output and executable hashes](results/native_encoded_first_size_m2max.json)
retain the evidence; I did not measure compilation time.

Native merge method and correctness
-----------------------------------

The shared [harness](native_encoded.cc) generates 4,096 original records. Older
and newer subsets overlap on every third key; overlapping older values differ
and the newer value wins. Values use byte-fixed8, byte-variable8–24, bit-fixed13
and bit-variable8–24 policy units. Integer keys encode an eight-byte big-endian
ID after the shared prefix. Changing tails occur after the ID and vary with it.
The fragment fixture uses independently generated proper extensions and branches.

The timer includes builder construction, all `step(128)` calls, `finish()` and
builder destruction. Returned-output destruction is outside. Input creation,
oracles, key/value inspection and digest computation are outside each timer.
The build includes output encoding, allocation and EF finalization. It excludes
mmap page faults, file opening/writing and durable publication. I use native arm64
M2 Max, AppleClang21, `-O3 -DNDEBUG` and the exclusive host CPU/build-directory
lease. The harness requests user-initiated QoS without checking whether it is
honored.

After each build I compare complete payload bytes, metadata and every EF array
and sample with a canonical batch encoding, then every reconstructed key/value
with the original records. The runner also compares wire digests and literal
counts across variants. All 1,200 timing observations across both comparisons
pass. Forty separate shared-harness ASan/UBSan observations cover ordinary
prefixes, changing tails and instrumented fragments. Native and mapped merge
sanitizer suites also pass for the final first-unit header, including empty and
proper-prefix keys, redundant/LP sources, later descending or duplicate frames,
paused/moved builders, key-aware/value-only callbacks, callback failures and
unlinked mapped inputs whose owners remain pinned.

The initial encoded implementation `2c975b4` had byte-fixed shared-prefix median
regressions of 4.51–11.28%, with overlapping ranges. Its
[initial rows](results/native_encoded_initial_m2max.csv),
[tail rows](results/native_encoded_initial_tail_m2max.csv),
[fragment rows](results/native_encoded_initial_fragments_m2max.csv), and
[code size](results/native_encoded_initial_size_m2max.json) remain available.
The direct first-unit comparison above resolves those byte median regressions
in this run. I do not add percentages from separate experiments.

Reproduction
------------

Use the [runner](native_encoded.py) inside the configured host resource lease.
The shared harness is revision `012b497`; source/header hashes in each metadata
file are authoritative. The final comparison is:

```sh
python3 bench/native_encoded.py --baseline e042920 --candidate e21766b \
  --records 4096 --prefix 0 64 4096 --rounds 3 --trials 5 \
  --build-dir build-native-encoded-first
python3 bench/native_encoded.py --baseline e042920 --candidate e21766b \
  --records 4096 --prefix 0 --tail 4096 --rounds 3 --trials 5 \
  --build-dir build-native-encoded-first-tail
python3 bench/native_encoded.py --baseline e042920 --candidate e21766b \
  --records 4096 --prefix 0 --fixture fragments --rounds 3 --trials 5 \
  --build-dir build-native-encoded-first-fragments
```

For the separately recorded allocation experiment, use candidate `2c975b4`,
`--allocations --rounds 1 --trials 1`; for the shared harness checks use
`--sanitize --records 128 --rounds 1 --trials 1`. JSON metadata preserves exact
commands and revisions. CSV line endings are normalized to LF.
