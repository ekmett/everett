Retained select measurements
===========================

The [report](report.md) summarizes measurements of the same exact offset
sequences. [The experiment guide](../../README.md) describes the candidates,
validation contracts, construction boundaries, external source, and regression.

- `manifest.json`: measured source, binary, source-file hashes, external-source
  hashes, generated boundary-patch hashes, compiler, host, and timing schedule.
- `sequences.json`: all 192 actual exported directories, six replay recipes,
  three boundary recipes, and exact sequence hashes/extents.
- `raw.tar.gz`: every process CSV and stderr, including excluded warmups,
  failed/ineligible candidates, and export metadata. No timing observations were
  dropped. `raw-sha256.json` checks each original uncompressed member.
- `summary.json.gz`: per-sequence/candidate/access statistics, process medians,
  immutable-array bytes, allocated capacity and object bytes, construction times.
- `checks.json`: complete-group, hash, and cross-candidate checksum validation.
- `verification.json` and `qualification.txt`: source qualification and the
  expected upstream Half boundary failure.
- `host.json`: supplemental read-only host query, needed because sandboxed
  `sysctl` was denied during initial collection.
- `sha256.json`: hashes of every other retained evidence file.

To regenerate the tables, copy this directory to scratch storage, unpack
`raw.tar.gz` there, and run `optional/select_compare/analyze.py` on that copy.
The analyzer verifies all original raw hashes, then regenerates `summary.json`,
`checks.json`, and `report.md`. Regenerating the integer sequences themselves
uses `collect.py` and the frozen source revision. Generated `.seq` files remain
local; their exact hashes and recipes are retained here.

The post-measurement analyzer adds presentation tables only. The measured
candidate/export code and executable remain those recorded in the manifest.
