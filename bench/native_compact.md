Compact native-merge span descriptors
====================================

I reduced each retained literal descriptor from 40 to 16 bytes. In the deep
fragment fixture this reduces peak requested allocation bytes by **31.4–58.0%**,
with unchanged output bytes and allocation counts. Runtime is mixed: every
baseline/candidate timing range overlaps, and I do not claim a speedup from
this space reduction.

The isolated baseline is `e21766bbde779fec2364bec2198f8b790ce70312`, the accepted
[first-unit encoded merger](native_encoded.md). The candidate is `81c28ef`.
Only `native_merge.h` differs in their complete header trees. Both retain strict
input-order checking, value-only/key-aware dispatch, the same format-2 codec and
identical encoded output. Later parser and disk-writer changes are excluded.

Representation and bounds
-------------------------

A descriptor now stores a source bit offset and the logical key endpoint in
policy units. The preceding descriptor's endpoint gives its logical start;
the first starts at zero. One backing bit view per input retains the exact
admitted meaningful extent, including a partial final byte. The input owners
continue to pin its bytes.

Truncation only removes descriptors or decreases the final endpoint. Appending
a literal records an offset from a frame of that same immutable backing. Every
reconstructed slice is therefore a prefix or subslice of the originally bounded
literal. The first-unit comparison and the redundant-front-coding fallback use
those slices; neither relies on padding bits or alignment of the mapped bytes.
The fixed backing view replaces repeated backing pointers in the descriptors.

New tests place the exact payload extent immediately before a protected page,
with actual unaligned source addresses and partial bit tails. They cover ordinary
and redundant deep fragments, moving a paused builder and retaining the mapping
after output. Existing native and mapped suites cover malformed prefixes/order,
empty and proper-prefix keys, value policies, callback failure and unlinked inputs.
Both sanitizer suites pass for the compact header. These are implementation tests,
not a formal refinement proof of the C++ code.

Allocation results
------------------

I use the same separately compiled single-thread allocation probe as the prior
report. Its timings are excluded. Counts are requested C++ heap bytes during
complete construction, not RSS, allocator metadata, preexisting source storage
or stack frames. Returned output stays live at the end; merger temporaries have
been destroyed. Each fixture has 4,096 distinct output keys.

| Fragment policy | Peak with 40-byte spans | Peak with 16-byte spans | Reduction | Output live, both |
| --- | ---: | ---: | ---: | ---: |
| byte fixed | 303,104 | 204,800 | 32.43% | 90,408 |
| byte variable | 313,344 | 215,040 | 31.37% | 98,696 |
| bit fixed | 211,968 | 89,088 | 57.97% | 12,624 |
| bit variable | 214,016 | 92,160 | 56.94% | 16,800 |

Total requested bytes fall by 196,560 in each fragment case. Allocation counts
remain 57/57/54/55, respectively. The ordinary integer/shared-prefix/changing-tail
cases reduce peak by 192 bytes for byte profiles and 768 bytes for bit profiles;
returned-output live bytes and allocation counts remain unchanged. The
[ordinary rows](results/native_compact_allocations_m2max.csv),
[changing-tail rows](results/native_compact_tail_allocations_m2max.csv) and
[fragment rows](results/native_compact_fragment_allocations_m2max.csv) retain all
40 instrumented observations.

This does not remove the fragment-space tradeoff. Against the separately measured
original materialized merger, compact fragment peaks remain **1.42×/1.39×** for
byte-fixed/variable and **4.24×/3.40×** for bit-fixed/variable. A worst-case chain
still retains one 16-byte descriptor per policy unit: 16 descriptor bytes per
byte of logical key, or per bit for bit policy, before vector spare capacity.
The 60% reduction describes the descriptor itself, not every complete-merge peak.
One fixed backing bit view per input also adds constant in-object storage that
this heap-only probe does not count.

Complete merge runtime
----------------------

