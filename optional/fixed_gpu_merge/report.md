Fixed-key merge measurements
============================

The stronger CPU baseline narrows the GPU advantage substantially. With
262,144 structured keys in each input and variable values from zero through
512 bytes, the forward CPU merge takes about 14.2 ms without cancellation;
Metal takes 6.1–7.2 ms. The original CPU baseline took about 37 ms. I retain
both so that improved CPU decoding is visible rather than attributed to the
GPU.

The current [raw run](results/forward/results.csv),
[repeat run](results/forward/results-repeat.csv),
[metadata](results/forward/metadata.json) and
[source/binary hashes](results/forward/source-hashes.json) use source revision
`3915732f08eeb342e4093960d2b41a4224aa75d7` (the metadata contains the full verified revision).
The equivalent main revision is `5077bbc`; every listed source hash matches.
Each run checks 86 cases, including 62 benchmark cases with five measured
iterations each. All original-CPU, forward-CPU and GPU output files matched
byte for byte. The 24 smaller cases also passed CTest and host ASan/UBSan with
real Metal execution.

The host is an Apple M2 Max running macOS 26.6.2. These are **resident complete
native merges**, including temporary memory, command submission, synchronization,
real copying and native EF output. The [experiment README](README.md) defines
the timing boundaries. They exclude checksums, content identity, fractional
indexes, durable sealing and catalog publication. These results do not measure
persistent `.ff`/`.fv` files in the production runtime.

Forward CPU comparison
----------------------

The forward decoder walks high-vector set bits in order, caches shared value
boundaries, and skips canceled ranges by whole-word popcount where possible.
It avoids consulting the select directory for every record. Output encoding
still uses the same production `elias_fano::build` as the original baseline.

Here are ranges of the two run medians, not best individual iterations:

| Layout | Records per input | Older cancellation | Forward CPU ms | GPU total ms | CPU/GPU |
| --- | ---: | ---: | ---: | ---: | ---: |
| `.ff`, 16-byte values | 262,144 | 0% | 4.29–4.34 | 1.80–2.07 | 2.09–2.38 |
| `.ff`, 16-byte values | 262,144 | 50% | 3.10–3.15 | 1.74–1.81 | 1.74–1.78 |
| `.ff`, 16-byte values | 262,144 | 90% | 2.25–2.38 | 1.53–1.62 | 1.47–1.47 |
| `.fv`, 0–512-byte values | 262,144 | 0% | 14.16–14.27 | 6.13–7.21 | 1.97–2.33 |
| `.fv`, 0–512-byte values | 262,144 | 50% | 9.75–9.94 | 5.30–6.74 | 1.45–1.88 |
| `.fv`, 0–512-byte values | 262,144 | 90% | 7.31–7.46 | 4.32–4.40 | 1.69–1.69 |
| `.fv`, 0–6-byte values | 262,144 | 0% | 7.51–7.54 | 3.31–3.32 | 2.27–2.27 |
| `.fv`, 0–6-byte values | 262,144 | 50% | 5.42–5.67 | 2.46–2.81 | 1.93–2.31 |
| `.fv`, 2048–4096-byte values | 32,768 | 0% | 8.46–8.80 | 4.71–6.15 | 1.43–1.80 |
| `.fv`, 2048–4096-byte values | 32,768 | 50% | 5.94–6.06 | 3.59–4.60 | 1.32–1.65 |

Observed brackets
-----------------

For a repeatable median margin, I require a CPU-forward/GPU ratio of at least
1.2 in **both** runs. The table gives the first tested size meeting that rule.
Individual timing ranges sometimes overlap, and GPU outliers remain visible in
the raw data; this is a median criterion, not a latency guarantee or an exact
crossover.

| Scenario | Older cancellation | Previous tested size per input | First tested size per input with the margin |
| --- | ---: | ---: | ---: |
| `.ff`, 16-byte values | 0% | 65,536 | 131,072 |
| `.ff`, 16-byte values | 50% or 90% | 131,072 | 262,144 |
| `.fv`, 0–512-byte values | 0%, 50% or 90% | 65,536 | 131,072 |
| `.fv`, 0–6-byte values | 0% or 50% | 65,536 | 131,072 |
| `.fv`, 2048–4096-byte values | 0% or 50% | 16,384 | 32,768 |

At 4,096 records per input the CPU wins throughout. The larger-payload result
shows why payload bytes must join record count in any dispatch rule. These
brackets apply to these structured keys, profiles and merge implementation;
they do not install a production threshold.

Skew still matters
------------------

These cases cancel half the older input. The stronger baseline makes the
older-heavy fixed-value case a clear CPU win:

| Layout | Older records | Newer records | Forward CPU ms | GPU total ms | CPU/GPU |
| --- | ---: | ---: | ---: | ---: | ---: |
| `.ff` | 262,144 | 4,096 | 1.105–1.106 | 1.48–1.51 | 0.73–0.75 |
| `.ff` | 4,096 | 262,144 | 1.70–1.71 | 1.58–1.59 | 1.08–1.08 |
| `.fv` | 262,144 | 4,096 | 3.403–3.404 | 2.82–2.87 | 1.19–1.21 |
| `.fv` | 4,096 | 262,144 | 6.102–6.105 | 4.07–4.69 | 1.30–1.50 |

