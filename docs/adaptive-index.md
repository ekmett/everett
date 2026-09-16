Adaptive fractional-index output
=================================

Small indexes often disappear into another prepared layout before any reader
needs a durable file for them. I keep their completed encoding in memory under
a shared output allowance. A larger index continues into the same IX03 file
format as the streamed builder; it does not restart its source scan or encode
the borrowed keys again.

The runtime context defaults to an 8 MiB shared retention allowance and a
128 KiB ceiling per output. `runtime_output_options` sets these limits. Either
zero selects eager streaming, which is useful for exercising storage failures.
Native and fractional-index outputs share the same allowance. A retained owner
keeps its charge until its last reference disappears, including references from
older snapshots. The allowance's lifetime is separate from the SQLite
connection.

One encoding, two destinations
------------------------------

`cola_adaptive_index_builder` runs the ordinary three-way index algorithm. Each
borrowed route has the same 64 KiB staging buffer and modified-FC encoder used
by the streamed builder. Before the first physical flush, the builder has no
output ID, catalog reservation, dependency seals or scratch file.

The first flush resolves the exact native, main and secondary owners, reserves
an output, and creates its destination. The main route writes to the final
object stream; the secondary route uses one private unlinked spool so the file
retains its canonical section ordering. The other route can keep an unfinished
byte in its buffer. Starting the destination never flushes a sink recursively.
This first spill can perform catalog work inside an append; later appends only
write the established stream or spool.

At completion I build Elias–Fano navigation once. If both payloads are still
staged and the completed sections fit, their byte vectors and sparse arrays
become an immutable `cola_index`. Its exact native and target owners remain
shared. Otherwise those same sections finish the streamed file. The streamed
and owned forms use identical IX03 bytes, including incomplete-byte padding,
false-borrow flags, rank groups and prefix cuts.

The retained charge includes object and lease storage plus vector capacities
for payload, Elias–Fano navigation, rank classes/checkpoints, false-borrow flags
and cut prefixes. It excludes allocator bookkeeping, shared-pointer control
blocks and shared source owners.
A terminal index can have no borrowed payload and still exceed its allowance
through navigation metadata; it then becomes a file.

Bounds and failure
------------------

This is an output-retention allowance, not a bound on the whole process. The
two 64 KiB construction buffers, sparse construction metadata, source mappings,
current keys and merge workspace remain separate. Streamed secondary payload
incurs one sequential spool write and one sequential read. No partial file or
spool is an adoptable checkpoint.

An unspilled builder reports `spilled() == false`; requesting `paths()` then
throws because no physical attempt exists. A finished owned output needs no
catalog acknowledgment. A file becomes usable only after its seal and exact
pair registration are acknowledged together. Failed writes or finalization
poison the builder; an uncertain catalog acknowledgment poisons its execution
context and returns no completed pair. Previously published snapshots retain
their own immutable owners.