Median nanoseconds per distinct output key, with five alternating process trials
and three rounds per trial. These fifteen observations per variant are not
fifteen independent processes. The resident-memory fixture and complete-build
timer are unchanged from [the preceding report](native_encoded.md#native-merge-method-and-correctness).

| Policy | Fixture | Shared prefix | Changing tail | 40-byte spans | 16-byte spans | Change |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| byte fixed | ids | 0 | 0 | 55.064 | 53.792 | -2.31% |
| byte variable | ids | 0 | 0 | 72.001 | 76.996 | +6.94% |
| bit fixed | ids | 0 | 0 | 91.359 | 96.771 | +5.92% |
| bit variable | ids | 0 | 0 | 129.750 | 129.689 | -0.05% |
| byte fixed | ids | 64 | 0 | 56.468 | 55.166 | -2.31% |
| byte variable | ids | 64 | 0 | 75.083 | 73.985 | -1.46% |
| bit fixed | ids | 64 | 0 | 93.638 | 94.106 | +0.50% |
| bit variable | ids | 64 | 0 | 129.669 | 129.110 | -0.43% |
| byte fixed | ids | 4,096 | 0 | 56.356 | 55.562 | -1.41% |
| byte variable | ids | 4,096 | 0 | 75.500 | 75.480 | -0.03% |
| bit fixed | ids | 4,096 | 0 | 106.364 | 106.262 | -0.10% |
| bit variable | ids | 4,096 | 0 | 136.983 | 133.616 | -2.46% |
| byte fixed | ids | 0 | 4,096 | 475.098 | 489.919 | +3.12% |
| byte variable | ids | 0 | 4,096 | 453.634 | 451.731 | -0.42% |
| bit fixed | ids | 0 | 4,096 | 1185.089 | 1185.649 | +0.05% |
| bit variable | ids | 0 | 4,096 | 1245.738 | 1210.063 | -2.86% |
| byte fixed | fragments | 0 | 0 | 56.529 | 53.660 | -5.08% |
| byte variable | fragments | 0 | 0 | 70.486 | 73.120 | +3.74% |
| bit fixed | fragments | 0 | 0 | 78.217 | 77.311 | -1.16% |
| bit variable | fragments | 0 | 0 | 112.345 | 106.944 | -4.81% |

Shared-prefix medians range from −2.46% to +6.94%; the largest increases are
prefix0 byte-variable (+6.94%) and bit-fixed (+5.92%). Changing-tail medians range
from −2.86% to +3.12%, and fragment medians from −5.08% to +3.74%. All twenty
observation ranges overlap, including these increases. I retain the outliers
and report this as mixed timing evidence, with space reduction as the benefit.
I do not add these percentages to the earlier materialized-versus-encoded gains.

All 600 rows pass the original key/value oracle and exact wire/metadata/EF checks.
The [shared-prefix rows](results/native_compact_runtime_m2max.csv),
[changing-tail rows](results/native_compact_tail_m2max.csv),
[fragment rows](results/native_compact_fragments_m2max.csv), and
[range summary](results/native_compact_summary.csv) preserve the observations.
Each JSON companion records full source/header hashes, compiler commands,
platform and alternating execution order.

The complete uninstrumented harness's `__text` is 133,960 bytes before and
133,532 after (428 bytes smaller). Executable file size is 327,056 versus
313,392 bytes; section alignment and link information also affect file size.
The [raw size output and executable hashes](results/native_compact_size_m2max.json)
keep this specific consumer separate from any general code-size claim.

Reproduction
------------

Use the unchanged [harness](native_encoded.cc) and [runner](native_encoded.py)
from `012b497`, under the configured host CPU/build-directory lease. The host
is native arm64 M2 Max, AppleClang21, C++20, strict warnings and `-O3 -DNDEBUG`.
No filesystem I/O, cold mmap faults, input construction or oracle work is timed.
The QoS request is not verified. Repeat all three families:

```sh
python3 bench/native_encoded.py --baseline e21766b --candidate 81c28ef \
  --records 4096 --prefix 0 64 4096 --rounds 3 --trials 5 \
  --build-dir build-native-compact
python3 bench/native_encoded.py --baseline e21766b --candidate 81c28ef \
  --records 4096 --prefix 0 --tail 4096 --rounds 3 --trials 5 \
  --build-dir build-native-compact-tail
python3 bench/native_encoded.py --baseline e21766b --candidate 81c28ef \
  --records 4096 --prefix 0 --fixture fragments --rounds 3 --trials 5 \
  --build-dir build-native-compact-fragments
```

For the allocation experiments, add `--allocations --rounds 1 --trials 1` and
use separate build directories. Their elapsed fields are not performance evidence.
CSV line endings are normalized to LF; metadata retains the exact pins and commands.