Scattered half-cancellation with 262,144 records per input gives 1.53–1.78 times
for `.ff` and 1.49–2.07 times for `.fv`. The same cancellation count can produce
different locality. I would use a conservative CPU fallback around marginal
cases while measuring more profiles.

The one-word payload variant remains slower than the four-word variant, but
its timings also vary: the no-cancel large `.fv` merge takes 7.89–13.01 ms with
one word per invocation, versus 6.13–7.21 ms with four. This is a concrete
optimization opportunity rather than a claim that payload copying itself is
inherently that expensive.

Retained random-select baseline
-------------------------------

I measured the [complete native merge experiment](README.md) on an Apple M2 Max
using source revision `c97e72fa9b3e5d2ba69f0f5b01b27ab2f85e7cb6`.
The [raw results](results/baseline/results.csv), [repeat run](results/baseline/results-repeat.csv),
[metadata](results/baseline/metadata.json) and
[source/binary hashes](results/baseline/source-hashes.json) preserve that run.
Both runs passed every complete-output comparison.

These are resident scratch-file merges. GPU totals include temporary allocation
and reclamation, both command submissions, synchronization, cancellation,
partitioning, input EF decoding, real value copying, output EF construction
and envelope assembly. Setup, clipping and durability have the separate
boundaries described in the experiment README.

The CPU baseline uses a single-thread streaming key merge and the production
EF output encoder. Its input access calls random-access EF select twice per
surviving variable-value record. That is a meaningful limitation of these
initial numbers: a sequential boundary decoder is a stronger CPU baseline.

Balanced inputs
---------------

Each interval below is the range of the **two run medians**, with five measured
iterations per median. Values in `.fv` vary from zero through 512 bytes;
`.ff` values are 16 bytes. Cancellation percentages apply to the older input.

| Layout | Records per input | Canceled | CPU ms | GPU total ms | CPU/GPU |
| --- | ---: | ---: | ---: | ---: | ---: |
| `.ff` | 262,144 | 0% | 4.29–4.59 | 1.59–1.63 | 2.70–2.82 |
| `.ff` | 262,144 | 50% | 3.06–3.15 | 1.64–1.81 | 1.74–1.87 |
| `.ff` | 262,144 | 90% | 2.27–2.39 | 1.41–1.68 | 1.43–1.61 |
| `.fv` | 262,144 | 0% | 36.32–36.64 | 5.58–7.11 | 5.15–6.51 |
| `.fv` | 262,144 | 50% | 22.99–23.24 | 5.18–5.36 | 4.29–4.49 |
| `.fv` | 262,144 | 90% | 17.04–17.15 | 4.07–4.15 | 4.10–4.22 |

At 4,096 records per input the CPU wins for both layouts. At 65,536 per input,
fixed values are borderline without cancellation and slower on the GPU with
cancellation. Variable values favor the GPU by roughly 2.0–3.2 times against
this CPU baseline. These points bracket different regimes; they do not locate
a production crossover.

Skew and scattered cancellation
-------------------------------

The tables are not always balanced. These cases cancel half the older input:

| Layout | Older records | Newer records | CPU ms, first run | GPU total ms, first run | CPU/GPU, both runs |
| --- | ---: | ---: | ---: | ---: | ---: |
| `.ff` | 262,144 | 4,096 | 1.08 | 1.03 | 0.75–1.05 |
| `.ff` | 4,096 | 262,144 | 1.60 | 1.35 | 1.05–1.19 |
| `.fv` | 262,144 | 4,096 | 7.84 | 2.75 | 2.75–2.85 |
| `.fv` | 4,096 | 262,144 | 15.00 | 4.10 | 3.66–3.75 |

The older-heavy `.ff` case changes sides between runs. A count-only GPU rule
would be premature. Scattered half-cancellation with 262,144 records in each
input gives 1.71–1.83 times for `.ff` and 5.31–5.51 times for `.fv` against
the initial baseline; locality affects both paths.

Payload tiling
--------------

For the large `.fv` inputs, retaining a one-word payload variant exposes the
cost of searching output boundaries too often:

| Older cancellation | One-word GPU total ms | Four-word GPU total ms |
| --- | ---: | ---: |
| 0% | 10.06–10.13 | 5.58–7.11 |
| 50% | 7.57–7.76 | 5.18–5.36 |

The four-word version performs one initial boundary search per 16-byte output
tile and uses word loads where one value covers a whole output word. Both
versions include the same complete native output and pass identical oracles.
This is implementation cost we can improve, separate from the unavoidable
payload copy.

The result is enough to justify further fixed-key merge work. It is not yet a
policy for production scheduling, a persistent `.ff`/`.fv` implementation, or
a claim about a durable merge's end-to-end latency.

-Edward Kmett
