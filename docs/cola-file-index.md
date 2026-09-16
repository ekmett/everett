Streaming fractional indexes
============================

`cola_file_index_builder` uses the same three-way merge and sampling algorithm
as `cola_index_builder`. I choose a different concrete output sink: borrowed
keys go into an IX03 file, while ranks, false-borrow flags, cut prefixes and
Elias–Fano offsets remain sparse in-memory metadata.

The output bytes are identical to `encode_cola_sections` for the same native,
main and secondary identities. Existing readers and recovery checks need no
format revision. Exact inputs stay pinned throughout construction; finishing a
file does not change any previously published index or its dependencies.

Two payloads, one file
---------------------

IX03 stores the main and secondary borrowed streams in separate contiguous
sections. Construction discovers their keys together. I write the main stream
to the object's private output and put the secondary stream in a private spool.
After the main stream and its offsets are complete, I copy the spool into the
secondary section and append the remaining navigation metadata.

The spool has an exclusive private name only until its descriptor opens; the
writer unlinks it immediately. It is never an adoptable Everett object, has no
seal receipt and gets no durability barrier. Destruction closes it after
success, abandonment or failure. If unlinking itself fails, construction fails
and the private non-object name remains for cleanup. Restart reconstructs the
partial index from its retained input recipe.

If the secondary FC payload has $S$ bytes, this strategy adds $S$ bytes of
sequential scratch writes and $S$ bytes of sequential reads. It avoids a second
walk through all native and target keys and preserves the existing physical
format. With no secondary samples, no spool file is opened.

Memory and failure behavior
---------------------------

Each borrowed stream has a 64 KiB bit buffer. The final spool copy uses a
64 KiB transfer buffer. Source cursors retain their current keys; the metadata
arrays grow with the number of groups and borrowed entries. There is no array
of all reconstructed borrowed keys and no FC payload-sized memory allocation.
Long Golomb unary controls are emitted through bounded spans.

A terminal index uses its native count to build zero navigation without opening
a native payload cursor. For a nonterminal index, `step(n)` consumes at most
$n$ augmented occurrences, with up to the sampling factor's worth of source
advancement for a sampled head. Key bytes, metadata finalization, spool copying
and file barriers are additional work; this is not a wall-clock bound.

Once output or spool I/O fails, the builder is poisoned. Repeating `step` or
`finish` cannot retry the uncertain attempt. Only `finish` returns the final
object's seal receipt after its normal file and directory barriers. The caller
must still record that receipt and publish the exact dependency graph.

The tests compare complete files for byte and bit policies, K=3 and K=15,
partial-bit keys, mixed integer/string sorts and false borrows. They exercise
short writes and reads, spool creation/unlink/write/read failures, final-file
write/header/flush failures, metadata-only terminal construction, and an
allocation cap while producing a multi-megabyte borrowed payload.
