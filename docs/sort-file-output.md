Streaming sort-owned native files
================================

`sort_profile_file_writer` and `sort_profile_file_merge` write KV03 native files
without keeping a complete output payload in memory. They use the same joint
sort/key framing state as `sort_profile_writer` and seal through `object_stream`.
The resulting bytes match the ordinary in-memory encoder, including the header,
checksum, sparse offsets, shared selector dictionary and block seeds.

Merge into a file
-----------------

```cpp

import everett;

using policy = everett::string_policy;
using native = everett::mapped_sort_profile<policy>;

// older and newer are shared_ptr<native const>. The caller has reserved the
// object and attempt identities and established the backing directory.
everett::sort_profile_file_merge<policy, native> merge(
  root, object_id, attempt_id, older, newer);
while (!merge.done()) merge.step(64);
auto receipt = merge.finish();
auto mapped = native::open(receipt.path);
```

Each step counts output keys. Key bytes, value bytes, callback work and I/O are
additional costs. The merger pins its inputs across pauses and failures. Only
`finish` installs and seals the completed immutable file; a paused or abandoned
attempt is not a resumable durable checkpoint.

The replacement path forwards the newer encoded value on a collision. It keeps
key prefixes as spans into mapped inputs rather than constructing another full
key. A custom composition operation may return a borrowed value or an owning
encoded value. An operation that requests the full logical key still pays for
its reconstruction on collisions.

Write encoded records
---------------------

For an already sorted stream of complete logical keys and sort-encoded values:

```cpp
everett::sort_profile_file_writer<policy> output(root, object_id, attempt_id);
for (auto const & record : records)
  output.append_encoded(record.key.view(), record.value.view());
auto receipt = output.finish();
```

The logical key is the selector code followed by the leaf's finite order bits,
with its length supplied separately. The value is exactly the sort value
codec's encoding. Neither receives an extra outer length. `append_encoded`
checks strict key order and retains one preceding full key for comparison.
Input spans need only remain alive during the call.

The merge uses `append_frame(frame, key_spans, common_bits, value)` instead.
That lower-level entry point accepts a trusted sorted prefix frontier, so it
can consume borrowed spans without a full-key buffer. A caller supplies the
matching sort descriptor and an exact output LCP; it cannot mix subsequent
`append_encoded` calls into that trusted frame stream.

Memory and framing
------------------

I use one 64 KiB payload buffer. Byte-aligned large spans can pass directly to
`object_stream`; unaligned spans move through the same fixed buffer. Golomb
unary runs are emitted in bounded chunks. A very large backspace or suffix
count therefore cannot allocate a correspondingly large control buffer.

The writer retains sparse block offsets, block seed IDs and the occupied-sort
dictionary in memory. It builds Elias–Fano and packs the seed IDs at finish.
This bounds payload buffering, not all metadata by a fixed constant. The
caller still owns the input values, and composition callbacks choose their own
allocation behavior.

Common value width is discovered from the encoded values, just as in the
in-memory writer. If a later value has a different width, the file becomes
variable-width. Sparse positions stay absolute until finalization, where the
actual common width, if any, is subtracted before Elias–Fano construction. No
payload rewrite or advance guess of a common width is needed.

`sort_profile_detail::encoder` contains the shared framing state. Its concrete
sink supplies `position`, `append`, `write_bits` and `write_count<Code>`.
Built-in navigable key codecs accept such an output in their `header` operation.
A custom codec used with file output must likewise emit its header through this
sink interface; a header accepting only `sort_bit_writer&` supports memory
output alone.

The incremental merge accepts a defaulted `Output` template parameter and an
injected output object. `sort_profile_file_merge` supplies a small concrete
reference adapter for its nonmovable file writer. There is no virtual backend
or second merge algorithm.

Failure and integration
-----------------------

An exception during trusted frame emission or finalization poisons the writer.
An I/O failure cannot be cleared by retrying `finish`; the uncertain private or
installed names are retained for recovery. Sealing uses the existing object
header, checksum combination, immutable installation and file/directory
barriers. A receipt establishes that sealing protocol, not catalog adoption or
a power-loss simulation.

The active sort runtime still creates owned merge outputs. This file merger is
available to a caller-owned worker, but connecting it to that runtime needs an
execution context that reserves object identities and provides file writers.
The storage family and injected merge output are the seams for that work.

Tests compare complete files with the in-memory encoding under K=3 and K=15,
independent codec block widths, mixed raw integers and FC strings, partial-bit
keys, fixed and changing value widths, and a 512-bit selector stored once per
file. Fault injection checks payload writes, metadata writes, synchronization
and installation. Allocation guards merge an 8 MiB value and multi-million-bit
unary controls while rejecting any individual allocation above 128 KiB. The
remaining mapped/typed runtime suites exercise the shared framing refactor.
