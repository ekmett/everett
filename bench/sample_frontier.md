# Sampling comparisons and encoder reuse

I keep the encoder change that obtains ordering and retained-prefix length from
one comparison. I leave `sample_cursor` unchanged: the carried-prefix variants
help long shared prefixes, but their short-key costs are too large for the measured default.
The measurements below distinguish the accepted encoder-only change from those
rejected cursor variants.

I measured complete `sample_cursor` traversal and three-stage `index_pipeline`
construction with one [shared harness](sample_frontier.cc) and
[pinned-header runner](sample_frontier.py). The fixtures exercise byte and bit
profiles, short keys, long shared prefixes, native/borrowed interleaving, and
repeated borrowed keys that must remain distinct occurrences.

## Accepted encoder-only comparison

The direct comparison is `98f9616` to main `84d5495`. These header trees differ
only in `profile_sample_encoder::encode`: one `compare_common_bits` supplies
both sortedness and the retained count. The cursor is unchanged. All 720 rows
pass the same independent occurrence, encoding and counter checks.

The long-prefix pipeline medians fall by 3.2–4.6%. Prefix-0/64 pipeline changes
range from a 1.6% decrease to a 1.4% increase. All twelve pipeline observation
ranges overlap, including the long-prefix cases; the table describes shifts in
their medians. I select the smaller encoder change without claiming a general improvement for
short keys. Pipeline medians, in ns per augmented source occurrence:

| Shared prefix bytes | Fixture | Profile | Before | Encoder only | Change |
| ---: | --- | --- | ---: | ---: | ---: |
| 0 | interleaved | byte | 61.56 | 61.90 | +0.5% |
| 0 | interleaved | bit | 105.07 | 105.10 | 0.0% |
| 0 | duplicates | byte | 60.78 | 59.87 | -1.5% |
| 0 | duplicates | bit | 94.14 | 93.94 | -0.2% |
| 64 | interleaved | byte | 66.38 | 67.31 | +1.4% |
| 64 | interleaved | bit | 108.51 | 110.04 | +1.4% |
| 64 | duplicates | byte | 63.80 | 63.77 | 0.0% |
| 64 | duplicates | bit | 100.73 | 99.08 | -1.6% |
| 4,096 | interleaved | byte | 322.54 | 309.04 | -4.2% |
| 4,096 | interleaved | bit | 365.73 | 349.09 | -4.6% |
| 4,096 | duplicates | byte | 307.40 | 295.88 | -3.7% |
| 4,096 | duplicates | bit | 345.13 | 334.15 | -3.2% |

Sampling is an unchanged control in this comparison. Its medians move by
−1.7% to +5.4%, including a +5.4% prefix-64 byte interleaved observation; the
ranges overlap. This noise limits the precision we should assign to the modest
pipeline gains. I report the directly measured change, not a product of the
larger gains from rejected cursor variants. The [complete CSV](results/sample_encoder_only_m2max.csv)
and [metadata](results/sample_encoder_only_m2max.json) contain all observations.

## Method and oracle

Each fixture has 4,096 unique native keys whose integer identifiers are `4i+1`.
The interleaved fixture adds borrowed identifiers `4i` and `4i+2`, giving 12,288
augmented occurrences. The duplicate fixture adds two equal borrowed occurrences
at each native identifier, nineteen every 32nd identifier, plus `4i+2`. It has
18,560 occurrences, native-equal false borrows, and duplicate runs crossing
`K=15` sampling cuts. These are deliberately synthetic local native/index pairs;
I do not claim their supplied borrowed entries were certified against a deeper
persisted dependency graph.

Keys encode the integer in eight big-endian bytes after 0, 64 or 4,096 shared
prefix bytes. The final cursor fallback also measures 128, 256 and 512 bytes. Bit-profile keys append three meaningful bits. Values contain the
integer's decimal representation. Both profiles use `K=W=15` and variable values.
The pipeline adds native stages with 1,024, 256 and 64 records, retaining their
exact native allocations and the original target pair.

