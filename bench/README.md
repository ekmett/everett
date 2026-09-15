Benchmark evidence
==================

The reports measure the exact Git revisions named in each experiment. I keep
recorded source snapshots, commands, metadata names and raw results unchanged
so those measurements remain reproducible. Some snapshots use the historical
`everett` namespace and include paths; these are recorded inputs, not the
current Diet API.

Most runners extract their exact headers from Git. For a runner or standalone
fixture that uses the current checkout, reproduce the report from its recorded
revision in a separate worktree; use the report's candidate and harness
arguments. Moving or renaming the repository does not change those Git objects.

New measurements of Diet should record their own source hashes and results.
The [implementation ledger](../docs/implementation.md) links the accepted
measurements and describes the current library.
