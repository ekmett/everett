Offset representations in complete searches
==========================================

The useful comparison is whole-search throughput against growth in complete
file size. Elias–Fano can be much smaller than a direct directory while that
difference is still a small fraction of the encoded table. I measure both.

The [space-versus-throughput plot](results/2026-09-16-m2max/whole-search.svg)
shows every workload median in the primary comparisons. The
[PNG copy](results/2026-09-16-m2max/whole-search.png) and
[plot provenance](results/2026-09-16-m2max/whole-search.json) are retained too.

Whole-search results
--------------------

Absolute offsets improve the whole lookup on this host. Packed offsets keep
most of the direct directory's gain with less space; denser sampling inside
Elias–Fano gives no useful aggregate whole-query gain in these fixtures.

| Offset choice | Byte throughput | Byte whole-file growth | Bit throughput | Bit whole-file growth |
| --- | ---: | ---: | ---: | ---: |
| Existing Elias–Fano | $1.000\times$ | Baseline | $1.000\times$ | Baseline |
| EF with 32-one subentries | $1.002\times$ | 0.003–0.067% | $0.997\times$ | 0.003–0.071% |
| Packed absolute | $1.094\times$ | 0.016–0.237% | $1.041\times$ | 0.016–0.245% |
| Direct 32-bit | $1.110\times$ | 0.079–0.473% | $1.055\times$ | 0.064–0.529% |
| Direct 64-bit | $1.105\times$ | 0.246–1.214% | $1.047\times$ | 0.229–1.309% |

Throughput is the geometric mean of 48 ratios of median query times per
profile. Space is the range over the corresponding fixtures. Each candidate
uses its own interleaved, contemporaneous baseline; the first phase tests
`sub32`/`direct32`, and the second tests `packed`/`direct64`. Small differences
between candidates from different phases should not be treated as a ranking.

All 96 direct32, 96 packed and 96 direct64 case medians beat their baseline.
Their process-median ranges are disjoint in the candidate's favor in 89, 78
and 86 cases respectively. Sub32 has only one disjoint favorable case and four
disjoint unfavorable cases; the rest overlap. These observations describe
repeatability across three processes, not statistical significance guarantees.

For one concrete example, byte storage with 131,072 base records and structured
16-byte keys occupies 6,762,352 bytes across its complete files with the existing
EF directory. Direct32 occupies 6,794,352 bytes: **32,000 extra bytes, or 0.473%**.
The offset arrays themselves grow from 18,160 to 50,160 bytes. Reporting only
their $2.76\times$ increase would obscure the useful tradeoff.

The [primary summary](results/2026-09-16-m2max/primary-summary.csv) and
[alternative summary](results/2026-09-16-m2max/alternatives-summary.csv) retain
every query kind, key shape, process range, absolute time and complete-file
denominator. The separate geometric-size panel increases both record count and
query working set; its results should be consulted before choosing a cutoff.

Scope and method
----------------

All measurements use an Apple M2 Max and Apple Clang 21.0.0, with C++20,
`-O3 -g -DNDEBUG -Wall -Wextra -Werror`. The production header baseline is
`cb2b029a657d2bbcd7135265cb3060b2b11cd961`. Candidate overlays change the offset
codec only. The source and binary hashes are retained alongside the results.
The [runner and reproduction instructions](README.md) describe the variants.
The measured count decoder is stateless; these profiles do not include the
subsequent reservoir reader change.

Each fixture maps real KV02 or KV03 files and IX03 fractional indexes. It has
four main native runs of $N$, $N/4$, $N/16$ and $N/64$ records, a terminal
secondary of $N/128$ records, and enough empty-native routing catalogs to bound
the head by $K=15$. Records have 16- or 128-byte keys and 32-byte values. Keys
are either a long shared prefix followed by an ordered integer or a deterministic
hash-like prefix with a unique integer suffix. Newer runs repeat subsets of the
base keys. Replacement reads use the existing `first_value` implementation,
including query encoding, exact cascade routing, record parsing, comparisons,
value decoding, allocation and destruction.
Every generation stores the same value for a given key. This catches wrong-key
selection, but does not independently test choosing the newest of unequal
replacement values. A separate untimed mapped-query check adds unequal newer
values and tombstones, described below.

The primary matrix uses 8,192 and 131,072 base records, both key shapes and
lengths, both byte/bit profiles, and hit/miss/mixed queries. The independent
order uses a precomputed pseudorandom query list. In the dependent order, the
previous returned value chooses the next query. Queries are precomputed before
timing; query encoding remains inside each lookup. The fixture checks exact
returned strings against the logical generator before timing, then checks
every timed aggregate against an independent oracle using the same recurrence.

Each primary comparison uses three fresh processes per variant and fixture,
with three trials of 2,048 lookups per query/access combination. I first take
the median within each process, then the median across process medians. The
process-median ranges show repeatability; they are not confidence intervals.
Warm queries reuse 1,024 query keys. They do not establish cold-file or
large-working-set behavior; the separate size panel increases the query set.

The denominator for space is the sum of **unique complete serialized `.kv`
and `.index` file lengths**, including headers, descriptors and alignment.
It excludes filesystem block allocation, directory entries and fixture memory.
Offset payload and auxiliary bytes are also reported separately. All variants
have identical record-stream lengths and fingerprints and return identical
logical values. The direct representations replace Elias–Fano; `sub32` changes
only the select directory. No faster in-word select result is presented as a
replacement-representation result.

