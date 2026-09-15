Benchmark evidence
==================

I recorded the existing measurements before renaming this project from Everett
to Diet. Their source snapshots, `everett` namespaces and include paths,
commands, metadata names and raw results remain unchanged so the reports stay
reproducible. They measure the revisions named in each report.

Most runners extract their exact headers from Git. For a runner or standalone
fixture that uses the current checkout, reproduce the report from its recorded
revision in a separate worktree; use the report's candidate and harness
arguments. Moving or renaming the repository does not change those Git objects.

New measurements of Diet should record their own source hashes and results.
The [implementation ledger](../docs/implementation.md) links the accepted
measurements and describes the current library.