An independent stable integer sort produces the augmented occurrence order.
Equal native keys precede borrowed copies, and duplicate borrowed occurrences
retain their original ordinals. Outside the timers I check every sampled key's
complete reconstructed bytes, occurrence tag and ordinal, false-borrow flags
against integer membership, and all traversal counters against that order.

For pipelines I compute each expected incoming sample set from that integer
order, compare every native and borrowed payload byte and every EF, rank,
false-borrow and cut-LCP array against batch construction, and independently
check the reconstructed borrowed keys. Native allocation sharing, exact target
pins, and emitted/consumed counters are checked as well. Cross-revision checks
require identical sample checksums, pipeline encoding digests and work counts.

The sample timer includes cursor construction, complete traversal, and an FNV
consumer of five 64-bit scalars per emitted sample: virtual ordinal, source
ordinal, source role, key length and its last 64 meaningful bits. It does not
rehash a long inherited prefix inside the timer. The pipeline timer includes
construction, stepping and final directory construction. Destruction is outside
both timers; correctness checks and encoding hashes are also outside.

I use five fresh-process trials in alternating revision order, with three rounds
per process and three prefix lengths. Each process visits interleaved byte,
interleaved bit, duplicate byte and duplicate bit fixtures in that order. Each
round measures sampling before pipeline construction. There are 720 rows in
each three-prefix release comparison, 360 per revision; the six-prefix fallback
has 1,440 rows. Results are native arm64 AppleClang 21
`-O3 -DNDEBUG` measurements on the local M2 Max host under the exclusive CPU and
build-directory lease. They are warm in-memory measurements, not cold-mapping
or storage-I/O bounds.

## Rejected cursor comparisons

The first cursor carries exact bit LCPs and obtains each successor LCP with
`advance_comparison`. Against `98f9616`, `045702c` gives strong long-prefix gains
but increases short-key sampling time by as much as 31.1% and pipeline time by
11.7%. Rounding equal-frontier comparisons back to a byte boundary in `797a728`
preserves the long-prefix gains but leaves a 29.5% worst short-key sampling
increase and an 8.8% pipeline increase.

Inspection of the compiled arm64 path shows the additional fixed work:
`advance_impl` receives a non-null comparison output, writes that result through
stack storage, and the sampler updates its frontier scalars. The equal-frontier
path also constructs two checked suffix views before comparing them. This
identifies added work; assembly alone does not assign elapsed cycles to it.

I also tested `efb4f20`, which reads the ordinary-FC retained count directly
and carries LCPs in policy units. This is mathematically sufficient for ordering,
but byte units discard useful distinctions between keys differing inside one
byte. Short byte interleaving grows from 37.24 to 59.38 ns per source entry in
sampling, and from 63.54 to 88.00 ns in pipeline construction. The bit cases are
much closer because their units retain every bit distinction.

An [independent integer model](results/sample_frontier_decisions.json) explains
the extra comparisons: exact-bit frontiers need 4,096 suffix comparisons in the
interleaved fixture; byte-unit frontiers need 12,224, out of 12,287 both-head
decisions. For duplicate runs the count grows from 4,096 to 8,128. The model uses
`64 - bit_width(a xor b)` for integer-key LCP, divided by eight for byte units;
a shared fixed prefix changes no equality decision. These are logical work
counts, not measured cycles. I would not select this coarser byte frontier from
these results. Its [CSV](results/sample_frontier_units_m2max.csv) and
[metadata](results/sample_frontier_units_m2max.json) preserve all 720 rows.

The final cursor fallback, `ad92282`, carries exact bit LCPs only when the
current heads agree for at least 1,024 bits. Below that threshold it advances
normally and resets both frontiers, forcing a fresh comparison. I checked that
reset and carried states both compute an exact current head-to-head LCP; the
threshold does not weaken ordering, tie handling or source occurrence identity.
All 1,440 timing rows passed the independent output and counter checks.

The fallback still increases prefix-0/64 sampling medians by 33–70% and pipeline
medians by 20–36%. It does not restore the original short-key path merely by
turning off the additional adjacent-key comparison. The byte interleaved
crossover makes the tradeoff visible (ns per augmented source occurrence):

