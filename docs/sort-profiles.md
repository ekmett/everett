Sort-owned native records
=========================

`sort_profile_array<P>` is the bit-first native stream for sorts that own their
key and value grammars. `mapped_sort_profile<P>` reads the same records directly
from a `.kv` mapping. I keep fractional-index keys in their own FC stream, so
changing an index never rewrites the native grammar.

```cpp
struct numbers {
  using encoding = diet::bit_encoding<diet::fixed_values<64>>;
  using key_codec = diet::unsigned_key<64>;
  using value_codec = diet::unsigned_value<64>;
};
using registry = diet::bin<diet::tip<numbers>, diet::sort_undefined>;
using policy = diet::storage_policy<registry>;

diet::sort_profile_writer<policy> writer;
writer.append<numbers>(17, 100);
writer.append<numbers>(23, 200);
auto native = writer.finish();
auto sections = diet::encoded_sort_sections<policy>::from(native);
// sections.seal(root, object_id, attempt_id) uses the normal durable writer.
```

Bit registries use this stream through the ordinary `active_engine` and named
[connection](connection.md). `sort_runtime_family<P>` connects its key/value
grammars to typed reads, chronological composition and the redundant scheduler.
The [execution-owned storage context](sort-runtime-context.md) retains small
outputs or streams larger ones through the durable writer. Saved snapshots,
forks and reopened connections keep their exact mapped native/index graph.
Byte registries currently use the opaque `profile_array` transport.

One continuation count
-----------------------

An FC string record uses one backspace count for both the sort prefix and the
key prefix. A backspace that remains inside the selected sort's key region keeps
that handler. A backspace into the sort code resumes selection, then enters the
new sort's key grammar. There is no second tree-backspace bit before every FC
key and no sort-suffix length field.

For raw integer keys, the continuation cursor stays at the sort leaf after the
integer is read. The next integer in the same sort therefore has a zero
continuation count followed by its fixed-width bits. It does not acquire an FC
suffix length or a large backspace count. With exponential-Golomb order zero,
two adjacent 64-bit integer records with 64-bit values differ by 129 bits: one
zero-continuation bit, 64 key bits and 64 value bits.

The value codec provides the complete value framing. A string value writes its
own length once. `value_codec::skip` finds the next record without allocating or
decoding that value. Fixed-width values occupy exactly their codec's width.
Actual common encoded value width is recorded per file and subtracted from the
Elias–Fano offset universe as usual.

Selection is a protocol
------------------------

`registry_selector<Registry>` adapts the declarative registry to two operations:

- `select(input, visitor)` consumes one prefix-free selector code and calls
  `visitor(std::type_identity<S>{}, input)` with the input positioned at the
  leaf grammar.
- `write<S>(output)` emits the matching code.

`sort_profile_writer<P, Selector>`, its array/view types and the mapped types
accept another selector implementation. The registry still declares the sorts
and their encoding requirements. A selector may use a generated switch or a
lookup table; record navigation does not depend on its tree shape. Code order
must agree with the registry's logical sort order, and old files must be opened
with an interpretation that preserves the codes they actually contain.

The parser caches the selected leaf handler across same-sort records. It calls
the selector on sort transitions and when entering a physical block without a
handler; a sequential walk keeps that handler across block boundaries too. A 512-bit selector test checks that it is not selected again for every
key within a block.

Restarting without a full key
------------------------------

Every physical block starts with an absolute retained position. It does not
start with a backspace that requires the previous key's length. A separate
packed seed selects that block's initial sort from the file's dictionary of
occupied prefix-free selector codes. The dictionary stores each occupied code
once. With $m$ occupied sorts and physical block width $W$, the seed vector costs

$$
\left\lceil\frac{n}{W}\right\rceil
\left\lceil\log_2 m\right\rceil
$$

bits; one occupied sort uses no seed bits. At $W=15$, two sorts cost roughly
$1/15$ of a bit per record, excluding the dictionary itself. A single 512-bit
code takes 512 dictionary bits for the whole file rather than another 512 bits
at every block.

