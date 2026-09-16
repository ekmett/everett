Mapped blobs and exact query roots
=================================

This guide describes the supported single-route IX02 layout and its
`mapped_blob` APIs. The [COLA guide](cola-indexes.md) describes IX03, whose two
borrowed streams route to a main pair and a terminal secondary native file.
Both layouts share the same KV02 native objects.

An encoded blob can be written as a native `.kv` object and an independent
`.index` object, then reopened for querying without copying its payload or
navigation arrays. The index records its native identity and its exact
downstream `(native, index)` identities. A mapped pair retains all three owners.
Reindexing can therefore share the native file while preserving old chains.

The outer [object envelope](file-lifecycle.md#object-envelope) still carries the
policy, kind, record count and CRC32C. The body now has a small section directory
followed by the encoded arrays. Opening a typed owner reads the envelope and
directory; it does not reconstruct keys or read rank/Elias–Fano contents.

Writing a pair
--------------

Given an immutable `profile_blob<P>` named `pair`, its reserved native/index
identities, and an optional exact target identity:

```cpp
auto native = everett::encode_native_sections(pair.native());
auto index = everett::encode_index_sections(pair, native_id, exact_target);
auto sealed_native = native.seal(object_directory, native_id, native_attempt);
auto sealed_index = index.seal(object_directory, index_id, index_attempt);
```

Both encoders borrow their source arrays. Keep those arrays immutable and alive
until writing finishes. On a little-endian host, the encoder owns its small
directory and borrows the existing data and word arrays. A big-endian host needs
conversion buffers for directory words. Neither path rebuilds front coding or
Elias–Fano. `chunks()` exposes the output as borrowed spans; these also borrow
the encoder and expire if it moves or dies. `materialize()` explicitly copies a
complete envelope and body into a vector, useful for small fixtures or transports
requiring contiguous storage.

The source must use sorted FC records with valid retained prefixes. Conservative
prefixes may repeat literal material. Encoding does not rescan every key to
certify those properties. An index encoder records the supplied identities; those
declarations do not prove the sampled keys agree with a target. The explicit
semantic scan below checks both properties.

Sealing uses the [immutable writer](object-writer.md). It requires durable root
creation and reserved object/attempt identities from its caller. A pair becomes
eligible for catalog publication only after both files and their dependencies
are ready. These encoders do not publish a saved world or reserve identities.

Preparing and reopening a chain
------------------------------

Prepare the in-memory query root before serializing its pairs. This includes
any empty-native routing prefix produced by `query_root<P>::build`. Persist the
identity of the resulting head through the catalog that owns the save.

```cpp
everett::multiverse<P> storage(object_directory);
auto root = storage.open_query(saved_head_identity);
auto cursor = root.cursor(query_key);
while (!cursor.done()) {
  cursor.step(1);
  if (cursor.has_match()) {
    auto match = cursor.take_match();
    // match.value is owned; match.source pins the exact mapped source pair.
  }
}
```

`open_mapped_query<P>(directory, head_identity)` is the equivalent standalone
function. It follows fixed identity metadata, shares native mappings reused
within the chain, rejects repeated index identities and missing dependencies,
then adopts the already-bounded head. It performs no sampling pass or implicit
index construction. A head larger than `K` is rejected.

For explicit ownership, `mapped_native<P>::open(file)` and
`mapped_index<P>::open(file)` retain individual mappings.
`mapped_blob<P>::bind(identity, native, index, target)` checks the declared
native/target identities and counts and returns an immutable shared pair.
`mapped_query_root<P>::adopt_prepared(head)` checks the chain shape and head
bound. The caller/catalog vouches for each supplied owner's physical identity;
the current opaque 128-bit identity is not a verified content digest.

Cursor and returned-match ownership work as they do for in-memory pairs.
Dropping an opener or unlinking a file does not invalidate retained POSIX
mappings. Mapped owners reject access after being moved from. Bare profile,
word and section views borrow their owner's lifetime; keep an owning pair or
mapped object alongside them.

File layout
-----------

All container integers are explicitly little-endian. Section positions and
lengths count **physical bytes**; the meaningful FC extent, key lengths and
residual offsets retain the policy's byte or bit units. For a bit policy, the
outer envelope extent is the complete container byte size multiplied by eight.
The FC section separately records its meaningful bit length and padding.

| Slot | Native object | Index object |
| ---: | --- | --- |
| 0 | Native FC bytes | Borrowed FC bytes |
| 1 | EF low words | EF low words |
| 2 | EF high words | EF high words |
| 3 | EF sample pairs | EF sample pairs |
| 4 | EF sparse positions | EF sparse positions |
| 5 | — | Packed origin classes |
| 6 | — | Origin-rank checkpoints |
| 7 | — | False-borrow bits |
| 8 | — | Exact cut-LCP words |

The native directory is 128 bytes; the index directory is 256 bytes. Both start
with four-byte magic (`KV02` or `IX02`), a 16-bit version of 2 and a 16-bit section
count. Offsets 8, 16 and 24 contain the FC extent, terminal key length and EF
universe as 64-bit words; byte 32 contains the low-bit width.

Each FC block begins with an absolute retained-prefix count; later records use
relative backspaces. Typed opening rejects version-1 section directories. The
outer envelope has its own version, which remains 1: it describes opaque body
bytes independently of the inner codec.

In an index directory, byte 33 indicates a target, offset 40 contains the
virtual occurrence count, and offsets 48, 64 and 80 contain the 16-byte native,
target-native and target-index identities. An absent target has zero bytes in
both target identity fields. Other reserved bytes are zero.

Native descriptors begin at byte 48, index descriptors at byte 112. Each is a
pair of 64-bit offset and length fields. Sections appear in slot order, starting
at the directory end and aligning each next start to eight bytes. Alignment
gaps are zero, and no trailing bytes follow the last section. Select samples
are explicit pairs of 64-bit words; serialized layout does not depend on C++
struct padding. Accessors use bounded byte loads, so an unaligned enclosing
slice needs neither copied arrays nor constructed `uint64_t` objects.

Fixed-width values are still subtracted from the **inner FC offset universe**.
Directory bytes and other sections do not inflate that universe. There remain
two physical EF directories, one grouped rank structure, false-borrow flags and
one direct-addressed cut-LCP array. There is no full offset per record.

Opening, access and scanning
---------------------------

I keep three different checks explicit:

1. **Typed opening** checks the envelope, fixed directory, canonical ranges,
   overflow and navigation-array shapes. It reads no FC or navigation elements.
2. **Query access** checks each addressed range, count and projection before
   use. It can reject malformed data it encounters, but does not certify all
   unvisited metadata.
3. **Scanning** checks the contents and their relationships. It can touch every
   key and directory entry and is requested explicitly.

The existing `file_open_mode::trusted` operation still creates an envelope
handle without reading mapped bytes. Converting that handle to a typed mapped
profile is a separate, explicit metadata read; it validates the envelope then.

`mapped_native::scan()` verifies CRC, padding, sequential framing, FC
order/uniqueness, physical checkpoints, terminal length and the canonical EF
arrays rebuilt from record positions. The scanner validates a changed prefix
by examining its first new unit. When a conservative prefix repeats that unit,
it compares the remaining explicit tails to establish order. It then updates
its key buffer in place; it does not copy the inherited prefix for every record.

`mapped_index::scan()` adds canonical rank classes/checkpoints and flag padding.
`mapped_blob::scan()` scans the complete pinned chain, recomputes each actual
interleaving, false-borrow flag and cut LCP, and compares each borrowed key with
every `K`th occurrence of its exact target's augmented stream. Unique native
keys and native-before-borrowed ties remain part of that contract.

A CRC-valid index with the right number of keys can still name the wrong
samples. Metadata-only opening and prepared adoption do not rule that out.
Use a trusted construction/admission path or an explicit full scan before
relying on untrusted index semantics.

Semantic scans retain reconstructed key buffers and compact directory scratch,
roughly $O(N/W+A/K)$ words for a pair with $N$ native and $A$ augmented entries,
in addition to key storage. String comparisons can exceed encoded-byte work.
Only the envelope CRC pass has a simple linear-in-physical-bytes bound.
Successful scanning describes current readable bytes; it cannot rehabilitate
an earlier failed synchronization attempt by itself.
