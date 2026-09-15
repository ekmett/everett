COLA Borrowed Payload Sharing
=============================

I kept this prototype off main: current Diet retains inline borrowed payloads
and its existing builder API. The two unconditional allocations, extra default
owner storage and inability to reuse persisted pairs do not justify adoption
from these overlapping timings.

I measured an optional in-memory reuse path for an unchanged target route. The
builder shares that route’s immutable borrowed FC/EF owner and rebuilds its ranks,
cut LCPs and false-borrow flags. It preserves the existing traversal and checks.

The prototype preserves exact complete IX03 bytes. Both-route reuse used fewer
allocations and had lower build medians in these fixtures, but every observed
timing range overlaps its corresponding control. Default build and full-query
medians were approximately flat. These measurements do not establish a general
speedup or an on-disk storage saving.

Scope and ownership
-------------------

- The optional fourth builder argument is a prior artifact of the same concrete
  index type. Each present route is eligible only when its target object pointer
  is identical. Equal content under another target object is insufficient.
- The builder retains only eligible borrowed payload owners, not the old index’s
  native source, opposite target, ranks, cuts or flags. Existing accessors retain
  their types; the borrowed array object and its FC/EF vectors share identity.
- Generic mapped-input construction artifacts work, but an existing mapped IX03
  pair is not a reuse source. Applications must retain a prior construction
  artifact to use this path. Sealing still writes both complete payloads.
- The default path creates two shared payload owners even for absent routes.
  Accessing borrowed arrays also gains a pointer indirection. The full-query
  control below measures the combined representation change, not just that load.

Method
------

Baseline: `df1cb2dce83280503d4c2276dffbcace4a26a914`. Candidate:
`63c5de6` (only `include/diet/cola_index.h`). Apple Clang 21, arm64 M2 Max,
C++20 `-O3 -DNDEBUG` and strict warnings. All heavy work used `cpu-heavy`,
with at most two concurrent compiler processes.

Each byte or partial-bit [fixture](cola_payload.cc) uses K3/K15 and W16, 2,048 local native records,
a 4,096-record main target with a deeper main and secondary, and a 4,096-record
secondary target. Keys share either 0 or 4,096 prefix bytes. Original integer IDs
independently determine every full-query match multiplicity; all values are
checked against original literals. The eight complete IX03 outputs are compared
byte-for-byte across modes, processes and implementations.

Five alternating baseline/candidate processes each perform one warm round and
three recorded rounds with the default path alone. A separate five-process
candidate run rotates default, main-only, secondary-only and both-route reuse.
I compare reuse only to its control in that second run, not to the earlier
baseline run. Reuse sources have different local native keys and, for one-route
reuse, a different opposite target.

Build timing includes constructor, all steps, finish and builder destruction;
it excludes source preparation, serialization and destruction of the returned
index. Query timing includes owned query creation, complete traversal and
consumption of all returned value bytes and ordinals, but excludes root
preparation. Each query round has 4,096 deterministic hits/misses and is preceded
by a full independent answer check. Allocation tracking runs in separate binaries
and is never used for timing.

Default-path comparison
-----------------------

Medians in ns per consumed virtual occurrence (build) or full query. Change is
candidate relative to baseline. All sixteen min–max ranges overlap; full ranges
remain in the raw rows and JSON summary.

| Unit | K | Prefix bytes | Build baseline → default | Change | Query baseline → default | Change |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| bit | 15 | 0 | 369.5 → 378.9 | +2.54% | 5863.9 → 5918.2 | +0.93% |
| bit | 15 | 4096 | 385.2 → 385.5 | +0.08% | 6778.2 → 6854.3 | +1.12% |
| bit | 3 | 0 | 315.4 → 319.1 | +1.18% | 6525.3 → 6522.4 | -0.04% |
| bit | 3 | 4096 | 315.3 → 320.6 | +1.67% | 7498.5 → 7569.3 | +0.94% |
| byte | 15 | 0 | 240.4 → 245.0 | +1.90% | 3997.4 → 3961.9 | -0.89% |
| byte | 15 | 4096 | 235.6 → 238.7 | +1.28% | 4552.3 → 4530.6 | -0.48% |
| byte | 3 | 0 | 220.0 → 213.1 | -3.14% | 4455.0 → 4468.6 | +0.31% |
| byte | 3 | 4096 | 212.3 → 214.5 | +1.07% | 5125.3 → 5142.1 | +0.33% |

Reuse within the candidate
--------------------------

Medians in ns per consumed virtual occurrence; percentages are relative to the
rotating-mode default in the same run. All ranges overlap their default control.

