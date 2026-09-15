# Specializing cursor advancement

I compared ordinary cursor advancement with a compile-time choice between
ordinary advancement and advancement that also computes the exact adjacent-key
comparison. In this run the change is broadly flat in complete sampling and
pipeline timings. I do not infer an end-to-end speedup from removing the runtime
comparison-pointer branch.

## Isolated change and correctness

The baseline is main `d1cc57368df10ffcade606b14ab40d8e6428e97d`, which already
contains the accepted sample-encoder comparison reuse and validated-subview
change. The candidate is `35be933dffba607e3a482a50f07a3f8c983e2ce7`: the complete
header trees differ only by four substitutions in `profile_cursor`:

- `advance()` calls `advance_impl<false>(nullptr)`.
- `advance_comparison()` calls `advance_impl<true>(&result)`.
- `advance_impl` has the compile-time `bool Compare` parameter.
- Its comparison block uses `if constexpr (Compare)`.

The candidate has the same Git tree as `7918b1c2b768d76e21efe445c8fd790c6af2db25`
(tree `7a09160749190760163bec5bc9e708421b6ebd00`). These names identify the same
content, including the common encoder change. The private call sites supply a
valid output pointer exactly when that comparison block exists; the previous
validation, reconstruction, errors and endpoint handling remain in both modes.

I use the unchanged [sampling harness](sample_frontier.cc) and
[runner](sample_frontier.py), with the fixtures and independent oracles described
in [the sampling report](sample_frontier.md#method-and-oracle). There are 4,096
native keys, byte/bit profiles, interleaved and duplicate-borrow fixtures, and
0/64/4,096 shared prefix bytes. Five alternating process trials with three rounds
each produce 720 rows. Every occurrence, counter and exact pipeline-encoding
check passed. Separate scoped O3 ASan/UBSan sampling and native-merge tests also
passed for this header change.

## Results

These are median ns per augmented source occurrence on the same native arm64
M2 Max host, AppleClang 21 `-O3 -DNDEBUG`, under the exclusive CPU/build-directory
lease. Sampling changes range from −3.0% to +2.0%. Short-prefix pipeline changes
range from −4.1% to +4.3%; long-prefix changes range from −0.7% to +0.7%. Every
baseline/candidate observation range overlaps. I find no material regression in
this matrix and no defensible speedup claim.

| Prefix bytes | Fixture | Profile | Sample before | Sample bool | Pipeline before | Pipeline bool | Pipeline change |
| ---: | --- | --- | ---: | ---: | ---: | ---: | ---: |
| 0 | interleaved | byte | 35.48 | 35.49 | 60.79 | 61.28 | +0.8% |
| 0 | interleaved | bit | 66.87 | 67.65 | 101.16 | 105.51 | +4.3% |
| 0 | duplicates | byte | 35.22 | 35.92 | 57.16 | 58.69 | +2.7% |
| 0 | duplicates | bit | 60.99 | 60.87 | 93.52 | 93.41 | -0.1% |
| 64 | interleaved | byte | 37.60 | 37.33 | 65.99 | 64.56 | -2.2% |
| 64 | interleaved | bit | 71.60 | 69.44 | 110.83 | 106.33 | -4.1% |
| 64 | duplicates | byte | 37.28 | 37.80 | 61.72 | 61.81 | +0.1% |
| 64 | duplicates | bit | 64.21 | 63.36 | 101.38 | 98.48 | -2.9% |
| 4,096 | interleaved | byte | 198.33 | 198.29 | 309.19 | 308.43 | -0.2% |
| 4,096 | interleaved | bit | 231.96 | 229.07 | 350.62 | 353.03 | +0.7% |
| 4,096 | duplicates | byte | 197.20 | 197.01 | 298.12 | 299.56 | +0.5% |
| 4,096 | duplicates | bit | 227.09 | 226.76 | 333.58 | 331.34 | -0.7% |

The complete executable's `__text` section is 168,764 bytes before and 168,128
after (636 bytes smaller, 0.38%). Its `__TEXT` segment remains 196,608 bytes;
exception tables and unwind information are unchanged. These are this harness's
code-size results; another program can instantiate a different mix of the two
advance modes. The raw [size output and executable hashes](results/sample_advance_size_m2max.json)
make the measurement explicit. I did not separately measure compilation time.

A separate small object-file probe explicitly instantiates byte/bit native
cursor `advance` and `advance_comparison`. Its ordinary function bodies shrink
from 548/504 bytes to 208/208 bytes, with stack frames shrinking from 224 to 144
bytes. The comparison-mode bodies are 544/500 bytes. Instantiating both modes
increases this probe's total `__text` from 9,972 to 10,452 bytes (+480) and compact
unwind from 3,648 to 3,904 bytes (+256); exception tables and strings are unchanged.
That object-file result and the complete harness's smaller code section describe
different consumers. The [probe source, commands, symbols and hashes](results/sample_advance_probe_m2max.json)
and its [baseline assembly](results/sample_advance_probe_baseline.asm.txt) and
[candidate assembly](results/sample_advance_probe_candidate.asm.txt) preserve the
evidence. No timing is inferred from those function or frame sizes.

## Separate subview assembly probe

The baseline above already contains the accepted `bit_view::subview` change.
Its separate earlier probe compares `f072b1c` with `37b8b28`: after checking the
requested range, a subview reuses its validated backing span instead of repeating
the original view constructor's capacity checks. In that small arm64 probe the
successful path goes from three conditional branches to one and from a 16-byte
stack frame to no frame. This is assembly evidence, with no attributed timing
measurement. The [probe metadata and exact source](results/sample_advance_probe_m2max.json),
[before assembly](results/sample_subview_probe_baseline.asm.txt) and
[after assembly](results/sample_subview_probe_candidate.asm.txt) keep it separate
from the bool-specialization benchmark.

## Reproduction

The [CSV](results/sample_advance_m2max.csv) preserves all 720 observations; the
[metadata](results/sample_advance_m2max.json) records full revisions, every header
hash, the shared harness/runner hashes, compiler commands, host and trial order.
Only CSV line endings are normalized to LF. Run inside the host's configured
CPU/build-directory resource lease:

```sh
python3 bench/sample_frontier.py \
  --baseline d1cc573 --candidate 35be933 \
  --records 4096 --prefix 0 64 4096 --rounds 3 --trials 5 \
  --build-dir build-sample-advance-bool
```

The sample timer includes the five-scalar result consumer; the pipeline timer
includes construction and finalization. Full correctness checks and destruction
remain outside the timers. These warm in-memory observations do not establish
cold mmap, storage-I/O, other-architecture or workload-independent performance.
