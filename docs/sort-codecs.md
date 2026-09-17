Sort-owned bit records
======================

I give the sort control over its record grammar. An integer key has a known
width; making it pass through a string length and an FC suffix would spend
bits and work without helping us. A string sort can instead retain a prefix
of its previous key. Either sort can carry a fixed value, a variable value,
an explicit tombstone, a sentinel niche, or no value payload.

`everett/sort_codec.h` implements that streaming layer. Its native and borrowed
streams share a registry and key grammar. This is a separate record format
from `profile_array`: the existing mapped profiles and their indexes continue
to use their own framing. These codecs do not by themselves change a `.kv`
file's format or make mixed record grammars queryable through those profiles.
The [sort contract](keys.md) describes their shared logical identity and order.

A small example
---------------

```cpp
#include <type_traits>
#include <optional>

import everett;

struct names {
  using key_codec = everett::fc_string_key<>;
  using value_codec = everett::tombstone_value<everett::string_value<>>;
  using encoding = everett::bit_encoding<>;
};
struct counters {
  using key_codec = everett::unsigned_key<32>;
  using value_codec = everett::niche_value<everett::unsigned_value<16>, 65535u>;
  using encoding = everett::bit_encoding<everett::fixed_values<16>>;
};
using registry = everett::bin<everett::tip<names>, everett::tip<counters>>;

everett::sort_record_writer<registry> write;
write.append<names>("alpha", "one");
write.append<names>("alpine", std::nullopt);
write.append<counters>(42, 7);

everett::sort_record_reader<registry> read(write.data().view());
read.next([](auto, auto const &, auto const &, auto) {
  // Receives std::type_identity<S>, S's key and value, and record controls.
});
```

The callback is instantiated for every occupied sort. It can use `if constexpr`
on the type carried by its tag to choose the appropriate operation. References passed
to it last only for that call. Input records must be strictly ordered by sort
code and then by the sort's key comparator. Both writer and reader check that
order, including duplicate keys within a sort.

Two levels of prefix state
--------------------------

I keep the tree's prefix state separate from the leaf's key state:

1. Read a backspace count for the previous **sort code**.
2. Keep that many fewer bits of the code, then read discriminator bits until
   the registry reaches a leaf.
3. Hand the input to that leaf's key codec, followed by its value codec.

There is no discriminator length or “descend this far” count. Prefix freedom
supplies the boundary. On the first record the old code is empty and its
backspace is zero. On consecutive records of the same sort, retaining the
complete code needs only the zero backspace count. The reader reuses the typed
leaf directly; it does not traverse a deep tree again for each record.

For example, changing codes from `0000` to `0001` backs out one bit and writes
one bit. Changing from `0001` to `001` backs out two and writes one. A leaf's
FC key then handles its own key backspace and literal. Key-prefix state is
retained only while that same sort remains selected. A switch begins the new
key grammar without a predecessor of another type.

`sort_record_writer<Registry, TreeCode, Role>` defaults to
`exponential_golomb<0>` for tree backspaces and `stream_role::native`. The key
codec can choose another count code independently. `golomb<M>` and
`exponential_golomb<Order>` use the existing bounded count readers and writers.
The complete registry must require bit addressing. Bytes inside a leaf can
start at any bit position; no padding is inserted between records.

Leaf codecs
-----------

`sort_codec<S>` obtains `S::key_codec` and `S::value_codec`, or can be specialized
for an existing sort. The header supplies specializations for
`unsorted<std::string>` and `unsorted<std::optional<std::string>>`. Those
specializations supply grammar, without changing the registry's unit inference.

| Key codec | Logical key | Physical key grammar |
| --- | --- | --- |
| `fc_string_key<CountCode>` | `std::string` | Backspace bits, suffix bit count, literal bits |
| `fc_bit_key<CountCode>` | `bit_string` | Backspace bits, suffix bit count, literal bits |
| `raw_string_key<CountCode>` | `std::string` | Byte count followed by literal bytes |
| `unsigned_key<Bits>` | `uint64_t` | Exactly `Bits` bits, most significant first |

FC string backspaces count **bits**, including agreement inside a byte. The
resulting complete key must contain a whole number of bytes. An `a` followed
by a `b`, for example, retains their first six bits and writes two new bits.
Packed-bit keys have no whole-byte restriction. String comparison uses unsigned
bytes, with shorter proper prefixes first and embedded zero bytes preserved.

| Value codec | Logical value | Physical value grammar |
| --- | --- | --- |
| `string_value<CountCode>` | `std::string` | Byte count followed by literal bytes |
| `bit_value<CountCode>` | `bit_string` | Bit count followed by literal bits |
| `unsigned_value<Bits>` | `uint64_t` | Exactly `Bits` bits |
| `tombstone_value<C>` | `optional<C::value_type>` | Presence bit, then `C` when present |
| `niche_value<C, Sentinel>` | `optional<C::value_type>` | `C`, reserving the sentinel for absence |
| `no_value` | `std::monostate` | No bits |