| Unit | K | Prefix bytes | Default | Main only | Secondary only | Both |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| bit | 15 | 0 | 366.1 | 366.6 (+0.16%) | 366.0 (-0.03%) | 347.8 (-4.99%) |
| bit | 15 | 4096 | 375.0 | 361.2 (-3.68%) | 367.2 (-2.09%) | 354.2 (-5.55%) |
| bit | 3 | 0 | 306.8 | 291.2 (-5.07%) | 292.7 (-4.59%) | 265.9 (-13.33%) |
| bit | 3 | 4096 | 311.7 | 280.9 (-9.89%) | 290.1 (-6.93%) | 267.5 (-14.18%) |
| byte | 15 | 0 | 232.0 | 230.8 (-0.52%) | 230.3 (-0.77%) | 225.6 (-2.78%) |
| byte | 15 | 4096 | 237.0 | 232.0 (-2.11%) | 230.9 (-2.58%) | 228.8 (-3.47%) |
| byte | 3 | 0 | 210.7 | 200.5 (-4.83%) | 206.2 (-2.12%) | 189.9 (-9.87%) |
| byte | 3 | 4096 | 209.9 | 198.4 (-5.50%) | 201.8 (-3.86%) | 190.9 (-9.04%) |

Allocation accounting
---------------------

The default candidate adds exactly two allocations and 528 requested heap bytes
per completed index in all eight fixtures. Its newly allocated live bytes at
return also increase by 528; peak requested live bytes are unchanged here. This
counts heap allocation only: the old representation held the array objects
inline, so it is not a total owner-size or RSS comparison. The separate layout
probe measured `sizeof(index)` as 744 → 296 bytes and `sizeof(builder)` as
2,776 → 2,808 bytes. Combining the index’s 448-byte inline reduction with its
528 additional live heap bytes gives 80 extra requested/object bytes for the
default returned owner, before allocator overhead.

Reuse counts below exclude the preexisting shared payload. They measure new
construction allocations, not total retained storage. The allocator records
requested C++ bytes, excluding allocator headers/alignment and input owners. It
is a single-threaded probe; elapsed times from its binaries are not evidence.

| Unit | K | Prefix bytes | Default calls / requested B | Both reuse calls / requested B | Default / reuse new live B |
| --- | ---: | ---: | ---: | ---: | ---: |
| bit | 15 | 0 | 99 / 16,827 | 51 / 8,999 | 8,208 / 4,512 |
| bit | 15 | 4096 | 85 / 64,017 | 51 / 29,479 | 21,572 / 4,512 |
| bit | 3 | 0 | 129 / 101,235 | 72 / 69,607 | 47,976 / 34,816 |
| bit | 3 | 4096 | 112 / 146,429 | 72 / 90,087 | 60,350 / 34,816 |
| byte | 15 | 0 | 99 / 16,804 | 51 / 8,994 | 8,192 / 4,512 |
| byte | 15 | 4096 | 87 / 63,986 | 51 / 29,474 | 21,548 / 4,512 |
| byte | 3 | 0 | 130 / 113,436 | 72 / 69,602 | 54,040 / 34,816 |
| byte | 3 | 4096 | 115 / 162,730 | 72 / 90,082 | 68,456 / 34,816 |

Correctness and reproduction
----------------------------

Independent tests cover same-content/different-object ineligibility and fallback,
present empty and absent targets, builder moves, moved-from reuse sources, old-graph release,
pointer identity, and real mapped-input artifacts that survive source unlinking.
Strict O3 ASan/UBSan tests and an O1 ASan/UBSan run of the complete harness passed.
The earlier scalar route-stability regression independently checks FC bits, EF
packing, changed ranks/cuts and local-equality flags.

Run the exact benchmark under the host resource gate:

```sh
python3 /Users/ekmett/cult/game/tools/resource_run.py --resource cpu-heavy \
  --build-dir "$PWD/build-payload-reproduce" -- \
  python3 bench/cola_payload.py --baseline df1cb2d \
  --candidate-patch bench/results/cola_payload/candidate.patch \
  --build-dir build-payload-reproduce --trials 5 --rounds 3
```

The [reproduction runner](cola_payload.py) applies the
[archived one-header patch](results/cola_payload/candidate.patch) only inside its
private candidate snapshot, then verifies **every** reconstructed header hash
against the measured candidate before compiling. No experimental Git ref is
needed in a fresh clone. The [original measured runner](results/cola_payload/measured_runner.py)
is preserved byte-for-byte so its recorded hash remains accurate; the current
runner adds only this patch-snapshot option.

The [raw metadata](results/cola_payload/results.json) retains exact source, runner,
header and binary hashes, compiler commands, execution order and every dumped
wire hash. [Timing rows](results/cola_payload/results.csv) retain all 1,080
observations; [allocation rows](results/cola_payload/allocations.csv) retain all
40 independent measurements. The small [layout probe](results/cola_payload/layout.json)
records object sizes separately. No new dependency or encoded-format change is
part of this prototype.