Statistical attribution
-----------------------

I sampled four baseline query loops with Instruments Time Profiler at a 1-ms
sampling interval. The profiler launched only the benchmark process. Each
fixture repeats 2,048 mixed dependent queries for ten seconds; the retained samples
come from the main thread's final eight seconds of query execution. The final
sample at the frozen lookup call site excludes later teardown. Setup, mapped-file construction
and the initial oracle pass finish before this interval. These profiled
executions are separate from the throughput measurements.

| Baseline workload, $N=131,072$ | EF navigation | Rank projection | Frame decoding | Key comparison | Other |
| --- | ---: | ---: | ---: | ---: | ---: |
| Byte, structured 16-byte keys | 9.47% | 2.40% | 19.30% | 22.75% | 46.08% |
| Byte, hash-like 128-byte keys | 8.15% | 2.06% | 24.11% | 18.89% | 46.80% |
| Bit, structured 16-byte keys | 4.65% | 0.97% | 66.59% | 10.84% | 16.95% |
| Bit, hash-like 128-byte keys | 4.63% | 1.05% | 68.59% | 9.50% | 16.24% |

These are **exclusive sampled categories**, rounded independently. EF navigation
includes low-bit decoding and validation, not just selecting a high one.
Framing includes parsing skipped records, not just the final compared window.
Other includes routing/context management, query encoding, value materialization,
allocation, the small harness cost and unresolved linker-folded frames. The
sanitized evidence retains resolved value/query categories separately; tiny
values there are not precise measurements of all value-copy costs.

Inclusive categories in the raw summaries can overlap. For example, a bit
frame parser calls a comparison helper; its inclusive frame-decoding fraction
is about 70–72%, while the exclusive frame fraction above is about 67–69%.
I do not add overlapping inclusive percentages. Sampling uncertainty, inlining
and symbol folding limit finer attribution; no timer is added to each select.

A separately compiled counter audit performs the primary 1,024-query lists without using
its elapsed times as performance data. For these large mixed/dependent cases,
byte queries average 11.29 offset selections, 11.55 rank calls and about
102 ordinary frame parses. Bit queries average about 12.4 selections, the same
rank count, about 46 ordinary borrowed-frame parses and 56 sort-owned native
frame parses. Both perform about 0.59 value materializations per query; the
dependent recurrence changes the mix from the original half-hit list. Those
counts help explain why improvements to one small selector do not translate
directly into its microbenchmark speedup.

Construction and applicability
------------------------------

The retained space rows include native and index preparation times. They cover
encoding, directory construction, record-stream fingerprinting, checksums,
temporary file writes and metadata-only mapping. They exclude durability
barriers, and are not isolated offset-builder measurements. The
[select-only report](../select_compare/results/2026-09-16-m2max/report.md)
separately measures offset construction on extracted real directories.

The files are resident and reused. These measurements do not cover storage
faults, durable publication, merge throughput, range initialization, arbitrary
arrow composition, x86 or GPU selection. The primary and size panels record
their different process counts explicitly. Throughput speedup is
$t_{\mathrm{baseline}}/t_{\mathrm{candidate}}$; for example, $1.10\times$ is
10% greater throughput and about 9.1% less elapsed time.

A production per-file choice needs a tagged offset directory, independent of
the sort registry. Its common interface needs `size`, `universe`, `select` and
a forward cursor. Native and each borrowed stream may choose independently;
unit conversion and fixed-value stride restoration stay in the caller. Opening
checks the tag and section shape without scanning every offset; explicit
recovery checks order, bounds and endpoints. `direct32` eligibility concerns
the **residual universe in the stream's byte or bit units**, not total file
length. Wider universes need another representation. The experimental overlays
are evidence for that choice, not a production wire-format proposal.
Each measured binary specializes all streams to one representation. Runtime
tag dispatch in a mixed-representation reader has not been measured here.
Metadata opening/shape checks finish before lookup timing and are included
only in the broader preparation figures.

Correctness and evidence
------------------------

The five selectors pass 213,460 sequential/random oracle selections each under
ASan/UBSan, including empty/repeated inputs, 32/256-one boundaries, clustered
offsets and sparse exceptions. Each variant also passes 16,384 mapped
changed-value, tombstone and neighboring-miss checks under ASan/UBSan, across
both byte/bit profiles and short structured/long hash-like keys. This separate
untimed check verifies newest-value selection without changing the measured
fixtures. Its [source and binary hashes](results/2026-09-16-m2max/chronology-checks.json)
are retained. Complete query fixtures check exact values,
then timed checksums. The analysis checks matching record-stream fingerprints,
result checksums and space across processes. Raw observations, source/header
hashes, binary hashes, operation counts and sanitized profiler samples are in
[the retained artifact manifest](results/2026-09-16-m2max/manifest.json).

- Raw timer observations: [primary CSV](results/2026-09-16-m2max/primary-queries.csv.gz),
  [alternative CSV](results/2026-09-16-m2max/alternatives-queries.csv.gz).
- Complete-file and preparation observations: [primary space CSV](results/2026-09-16-m2max/primary-space.csv.gz),
  [alternative space CSV](results/2026-09-16-m2max/alternatives-space.csv.gz).
- [Untimed operation counts](results/2026-09-16-m2max/operation-counts.csv) and
  [measurement provenance](results/2026-09-16-m2max/provenance.json).
