# Incremental native writing and merging

I can build a native file without first collecting every full key and value in
a record vector. `profile_native_writer<P>` accepts strictly increasing keys,
emits ordinary FC and retains the previous key, encoded output and one residual
offset per physical block. Values and keys passed to `append` only need to live
through that call.

```cpp
#include <everett/native_writer.h>
#include <everett/query.h>

using P = everett::storage_policy<everett::profile_unit::byte>;

everett::query_root<P> make_table() {
  everett::profile_native_writer<P> writer;
  auto key = everett::bit_string::from_bytes("alpha");
  auto value = everett::bit_string::from_bytes("first");
  writer.append(key.view(), value.view());
  auto pair = std::make_shared<everett::profile_blob<P> const>(
    everett::profile_blob<P>::adopt_native(writer.finish()));
  return everett::query_root<P>::build(pair);
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
everett::profile_native_writer<P> writer(8); // Eight bytes for this byte policy.
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

## Ordered two-way merges

`native_merge_builder<P, Native, Compose>` pins two native source owners and
feeds their sorted union directly to the native writer. `Native` defaults to
`profile_array<P>`; a `mapped_native<P>` supplies the same view interface.
The first input is older, the second newer.

```cpp
everett::native_merge_builder<P> merge(older_array, newer_array);
while (!merge.done()) merge.step(128);
auto array = merge.finish();
```

The default composition uses the newer value when both streams contain the
same key. It borrows that value until the output writer copies it, avoiding an
intermediate payload allocation. For another per-key category, supply
`compose(key, older_value, newer_value)`, returning either a borrowed `bit_view`
or an owned `bit_string`. The key lets that operation choose its interpretation
per record. Keys occurring in only one input retain their values unchanged.

For associative composition, we can merge `(A,B)` then `C`, or `A` then `(B,C)`.
We still preserve the chronological order `A,B,C`. Associativity permits
reparenthesizing; it does not permit swapping two updates to the same key.
The tests use string concatenation as an associative, noncommutative instance.

The merger treats values as encoded data. It does not interpret a tombstone,
drop an identity arrow, validate arrow endpoints or calculate a world's
fingerprint. In particular, retaining a deletion marker is the default: removing
it needs the older-coverage proof described in [rebuilding](rebuild.md).

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
saved root nor releases catalog pins. Those transitions belong to the
[catalog](sqlite-catalog.md) and [publication protocol](durability.md).
