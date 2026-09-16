Small native merge outputs
==========================

I keep small native merge results in memory until they belong to a published
frontier. A batch can create an intermediate result, merge it again, and discard
it without reserving an object or writing a file. Publication seals the graph it
actually retains. Larger results use the streamed KV03 writer.

The merge runs the same sort-owned encoder in both cases. Its existing 64 KiB
bit buffer holds the encoded prefix. The first physical flush reserves an
identity, creates a private file, and writes that prefix verbatim. Encoding then
continues from the same state. In particular, spilling never calls composition a
second time or reconstructs an already encoded record.

The retained allowance
---------------------

`runtime_output_options` defaults to an 8 MiB allowance per execution context and
a 128 KiB ceiling per retained output. Native and fractional-index construction
share that allowance. The charge includes the owning object, its allowance
lease, and the actual allocation capacities of its payload, selector dictionary,
selector seeds, dictionary offsets, and all Elias–Fano sections.

The charge does not measure allocator/control-block overhead, source owners,
cursor workspace, user composition temporaries, or the existing streamed
encoder's construction workspace. It is an allowance for retained outputs, not
a bound on the process's memory use.

The two source counts and selector dictionaries provide a metadata-only
preflight. An output cannot contain more records or selector codes than its
inputs, although composition can enlarge a value arbitrarily. If the navigation
and dictionary bounds do not fit, construction starts with a file immediately.
This bound is conservative when many input keys coincide. At completion I check
the actual payload and metadata capacities again. If the complete object fits
and the shared allowance has room, the result becomes an ordinary owning native
facade. Otherwise it is sealed as a file.

The lease belongs to the output's shared owner. Dropping a merge worker or an
execution context does not invalidate a retained result. The last owner returns
its charge, even when that owner is released on another thread. A retained array
does not keep a catalog connection alive.

For explicit storage construction, either zero option selects the eager file
path:

```cpp
auto storage = storage_type::open(root, {}, {}, {}, {},
  everett::runtime_output_options{0, 0});
```

`sort_runtime_context::retained_output_bytes()` reports the current charge and
`output_limit()` reports the context's allowance. Existing explicit file writers
continue to return a seal receipt.

Publication and failure
-----------------------

A completed owning result has no seal receipt and no durable-object authority.
The graph sealer treats it like any other owning native array when a checkpoint
needs it. It reserves and seals the object before publishing the root and its
pins. Completed private results that never enter that graph need no file.

Once a writer spills, it follows the same reserve-before-create, file barriers,
and acknowledged catalog-seal path as an eager writer. A reservation, write,
barrier, or seal failure poisons that execution context. It cannot retry the
attempt by silently returning an owning result instead. Previously published
snapshots remain valid.

The scheduler's record and merge-work accounting is unchanged. This changes
which intermediate physical outputs survive construction; it does not remove
the work needed to produce them or the metadata needed for a durable checkpoint.
