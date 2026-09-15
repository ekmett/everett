# Incremental native writing and merging

I can build a native file without first collecting every full key and value in
a record vector. `profile_native_writer<P>` accepts strictly increasing keys,
emits ordinary FC and retains the previous key, encoded output and one residual
offset per physical block. Values and keys passed to `append` only need to live
through that call.

After an append succeeds, the writer reuses the unchanged prefix in its private
key buffer and copies the changed suffix. It retains capacity for the largest
key seen until finalization or destruction. The
[construction measurements](../bench/native_prefix.md) cover both fixed and
variable value widths and verify exact output against batch encoding.

```cpp
#include <diet/native_writer.h>
#include <diet/query.h>

using P = diet::storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<>>>>;

diet::query_root<P> make_table() {
  diet::profile_native_writer<P> writer;
  auto key = diet::bit_string::from_bytes("alpha");
  auto value = diet::bit_string::from_bytes("first");
  writer.append(key.view(), value.view());
  auto pair = std::make_shared<diet::profile_blob<P> const>(
    diet::profile_blob<P>::adopt_native(writer.finish()));
  return diet::query_root<P>::build(pair);
}
```

`adopt_native` moves the encoded allocation into a native-only pair and builds
its all-zero rank and cut-LCP directories. It does not decode the native keys,
rebuild their Elias–Fano directory or change their FC bytes. This operation
trusts the array's ordinary-FC and unique-key contract; the native writer
establishes both. General profile arrays can also represent repeated keys or
explicit restart variants, so arbitrary arrays need their content precondition
established before adoption.

## Value widths and completion

A fixed-value policy supplies the common width. For a variable-value policy,
the writer normally emits a length for every value. If I already know all values
have one width, I supply it at construction:

```cpp
diet::profile_native_writer<P> writer(8); // Eight bytes for this byte policy.
```

Every append must agree with that width. The sampled residual universe removes
the known value stride, just as it does in batch encoding. I choose the width
before writing; the streaming encoder cannot discover a different layout at the
end without rewriting already emitted records. Zero is a valid common width.
For bit policies, the width counts bits.

A rejected append leaves previously accepted records intact. `finish()` builds
Elias–Fano from the staged offsets, transfers the encoded bytes into a
`profile_array<P>` and closes the writer. A failed final allocation leaves it
available for another attempt. Moving a writer transfers its state and closes
the source.

The output is still an in-memory encoded array. Neither append nor finish is a
byte-budgeted disk continuation. [Portable sections](mapped-blobs.md) package
the finished array for the immutable writer without recoding it.

## Writing directly to a file

`native_file_writer<P>` accepts the same sorted records while streaming the FC
payload into an `object_stream<P>`. Construction takes the object directory,
reserved native identity, attempt identity and optional common value width.
`append` consumes its key and value before returning. The writer keeps its
previous key, 64 KiB of payload buffering, bounded control scratch and one
residual offset per physical block. It does not retain the complete payload.

`finish` builds Elias–Fano, writes its portable sections and fills the reserved
native directory and envelope. CRC combination accounts for the backpatch
without reading the body again. It then seals the file with the
[immutable-object barriers](object-writer.md). Bytes match
`encode_native_sections(profile_native_writer.finish())` for the same records
and common-width choice. Bit profiles retain partial bytes until complete and
stream long Golomb unary counts in bounded chunks.

`native_file_merge<P, Native, Compose>` connects the same merge algorithm to
that file sink. Its arguments are the directory, reserved native and attempt
identities, older/newer source owners, composition policy and optional common
width. `step`, `done` and `progress` have the in-memory builder's contracts;
`finish` returns an `object_seal_receipt`. Both file wrappers are nonmovable.
The [streamed timeline example](streamed-timeline.md) writes, merges, publishes
and reopens actual files while retaining an earlier save.

Invalid record input is rejected before output is changed. An allocation
failure while constructing final EF metadata can be retried. Once final bytes
are being emitted, an I/O or finalization failure poisons the attempt and
preserves surviving names. This differs from the in-memory writer's ability to
discard an unfinished allocation. Pausing a live merger retains its state;
restoring an interrupted process still requires the
[durable continuation](merge-resumption.md).

## Ordered two-way merges

`native_merge_builder<P, Native, Compose>` pins two native source owners and
feeds their sorted union directly to the native writer. `Native` defaults to
`profile_array<P>`; a `mapped_native<P>` supplies the same view interface.
The first input is older, the second newer.

```cpp
diet::native_merge_builder<P> merge(older_array, newer_array);
while (!merge.done()) merge.step(128);
auto array = merge.finish();
```

The default composition uses the newer value when both streams contain the
same key. It borrows that value until the output writer copies it, avoiding an
intermediate payload allocation. For another category, supply
`compose(older_value, newer_value)`, returning either a borrowed `bit_view` or
an owned `bit_string`. If interpretation depends on the key, supply
`compose(key, older_value, newer_value)` instead. A callable supporting both
forms uses the key-aware form. Keys occurring in only one input retain their
values unchanged.

The key argument borrows cursor scratch and expires when that cursor advances.
Value arguments borrow the immutable input bytes. I copy the returned view into
the output before advancing either cursor. A policy that keeps a value view in
its own state must retain the corresponding source owner too: builder
reassignment can release the previous inputs before transferring the policy.
Retaining a key requires a copy because its cursor buffer is reused.