A sentinel is unavailable as a present value. An explicit tombstone usually
makes a fixed-width value variable-width: the absent case is just one bit,
while the present case also has its payload. `fixed_value_bits` reports the
codec's actual common width, when one exists. The sort's `encoding` must agree
with the grammar it declares. The stream checks a fixed-width promise against
the codec's width trait; layout inference does not inspect user-defined codec
bodies. An empty optional string and a present empty string stay distinct.

Each value codec supplies `skip(reader)`, so a caller can advance over a
payload without constructing it. A custom key codec supplies `value_type`,
`less(a,b)`, `write(writer,key,previous)` and `read(reader,previous)`. The previous
key pointer is null when entering a different sort. A custom value codec supplies
`value_type`, `write`, `read`, `skip` and its optional `fixed_value_bits`.
`sort_bit_reader` and `sort_bit_writer` provide bounded fixed-width fields,
borrowed bit ranges, and policy-selected counts. There is no virtual dispatch.

Borrowed streams and comparison
-------------------------------

Use `stream_role::borrowed` to write only keys with the same registry:

```cpp
everett::sort_record_writer<registry, everett::exponential_golomb<0>,
  everett::stream_role::borrowed> index;
index.append<names>("alpha");
index.append<counters>(42);
```

The corresponding reader receives `std::monostate` instead of a value. This
omits values entirely, including optional tags and length controls. Native
integer keys and borrowed integer keys both retain their direct fixed-width
representation.

The full-record reader reconstructs keys because streaming merges need the
current key. Comparisons can use a cheaper operation: `fc_string_key::read_frame`
and `fc_bit_key::read_frame` return an `fc_key_frame` with an absolute retained
bit position and a borrowed literal. They need the previous key's **length**,
not its contents. Given an exact comparison of the predecessor against a
query, `frame.compare(query, previous_comparison)` transfers that comparison
without reconstructing the inherited prefix. `bit_comparison` records the
number of agreeing bits and the ordering result. Supplying the correct
predecessor comparison remains the caller's obligation.

Canonical keys and hashes
-------------------------

`sort_code<Registry,S>()` returns a sort's complete discriminator;
`write_sort_code<Registry,S>(writer)` writes it into an existing bit stream.
Key codecs also expose `write_ordered` and `read_ordered` for a standalone,
prefix-free, order-preserving key representation. These are useful when an
adapter must transport complete keys through an existing opaque-key API.

For strings I leave nonzero bytes unchanged, escape zero as `00 ff`, and use
`00 00` as the terminator. This stays byte-sized even when a bit-tree code
precedes it. Integers retain their fixed-width bits. Packed-bit keys use `1b`
for each input bit `b`, followed by a zero terminator. Those standalone codes
are not the FC record bytes and do not replace the compact native grammar.

The codec does not hash records. The selected sort hashes its logical key and
value; discriminator bits are routing information and do not enter the state
fingerprint. Different sorts can have the same key bytes without becoming the
same logical key.

Anchors, validation and integration
----------------------------------

`state()` exposes a copyable `sort_record_state<Registry>` containing the last
sort path and its typed key. Pass a copied state to another writer to continue
the stream, or to a reader starting at the following record's bit position.
Its path is checked against its typed leaf. This is an in-memory anchor, not
a portable persisted checkpoint, a full-key restart inserted every $K$ entries,
or a substitute for the fractional index's boundary context.

Readers reject undefined sorts, truncated fields, impossible backspaces,
misaligned reconstructed strings, bad ordered-string escapes and out-of-order
keys. A failed writer or reader is poisoned. Callback failure also poisons the
reader after consuming that record; the established state and position can be
copied into a new reader over the remaining slice. Input bit extent is exact:
an enclosing file still needs its record count, framing version and integrity
metadata to distinguish a shorter valid stream from a missing final record.

The focused test exercises mixed sorts, deep code changes, partial-byte FC
prefixes, every packed-bit tail length, borrowed streams, direct integer
fields, both tombstone forms, restart anchors, comparison transfer, and malformed
input. The [sort-owned profile](sort-profiles.md) supplies sampled offsets and
mapped native framing for these leaf grammars. `sort_runtime_family` connects
them to the redundant scheduler and [typed semantics](typed-world.md), while the
[storage context](sort-runtime-context.md) supplies durable merge output.
The mixed-sort catalog tests combine string replacements, integer additions
and chronological string arrows through repeated close/reopen cycles.