These seeds identify the grammar and the retained position. They do not contain
full keys. `encoded_at` can find and skip a record while inherited key bytes are
unknown. A query carries its comparison with the incoming boundary: the order,
common-prefix length and any known full length. It compares only new literal
spans when that comparison needs refinement. Sort transitions expose the sort
suffix and key suffix as two borrowed spans, without joining them into a key.

The supplied writer starts a complete file with a literal key. Its sequential
cursor reconstructs one current key for sampling or explicit scans. This does
not make an arbitrary interior block independently reconstructible: a consumer
that needs the full key still needs its inherited prefix. Comparison-only
navigation does not need it. This implementation does not yet emit files whose
first key inherits a prefix from an external continuation.

Logical order and codec contracts
----------------------------------

A key's comparison sequence is its selector code followed by its local order
bits, with an explicit logical length. String order bits are the string's bytes;
integer order bits are its fixed-width unsigned representation. A proper-prefix
string sorts before its extension because their logical lengths differ. Native
strings do not carry the zero escapes used by the opaque typed adapter.

`sort_profile_key<Codec>` connects a leaf codec to navigation. The supplied
specializations cover FC byte strings, FC bit strings, raw byte strings and
fixed-width unsigned integers. A custom specialization supplies:

- `front_coded`, whether the continuation cursor includes local key bits.
- `order(value)`, an order-preserving finite bit sequence.
- `read(input, retained_bits)`, an `fc_key_frame` borrowing literal bits.
- `header(output, retained_bits, suffix_bits)`, framing before literal bits.

The frame must recover its logical length and locate its suffix without reading
inherited prefix bytes. Its finite-bit order must agree with the sort's key
order. Arbitrary key codecs that cannot provide these operations do not
implicitly gain the FC comparison bound. Their adapter needs an explicit
fallback and cost model.

Fractional-index integration
----------------------------

`cola_index` obtains its native view, borrowed view and borrowed writer from the
native owner's `stream_family`. The default family is unchanged. The sort family
pairs its native grammar with ordinary FC of sampled logical order bits.
Projection ranks, false-borrow flags, cut LCPs and the two-route COLA search all
use the existing algorithms. Native values stay in their original files.

`mapped_sort_cola<P>::bind` pins a KV03 native object, its IX03 index, its exact
main pair and its optional terminal secondary native object. The ordinary
`cola_query_root<P, mapped_sort_cola<P>>` searches that graph. Build its query
once with `sort_profile_query<P,S>(key)`. Matches own their encoded values and
retain their source node; decoding a match uses the sort's value codec.

`encoded_sort_sections` writes the KV03 section revision. It has the ordinary
Diet file envelope, native payload, Elias–Fano components, selector dictionary,
dictionary offsets and packed block seeds. Existing KV02 readers reject it.
Fractional-index bytes retain IX03 because their physical grammar is unchanged.
Opening checks the envelope and fixed directory but touches no payload, EF,
dictionary or seed pages. `scan()` explicitly validates the checksum and walks
the records. The tests protect every payload page during opening.

Merging without repeated prefixes
---------------------------------

`sort_profile_merge_builder<P, Native, Compose>` owns both immutable sources.
Each source keeps its current key as a stack of borrowed literal spans. Advancing
truncates that stack at the retained position and appends new literals. The
merge carries the LCP of each input frontier with its last output key, just as
the opaque merge does. Output copies only the required suffix into its new
native record.

Replacement merging reconstructs no full keys. A key-aware composition callback
gets a materialized logical key only when equal keys actually meet; the
`materialized_keys()` counter makes this observable in tests. A callback that
only needs the two value encodings receives those directly. The callback's key
and value views expire when the callback/append step ends; retaining them
requires copying or retaining the appropriate source ownership.

A step budget counts distinct output keys and consumed records. Literal bytes,
value composition and final offset construction remain additional work. This
is a concrete resumable merge component, not a wall-clock latency guarantee.