For associative composition, we can merge `(A,B)` then `C`, or `A` then `(B,C)`.
We still preserve the chronological order `A,B,C`. Associativity permits
reparenthesizing; it does not permit swapping two updates to the same key.
The tests use string concatenation and fixed-width affine maps as associative,
noncommutative instances. Mapped-input tests seal and reopen the result, pause
and move the merger, and keep using input mappings after their names are unlinked.

The merger treats values as encoded data. It does not interpret a tombstone,
drop an identity arrow, validate arrow endpoints or calculate a cola's
fingerprint. In particular, retaining a deletion marker is the default: removing
it needs the older-coverage proof described in [rebuilding](rebuild.md).

## Carrying comparisons forward

Let $p$ be the last emitted key and $a,b$ the current input heads. Both heads
follow $p$. I keep $\ell_a=\mathrm{lcp}(p,a)$ and
$\ell_b=\mathrm{lcp}(p,b)$ in the policy's units.

| Known prefixes | Next key | Additional comparison |
| --- | --- | --- |
| $\ell_a>\ell_b$ | $a$ | None |
| $\ell_a<\ell_b$ | $b$ | None |
| $\ell_a=\ell_b$ | Determined from the suffixes | Start at that common boundary |

After emitting $a$, the comparison supplies the remaining head's
$\mathrm{lcp}(a,b)$. The advancing cursor supplies $\mathrm{lcp}(a,a')$.
The replacement and value-only paths represent each current key as spans of
immutable input literals. Advancing trims that span sequence at the retained
position and appends the next literal. Before trimming, it compares the old key
with the new suffix to obtain the exact LCP and reject non-increasing inputs.
The boundary can precede the previous record's literal, so retaining only that
one literal would lose the context needed for validation. Ordinary FC finds the
difference in the first new unit; redundant FC may repeat more material.

Each record adds at most one span; backward traversal removes crossed spans
permanently. A long chain of one-unit extensions can still retain one descriptor
per key unit. This saves literal copies, but does not guarantee smaller scratch
space than a contiguous key buffer. Key-aware callbacks use a reconstructed key
through `profile_cursor::advance_comparison`.

Each descriptor stores a source-relative bit offset and cumulative logical
endpoint in 16 bytes; the source backing is shared by the cursor. The
[space measurements](../bench/native_compact.md) quantify the savings and the
remaining cost of deep fragment chains. This representation also gives us
address-independent ingredients for a future checkpoint, though the current
cursor does not serialize or restore them.

The shared frame writer receives the winning head's known prefix and literal
suffix. It checks units, value width and prefix bounds, then writes the frame.
It retains only the previous key's length. If the input record retains $r_i$
units and its output retains $r_o$, sorted order gives $r_o\ge r_i$: the previous
output lies between this input's predecessor and its current key. We can emit
the input literal after dropping $r_o-r_i$ units. A physical output block starts
with absolute $r_o$; other records use a backspace from the last output length.
The merger keeps no third key buffer. `profile_native_writer::append` uses the same framing component and
retains its own predecessor to check arbitrary caller keys.

Byte policies carry whole-byte LCP counts; bit policies carry exact bit counts.
Fractional-index cut scalars remain exact bit LCPs, so a byte merge count alone
cannot replace those scalars. EOF is a separate cursor state, never a sentinel
key. Equal heads consume both inputs and preserve the callback's older/newer
argument order.

The [complete merge measurements](../bench/native_merge.md) cover short and
long-prefix keys in both profiles, fixed and variable values, exact encoded
output, and chronological composition. In the measured M2 Max fixtures, the
integrated merger uses 12.50–80.31% less time than the original implementation.
The isolated frame-output change saves 2.14–11.03%; these are separate paired
comparisons, and their percentages should not be added.

The [literal-forwarding measurements](../bench/native_encoded.md) isolate the
encoded-source path and its first-unit comparison. They cover changing literal
tails and deep prefix fragments in addition to shared prefixes, with requested
allocation bytes reported separately from runtime. Fragment descriptors can
use more memory than a reconstructed key even when literal copying is avoided.

## Work and ownership

One `step` unit resolves one distinct key and consumes at most two input records.
It reports both counts, and a zero budget makes no progress. Construction loads
the first record from each nonempty input; each later cursor advance may decode
the next key. String decoding, composition, allocation and final Elias–Fano
construction are additional work. This is an incremental key budget, not a
worst-case byte or time guarantee.

Both sources remain owned while the builder exists, including after completion
or a step failure. A decoding, encoding or composition error during `step`
poisons the builder: it cannot resume or publish a partial result. A failed
composition-policy move assignment also leaves the destination poisoned.
Calling `finish` before both inputs end is a recoverable usage error; failure
while building the final EF directory can be retried without consuming inputs.

Input arrays must remain immutable through all aliases and contain unique
sorted keys. Use validated mapped inputs when reading untrusted files. The
builder's C++ state is not a durable merge checkpoint, and it neither moves a
saved root nor releases catalog pins. The [restart contract](merge-resumption.md)
lists the input contexts, unfinished output and composition state that a durable
continuation must retain. Those transitions belong to the
[catalog](sqlite-catalog.md) and [publication protocol](durability.md).