| Shared prefix bytes | Sampling before | Sampling fallback | Change | Pipeline before | Pipeline fallback |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 36.84 | 61.12 | +65.9% | 63.11 | 83.22 |
| 64 | 38.31 | 63.55 | +65.9% | 65.77 | 86.99 |
| 128 | 40.68 | 48.69 | +19.7% | 70.88 | 73.37 |
| 256 | 45.28 | 48.95 | +8.1% | 78.50 | 75.75 |
| 512 | 57.11 | 47.34 | −17.1% | 95.26 | 79.98 |
| 4,096 | 198.43 | 48.37 | −75.6% | 322.81 | 164.82 |

These long-prefix gains do not justify adopting this fallback for short keys.
I stop the cursor experiments here. The [complete fallback CSV](results/sample_frontier_fallback_m2max.csv)
and [metadata](results/sample_frontier_fallback_m2max.json) retain the byte/bit,
interleaved/duplicate measurements and exact source hashes.

## Separate encoder isolation

The separate encoder change `045702c` to `c40a715` reuses one comparison for both
ordering and retained-prefix length. Its long-prefix pipeline medians fall by
7.1–8.4%, while short-prefix pipeline changes range from a 2.7% decrease to a
2.2% increase with noisy overlapping trials. Sampling is an unchanged control
in that comparison. These percentages are measured on the first carried-bit
cursor, so they are not the encoder-only improvement over the original cursor.
I do not multiply speedups from separate runs to estimate a combined result.

These exact intermediate measurements are retained:

- Initial carried-bit cursor: [CSV](results/sample_frontier_initial_m2max.csv), [metadata](results/sample_frontier_initial_m2max.json).
- Byte-aligned comparison start: [CSV](results/sample_frontier_aligned_m2max.csv), [metadata](results/sample_frontier_aligned_m2max.json).
- Encoder comparison reuse: [CSV](results/sample_encoder_m2max.csv), [metadata](results/sample_encoder_m2max.json).
- Baseline-versus-baseline harness ASan/UBSan check: [CSV](results/sample_frontier_check.csv), [metadata](results/sample_frontier_check.json). This check uses 128 native records, one round/trial and all three prefixes; its instrumented times are not performance measurements.

## Reproduction and limits

The measured harness is revision `39f7ba34f46503f986326df61e965b28f16b06dc`:

- C++ SHA-256: `75ecc04140379f88d140d066dbb46225b897d925c5f2a8044054053715f89c76`.
- Runner SHA-256: `d424b4ca2836f079117b14222d63b4ee29d46497cd1abbcf0f81a3858ba8a3c3`.

Each metadata file records the complete baseline/candidate revisions, every
header's SHA-256, compiler version and exact command, host platform, trial order
and timestamps. The archived CSV files normalize line endings to LF without
changing any field. The runner snapshots the same harness and both complete header
trees before compiling. The accepted comparison is reproducible from the
main-reachable source revisions:

```sh
python3 bench/sample_frontier.py \
  --baseline 98f9616 --candidate 84d5495 \
  --records 4096 --prefix 0 64 4096 --rounds 3 --trials 5 \
  --build-dir build-sample-encoder-only
```

I run that command inside the host's configured exclusive CPU/build-directory
lease. `CXX` accepts a compiler command such as `ccache clang++`. For the harness
sanitizer check, use `--sanitize --records 128 --rounds 1 --trials 1` with both
revisions set to `98f9616`. Rejected cursor revisions are identified in their
metadata; those experimental Git objects must be available to rerun them.

The measurements do not establish a workload-independent speedup, mmap page
fault behavior, or a scheduler bound. Five process trials and three inner rounds
are observations on this host, not independent hardware samples. The rejected prototypes preserve the public `key_comparisons` counter as the
baseline count of decisions with both source heads present, even when unequal
cached LCPs resolve a decision. It is not a count of bytes compared. Equality of
that counter verifies its existing meaning rather than claiming that the
rejected frontiers do the same physical comparison work.
