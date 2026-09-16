Header-only GPU cutover
======================

I calibrate the CPU/GPU choice for a particular device, backend, and checked
implementation. Record count alone misses large values and unequal inputs, so
the optional selector also uses payload bytes, complete file bytes, their
imbalance, fixed value-width metadata, and the terminal key length. It never
walks records, selects EF offsets, reconstructs keys, or reads value payloads.

The [M2 Max calibration](cutover-report.md) records all 50 measured cases and
the resulting frozen rule, including its held-out checks and execution identity.

The terminal key length is one observed header field. It is not a maximum over
all keys. The selected backend still checks framing and resource bounds. A
successful selection is a performance heuristic, not permission to skip those
checks.

Calibration
-----------

`cutover_cases.h` defines 34 training and 16 held-out cases. We vary record counts,
input imbalance, common key prefixes, overlap, and fixed or variable value widths.
Equal record counts include several byte sizes. Each case verifies one warm-up
and then alternates three timed CPU/GPU runs. Both totals include mapped output
construction and CRC; neither includes durability barriers. Input construction,
pipeline setup, and canonical-byte verification are outside those totals.

The host's `cutover` mode must select its final checked variant. For example,
a Metal host can be collected with:

```sh
python3 calibrate_cutover.py collect \
  --output build/cutover-measurements \
  --backend metal --variant-label checked-optimized \
  --artifact /path/to/prototype --artifact /path/to/kernels.metallib \
  --command-json '["/path/to/prototype","/path/to/kernels.metallib","{directory}","cutover","{case}","{trials}"]'
python3 calibrate_cutover.py analyze \
  --input build/cutover-measurements --output build/cutover-model
```

The collector shuffles the case order, preserves raw logs, limits each subprocess
to 180 seconds, and hashes execution artifacts before and after the run. The
manifest records the GPU token, CPU model, OS, backend, and driver. Vulkan hosts
can use the same CSV fields and collector with their own command and an explicit
`--driver` version. Metal and Vulkan measurements do not share a calibration.
Observed device tokens are not assumed to survive a reboot.

The analysis fits a tree of at most three decisions using training cases only.
A GPU leaf needs at least two training cases, each with its slowest GPU repetition
at least 10% faster than its fastest CPU repetition. The script writes the frozen
training rule before evaluating held-out cases. Held-out results never adjust a
threshold. The exported calibration is enabled only when at least two held-out
cases select GPU and every selected case clears the same margin; otherwise the
exported selector stays on CPU.

`report.md` describes the rule, `calibration.json` contains its exact inputs,
and `calibration.h` contains its optional C++ representation. No empirical
threshold is built into `cutover.h` itself. Unknown device/backend/implementation,
an unvalidated model, or features outside the observed training envelope select
CPU. Artifact, CPU, OS, or driver changes require a matching execution fingerprint
and new measurements.

Scope
-----

The current calibration fixtures use the default code-zero string replacement
profile with sorted, unique keys within each input. Their key shapes and byte
sizes are a sample of possible workloads. The held-out checks are empirical
evidence for that sample, not a guarantee for every workload inside its numeric
envelope. The selector is optional and does not change the durable runtime's
publication, ownership, or barrier rules.
