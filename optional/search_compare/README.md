Whole-search offset choices
===========================

I compare offset representations inside complete mmap-backed string searches,
including the fractional cascade. This answers a different question from the
[select-only benchmark](../select_compare/README.md): how much faster does a
lookup become, and how much larger do the **complete files** become?

The [report](report.md) retains measured results, statistical attribution and
the limits of the fixtures. The production implementation is the baseline.
Candidate headers are generated into an ignored build directory; they never
replace installed headers or open existing user files.

Representations
---------------

| Variant | Stored offsets | Auxiliary selector |
| --- | --- | --- |
| `base` | Existing Elias–Fano low/high bits | Existing 256-one samples and sparse exceptions |
| `sub32` | Same Elias–Fano low/high bits | Eight 16-bit relative subpositions per dense 256-one sample |
| `packed` | Absolute offsets using $\lceil\log_2(U+1)\rceil$ bits each | None |
| `direct32` | Absolute 32-bit offsets | None; requires $U\leq 2^{32}-1$ |
| `direct64` | Absolute 64-bit offsets | None |

`sub32` changes selection within Elias–Fano. The other candidates replace the
offset representation. All preserve the native and borrowed record streams,
sampling interval, ranks, false-borrow flags and cut-LCP metadata. Fixed-value
stride arithmetic remains outside the offset codec.

The generated files are temporary benchmark formats. They reuse section slots
to let existing serializers exercise the changed views, and are incompatible
with production readers. A production choice needs an explicit per-stream
format tag, shape validation and recovery support.

Reproducing the comparison
-------------------------

Generate independent header trees, then compile each benchmark against its
own tree. `prepare.py` extracts the frozen header baseline `cb2b029` directly
from Git, so reproducing the measurements does not depend on the current
checkout. `--revision` selects another source revision; replacement checks
reject unrecognized interfaces. Use the same compiler and flags for all candidates:

```sh
python3 optional/search_compare/prepare.py build-search/base
python3 optional/search_compare/prepare.py build-search/sub32 --candidate
python3 optional/search_compare/prepare.py build-search/packed --packed
python3 optional/search_compare/prepare.py build-search/direct32 --direct32
python3 optional/search_compare/prepare.py build-search/direct64 --direct64
```

For each variant, replacing `VARIANT` below:

```sh
c++ -std=c++20 -O3 -g -DNDEBUG -Wall -Wextra -Werror \
  -Ibuild-search/VARIANT/include optional/search_compare/bench.cc \
  -o build-search/VARIANT/bench
c++ -std=c++20 -O3 -g -DNDEBUG -Wall -Wextra -Werror \
  -Ibuild-search/VARIANT/include optional/search_compare/size_panel.cc \
  -o build-search/VARIANT/size_panel
```

The primary matrix covers byte KV02 and sort-owned bit KV03 files, short and
long structured/hash keys, hits, misses, mixed queries and dependent query
selection. Each process builds its own files, verifies complete values against
an independent logical oracle, warms searches and measures three trials.
Candidates rotate across fresh processes:

```sh
python3 optional/search_compare/run.py build-search build-search/primary \
  --queries 1024 --loops 2 --trials 3 --variants base sub32 direct32
python3 optional/search_compare/run.py build-search build-search/alternatives \
  --queries 1024 --loops 2 --trials 3 --variants base packed direct64
```

The size panel grows from 1,024 to 2,097,152 base records and scales the query
set to 65,536 keys. It measures random and sorted order of the same hit set,
with separately excluded warmups. Byte/short-structured and bit/long-hash are
two distinct workloads, not a matched byte-versus-bit comparison. Run selected
sizes to split measurements into bounded quiet periods; completed files are
retained when resuming:

```sh
python3 optional/search_compare/size_panel.py build-search build-search/sizes \
  --sizes 1024 8192 32768 131072
python3 optional/search_compare/size_panel.py build-search build-search/sizes \
  --sizes 524288 2097152
```

`summarize.py` checks matching payload fingerprints and result checksums,
computes medians within each process, then compares medians of those process
medians. File growth uses the sum of unique complete `.kv` and `.index` file
lengths, including envelopes, section descriptors and alignment. It excludes
filesystem allocation granularity and temporary fixture/oracle memory.

```sh
python3 optional/search_compare/summarize.py build-search/primary \
  build-search/primary-summary.csv --phase primary
```

Attribution and checks
----------------------

`prepare.py --audit` creates a separate operation-count build. Compile
`bench.cc` with `-DEVERETT_SEARCH_AUDIT` against it. Those timings are discarded:
the counters count calls, not nanoseconds. Its `byte_frame_calls` field names
the ordinary profile parser, used for both byte natives and bit/byte borrowed
streams; `bit_frame_calls` names the sort-owned native parser.

On macOS, Instruments Time Profiler can launch only the benchmark process:

```sh
xcrun xctrace record --template 'Time Profiler' --output query.trace \
  --time-limit 20s --no-prompt --launch -- \
  ./build-search/base/bench byte 131072 16 structured 2048 1 1 10 profile
xcrun xctrace export --input query.trace \
  --xpath '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]' \
  --output query.xml
python3 optional/search_compare/profile.py query.xml profile-summary --seconds 8
```

The summarizer retains only the target main thread's final eight seconds of
query execution, using the frozen harness's lookup call site to exclude
teardown from the ten-second repeated-query phase. It strips host paths and process
identifiers. Category totals are statistical estimates, with unresolved
linker-folded frames retained in `routing_and_other`; they are not precise
per-call timings. Inclusive categories can overlap. Exclusive categories
choose the innermost recognized scope and sum to 100%.

`chronology_check.cc` adds a newer mapped run containing changed values and
tombstones, then exhaustively checks its keys and neighboring misses. It is
untimed and uses the same generated readers:

```sh
c++ -std=c++20 -O0 -g -fsanitize=address,undefined -Wall -Wextra -Werror \
  -Ibuild-search/VARIANT/include optional/search_compare/chronology_check.cc \
  -o build-search/VARIANT/chronology_check
./build-search/VARIANT/chronology_check
```

`select_check.cc` checks sequential and random selection against the original
integer vector, including empty streams, repeated offsets, 32/256-one
boundaries, clustering and sparse exceptions. Run it against each generated
tree with ASan/UBSan. Complete-query fixtures also compare exact values before
timing and validate measured checksums afterward.

Plotting
--------

With Matplotlib installed, regenerate the complete-file tradeoff plot from the
retained summary rows:

```sh
python3 optional/search_compare/plot.py \
  optional/search_compare/results/2026-09-16-m2max/primary-summary.csv \
  optional/search_compare/results/2026-09-16-m2max/alternatives-summary.csv \
  build-search/whole-search
```

This writes PNG and SVG copies plus the input and script fingerprints. Each
point is one workload median; the plot does not pool latencies across fixtures.
