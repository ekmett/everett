Fixed-key merge measurements
============================

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
