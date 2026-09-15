Everett: Persistent Storage Through Composable Change
====================================================

Suppose we have a large table and a small change to make to it. We'd like to
store the change as a small object, keep the old table available to readers,
and combine the two when we have time. A snapshot then amounts to retaining
the objects we already have.

Everett is a C++20 library built around that idea: immutable sorted blobs,
compressed string keys, and explicit ownership. Fractional cascading lets us
search across the blobs; partitioned updates let us produce changes
independently; algebraic fingerprints let us compare the resulting worlds
even when we've compacted them differently. The interesting part is making
these pieces agree about what they own, what they can forget, and who pays
for the work.

Everett 0.1.0 is experimental. The header-only library provides encoded storage
components, mmap-backed query chains, incremental native merges, immutable
object sealing, an optional persistent SQLite catalog, and an in-memory reference
world. The
[implementation ledger](docs/implementation.md) records the tested contracts.
APIs and persisted formats may change during this work.

Start with the [examples](#examples) to build a blob and an index chain. The
[field guide](#field-guide) explains how the pieces fit, and the
[design document](docs/design.md) follows the accounting behind them.

Field Guide
-----------

### Blobs, worlds, and ownership

A **native blob** holds sorted key/value records. A **fractional index** holds
selected keys borrowed from a particular target. Together they form a searchable
pair. A world retains a collection of objects; a timeline describes successive
worlds; a branch point retains a place from which another timeline can grow.
I call the backing store and its relationships the **multiverse**.

| Component | What it gives you |
| --- | --- |
| `storage_policy` | One choice of byte/bit units, value layout, group size and backspace code throughout a type family. |
| `elias_fano`, `rank_groups` | Monotone offsets and grouped origin counts, independent of the key representation. |
| `profile_array`, `profile_view`, `profile_cursor` | Encoded records, borrowed views, and sequential decoding. |
| `profile_native_writer`, `native_merge_builder` | Incremental native encoding and ordered per-key value composition. |
| `profile_blob` | Native records, a separate borrowed stream, group navigation, and false-borrow flags. |
| `sample_cursor`, `index_builder`, `index_pipeline` | Sampling an existing pair and building new index links incrementally. |
| `query_root`, `query_root_builder`, `query_cursor` | Preparing a bounded search head and visiting matching native entries through an exact index chain. |
| `mapped_file`, `file`, `multiverse` | Retained read-only mappings and policy-checked object access. |
| `encode_native_sections`, `encode_index_sections`, `mapped_blob`, `mapped_query_root` | Portable blob files and queries over exact pinned mmap chains. |
| `object_writer`, `multiverse::seal_object` | Streamed immutable object writes with explicit persistence barriers and retained failure identities. |
| `sqlite_catalog` | Durable reservations, exact file graphs, immutable named saves and reader pins. |
| `reference_world`, `partition_round`, `pin_set` | Executable snapshot, update, fingerprint, and ownership semantics. |

Immutability makes sharing straightforward. Two readers can retain the same
native allocation while using different indexes. A saved world can retain old
inputs after a newer world has compacted them. Reclamation follows ownership:
an object stays alive while something still needs it.

Indexes make that last sentence precise. A borrowed key and its ordinal name
an occurrence in an **exact target pair**. If another branch merges that target,
the old ordinal still belongs to the old pair.
We keep the old target pinned, build the new link, then adopt the replacement.
This permits shared compaction work without forcing all branches to repair
their dependencies simultaneously. The [file lifecycle](docs/file-lifecycle.md)
and [catalog design](docs/catalog.md) describe that transition on disk.

### Searching through sorted streams

Binary search gets us into one sorted run. Repeating it independently in every
run costs another logarithm. Fractional cascading lets us carry the result of
one search into the next: selected target keys bound a small interval in the
following pair.

We don't have to store the merged keys to describe that interval. Grouped counts
suffice to represent the virtual merge of native and borrowed keys. At a group
boundary, rank tells us how many occurrences came from the borrowed stream;
subtraction gives the native count. The projected ranges contain at most `K`
occurrences in total. Native entries come first on equality, and borrowed
duplicates remain distinct. A borrowed key also present natively gets a
**false-borrow** flag, preserving both its lookup meaning and its position.

The group size is a policy choice of the form `K = 2^r - 1`:

| `K` | Bits per group count | Sampling positions |
| ---: | ---: | --- |
| 3 | 2 | 0, 3, 6, … |
| 7 | 3 | 0, 7, 14, … |
| 15 | 4 | 0, 15, 30, … |
| 31 | 5 | 0, 31, 62, … |

Fifteen is the default. A smaller group buys a narrower search window with more
index space; a larger group spreads the metadata over more records. Group size and
level growth are separate choices. The [sampling analysis](docs/sampling.md)
explains their interaction, including why sampling counts augmented occurrences
rather than distinct keys.

### String compression and offsets

Sorted strings share prefixes, so we can encode a key by backspacing from its
predecessor and appending a suffix. Both physical streams use ordinary front
coding. A search carries comparison state against its query: the known prefix
agreement, comparison direction and the full key length when known. It compares the next
literal without reconstructing the inherited prefix.

The borrowed predecessor before a window needs one extra scalar: its exact LCP
with the incoming boundary. Because those two keys and the query are ordered,
the smaller adjacent LCP gives its agreement with the query. This scalar belongs
to the exact index view. Rebuilding an index recomputes it while retaining the
native bytes. The [comparison argument](docs/comparison-fc.md) covers ties,
endpoints and the separate roles of physical and virtual boundaries.

The byte profile counts lengths and offsets in bytes. The bit profile works
with densely packed, most-significant-bit-first strings and counts in bits.
Both use the same policy-bound interfaces. A fixed value width counts the chosen
units: `fixed_values<3>` means three bytes under a byte policy and three bits
under a bit policy. Borrowed records carry zero value bits while retaining the
same policy family.

Bit backspaces can use `golomb<M>` or `exponential_golomb<Order>`; the default
is `exponential_golomb<0>`. The choice belongs to the policy and is checked in
stream and file metadata. Absolute retained counts and suffix/value lengths use
order-zero exponential-Golomb, while the byte profile uses unsigned varints.
Golomb's unary quotient can be long for a large backspace, so its decoding
cost includes the count's encoded length even when the resulting key is short.

We mark each physical stream's block starts and end sentinel, then encode those
monotone offsets with `elias_fano`. Its `select(i)` returns the stored integer
at ordinal `i`; sampling intervals and value strides belong to the profile.
Fixed-width values give us an additional saving: their contribution to an offset is predictable, so we subtract it before
encoding and add it back on access. If width is `w` and record ordinal is `i`,
the contribution is `w * i` in the same address units. The fixed payload stride
consequently does not inflate the residual offset universe. The terminal sample
uses the actual record count, including a short final block. Physical block
width `W` is independent of cascade stride `K`; both are part of the policy.
Each block starts with an absolute retained-prefix length, then uses relative
backspaces. We can parse controls before the selected lane without reading the
preceding block or reconstructing those earlier keys.

This is why the blob has two sparse offset structures and a grouped rank
structure, plus exact cut LCPs: two physical byte/bit streams, one virtual order. See
[key policies](docs/keys.md) for the framing and reconstruction contracts.

### Work that can stop and resume

`profile_cursor` preserves one decoder context as it advances. `sample_cursor`
keeps two such contexts, merges their current keys, and emits every `K`th
occurrence. It pins the source pair, reconstructs each source key once during
a complete pass, and borrows value payloads without copying them.

An `index_builder` accepts these samples and merges them with a native stream.
An `index_pipeline` connects builders so that newly produced samples flow
straight into the next stage:

```text
exact target pair
       |
       v
  sample_cursor --> native stage 0 --> native stage 1 --> new head
                       |                  |
                 encoded index      encoded index

completed links:       head ------> stage 0 ------> exact target
```

Handoffs are front-coded: a backspace count and suffix relative to the preceding
sample from that producer, with a literal first key. Counts use policy bytes or
bits. The receiving builder retains its decoder context and independently
records the exact cut LCPs required by its final index.

Stages are supplied nearest the target first. Bounded queues let a downstream
stage pause its producer. `step(budget)` advances the pipeline in work quanta;
`finish()` assembles the completed directories and binds the resulting pairs
to their targets. A quantum can include up to `K` source occurrences. Key
reconstruction, comparisons, and final directory construction have their own
costs, so an entry budget is not a byte or wall-clock deadline.

### Updates and agreement

A replacement table represents deletion with an absent value. To count a
deletion, though, we need to know that something was there. Updates therefore
carry both the old and new binding, and admission checks the old one. Otherwise
we could earn rebuilding credit by inventing tombstones for absent keys. The
[strong-deletion protocol](docs/rebuild.md) uses that accounting to replace
accumulated history with a smaller live table.

Now summarize the reference world's resolved contents with
`sum(h_key(key) * h_value(value))`, taking the hash of an absent value as zero.
An update subtracts the old binding's contribution and adds the new one.
Compaction leaves this sum alone, and disjoint changesets can contribute their
deltas in either order.
A file's native contents and its contribution to a world are separate quantities;
replacement deltas retain the information needed to subtract older bindings.

These fingerprints are useful for noticing disagreement. They are not
cryptographic authentication or physical object identities. The default algebra
uses wrapping unsigned 64-bit arithmetic; a supplied algebra can use a prime
field, a binary extension field, or another suitable ring. No division is needed.

Replacement values are one useful instance of a broader idea. The
[categorical update design](docs/arrows.md) treats each key's update as an arrow
between states. A list can describe prepends, appends and deletions as composable
edits, while another key can choose a different vocabulary of changes. This keeps
the storage mechanism useful beyond one interpretation of a map.

Examples
--------

Each C++ example below is a complete program. Include the component you use
and link the CMake interface target described under [building](#building).

### Choose byte or bit units

Policies make representation choices visible in types. Arrays, blobs, indexes,
and files that share a policy agree on the units used by their metadata.

```cpp
#include <everett/profile.h>

int main() {
  using bytes = everett::storage_policy<
    everett::profile_unit::byte, everett::fixed_values<8>, 15,
    everett::exponential_golomb<0>, 16>;
  using bits = everett::storage_policy<
    everett::profile_unit::bit, everett::fixed_values<3>, 7, everett::golomb<3>>;

  static_assert(bytes::bits_per_unit == 8);
  static_assert(bytes::group_size == 15 && bytes::codec_block_size == 16);
  static_assert(bits::bits_per_unit == 1);
  static_assert(*bytes::value_width == 8);
  static_assert(*bits::value_width == 3);
  static_assert(bits::backspace_parameter == 3);

  auto key = everett::bit_string::from_bits("1011011");
  return key.view().size() == 7 ? 0 : 1;
}
```

`variable_values` selects independently framed values. `bit_string` owns a
packed string; `bit_view` borrows one and can describe a range beginning inside
a byte. Keep the owner alive while using its view.

### Build and query a blob

Provide strictly increasing native keys and nondecreasing borrowed keys. This
small example fits in one group, so its first key supplies the known boundary
context for `search_window`.

```cpp
#include <everett/profile_blob.h>
#include <vector>

int main() {
  using namespace everett;
  using policy = storage_policy<profile_unit::byte, variable_values, 15>;
  auto text = [](char const * s) { return bit_string::from_bytes(s); };

  std::vector<profile_record> records{
    {text("alpha"), text("one")},
    {text("beta"), text("two")},
    {text("gamma"), text("three")}
  };
  std::vector<bit_string> borrowed{text("beta"), text("delta")};
  auto blob = profile_blob<policy>::build(records, borrowed);

  auto query = text("beta");
  auto boundary = profile_query_context<policy>(query.view()).with_key(records.front().key.view());
  auto found = blob.search_window(0, boundary);
  if (!found.native || !blob.false_borrow(0)) return 1;
  return compare_bits(found.native->value.view(), records[1].value.view()) == 0 ? 0 : 1;
}
```

`search_window` is the local navigation operation: its caller supplies a group
and the known boundary context. Its projected native and borrowed intervals
share one `K`-entry budget. For sequential access, use
`blob.native().view().cursor()`; a cursor's `peek()` exposes its reconstructed
key and an original-payload value view.

### Sample an exact encoded pair

Sampling includes both streams. Here the two equal borrowed `delta` keys remain
separate occurrences after the native `delta`.

```cpp
#include <everett/sampling.h>
#include <memory>
#include <vector>

int main() {
  using namespace everett;
  using policy = storage_policy<profile_unit::byte, variable_values, 3>;
  using blob = profile_blob<policy>;
  auto text = [](char const * s) { return bit_string::from_bytes(s); };
  std::vector<profile_record> records{
    {text("alpha"), text("a")}, {text("delta"), text("d")},
    {text("omega"), text("o")}
  };
  std::vector<bit_string> borrowed{text("delta"), text("delta"), text("theta")};
  auto target = std::make_shared<blob const>(blob::build(records, borrowed));
  sample_cursor<policy> samples(target);
  target.reset();

  std::uint64_t count = 0;
  while (!samples.done()) {
    auto sample = samples.peek();
    if (sample.target_ordinal != count * policy::group_size) return 1;
    auto retained_key = bit_string::copy(sample.key);
    (void)retained_key;
    ++count;
    samples.advance();
  }
  return count == 2 && samples.counters().decoded_entries == 6 ? 0 : 1;
}
```

Repeated peeks leave the cursor where it is. A sampled key view lasts until
advance, move, or destruction; copy the key when retaining it beyond that point.
The sampler's `target()` retains the exact pair independently of the caller's
original handle.

### Build a chain while retaining native allocations

The pipeline starts at an existing target and works outward through the supplied
native stages. The final stage becomes the new head. This example also destroys
the pipeline and its original input handles before checking the result's retained
target.

```cpp
#include <everett/index_pipeline.h>
#include <initializer_list>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

int main() {
  using namespace everett;
  using policy = storage_policy<profile_unit::byte, variable_values, 3>;
  using blob = profile_blob<policy>;
  using pair = std::shared_ptr<blob const>;
  auto make = [](std::initializer_list<std::pair<std::string_view, std::string_view>> rows) {
    std::vector<profile_record> records;
    for (auto const & [key, value] : rows)
      records.push_back({bit_string::from_bytes(key), bit_string::from_bytes(value)});
    return std::make_shared<blob const>(blob::build(records));
  };

  pair head;
  std::weak_ptr<blob const> retained_target;
  std::byte const * native_bytes = nullptr;
  {
    auto target = make({{"maple", "m"}, {"oak", "o"}, {"pine", "p"}, {"willow", "w"}});
    auto near = make({{"elm", "e"}, {"spruce", "s"}});
    auto far = make({{"ash", "a"}, {"birch", "b"}});
    retained_target = target;
    native_bytes = far->native().bytes().data();
    index_pipeline<policy> pipeline(target, std::vector<pair>{near, far});
    while (!pipeline.done()) pipeline.step(64);
    head = pipeline.finish();
  }
  return !retained_target.expired() && head->native().bytes().data() == native_bytes
    && head->borrowed().size() != 0 ? 0 : 1;
}
```

The borrowed indexes are new; the native allocation at the head is the same one
supplied for the final stage. Intermediate sampled catalogs are passed through
the pipeline as bounded queues. Finalized pairs own their exact downstream
relationships, allowing older and newer chains to coexist.

### Query the whole chain

A query starts from a prepared root. If the head already fits in one group,
preparation retains it as-is. Otherwise, I add empty-native routing catalogs
above it until the new head fits. Those catalogs contain successively sparser
samples and retain the existing chain. Preparation scans the original head;
we do that once and reuse the root for subsequent queries.

```cpp
#include <everett/query.h>
#include <initializer_list>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

int main() {
  using namespace everett;
  using policy = storage_policy<profile_unit::byte, variable_values, 3>;
  using blob = profile_blob<policy>;
  using pair = std::shared_ptr<blob const>;
  auto make = [](std::initializer_list<std::pair<std::string_view, std::string_view>> rows) {
    std::vector<profile_record> records;
    for (auto const & [key, value] : rows)
      records.push_back({bit_string::from_bytes(key), bit_string::from_bytes(value)});
    return std::make_shared<blob const>(blob::build(records));
  };
  auto base = make({{"alpha", "a"}, {"beta", "lower"}, {"gamma", "g"}, {"omega", "o"}});
  auto upper = make({{"beta", "upper"}, {"delta", "d"}, {"theta", "t"}});
  index_pipeline<policy> indexes(base, std::vector<pair>{upper});
  while (!indexes.done()) indexes.step(64);

  query_root_builder<policy> prepare(indexes.finish());
  while (!prepare.done()) prepare.step(64);
  auto root = prepare.finish();
  auto key = bit_string::from_bytes("beta");
  auto query = root.cursor(key.view());
  std::vector<bit_string> values;
  while (!query.done()) {
    query.step(1);
    if (query.has_match()) values.push_back(query.take_match().value);
  }
  return values.size() == 2 && values[0] == bit_string::from_bytes("upper") &&
    values[1] == bit_string::from_bytes("lower") ? 0 : 1;
}
```

`query_root<policy>::build(head)` is the eager preparation convenience. A cursor
owns its query key and pins the unvisited chain, so it can outlive the original
handles. `step(budget)` visits at most that many catalogs and stops when a match
is ready. `take_match()` returns an owned value, native ordinal and exact source
pair; a pending match pauses further traversal until it is taken.

Matches arrive from head toward target. That order describes the index chain;
the caller supplies any replacement or arrow-composition semantics. A native
match does not suppress routing to later matches. Query steps bound catalog
visits, not reconstructed bytes or value-copy cost. The
[query contract](docs/query.md) describes preparation, trust and work bounds.

### Fork a world and apply disjoint updates

Use `reference_world` to exercise the update semantics with byte-string keys and
unsigned 64-bit values. Each batch is prepared against the same base. Partition
ownership determines which keys it may change; different workers may read that
base while preparing their assigned writes.

```cpp
#include <everett/world.h>
#include <sstream>

int main() {
  using namespace everett;
  auto base = reference_world<>::from_records({{"alpha", 10}, {"beta", 20}});
  auto saved = base.snapshot();
  auto partition = [](std::string_view key) -> std::uint64_t {
    return key == "alpha" ? 0 : 1;
  };
  partition_round first(base, "round-1", partition);
  auto a = first.make_batch("a", 0, {{"alpha", 12}});
  auto b = first.make_batch("b", 1, {{"beta", 25}});
  first.apply(a);
  first.apply(b);

  partition_round second(base, "round-1", partition);
  second.apply(b);
  second.apply(a);
  auto next = first.snapshot();
  if (next.signature() != second.snapshot().signature()) return 1;
  if (next.resolved() != second.snapshot().resolved()) return 1;
  if (saved.get("alpha") != 10 || next.get("alpha") != 12) return 1;

  auto compacted = next.compact();
  if (compacted.signature() != next.signature()) return 1;
  std::stringstream export_stream;
  compacted.save(export_stream);
  auto restored = reference_world<>::restore(export_stream);
  return restored.resolved() == next.resolved() ? 0 : 1;
}
```

Admission checks old values, partition ownership, overlap, and the advertised
fingerprint contribution. Identical batch replay is recognized. Calls to a
receiver's `apply()` are serialized; preparing disjoint batches can happen
independently. The reference `save`/`restore` pair exports a resolved table,
while `snapshot()` shares the existing immutable state.

Incremental Native Merges
-------------------------

`profile_native_writer<P>` accepts records one at a time. We choose a common
value width before writing, or let each value carry its own length. Finishing
produces an ordinary front-coded array with its sparse offset directory.
`profile_blob<P>::adopt_native` turns it into a terminal pair without decoding
or rewriting the native data.

The merge builder consumes two sorted native streams, older then newer. Its
default equal-key operation takes the newer value:

```cpp
#include <everett/native_merge.h>
#include <everett/query.h>
#include <memory>
#include <string_view>

int main() {
  using P = everett::storage_policy<everett::profile_unit::byte>;
  using array = everett::profile_array<P>;
  auto make = [](std::string_view text) {
    everett::profile_native_writer<P> writer;
    auto key = everett::bit_string::from_bytes("alpha");
    auto value = everett::bit_string::from_bytes(text);
    writer.append(key.view(), value.view());
    return std::make_shared<array const>(writer.finish());
  };
  everett::native_merge_builder<P> merge(make("before"), make("after"));
  while (!merge.done()) merge.step(16);
  auto pair = std::make_shared<everett::profile_blob<P> const>(
    everett::profile_blob<P>::adopt_native(merge.finish()));
  auto root = everett::query_root<P>::build(pair);
  auto key = everett::bit_string::from_bytes("alpha");
  auto query = root.cursor(key.view());
  query.step(1);
  if (!query.has_match()) return 1;
  return query.take_match().value == everett::bit_string::from_bytes("after") ? 0 : 2;
}
```

A custom `compose(key, older, newer)` can combine encoded arrows instead. Its
result may own its bits or borrow a view until the writer consumes it. With
associative composition, we can change the merge parentheses while keeping
each key's update order. One step unit resolves one distinct key; string work,
composition and final EF construction have separate costs. The builder retains
its source owners, and a failed step cannot publish a partial result. See
[native writing and merging](docs/native-merges.md) for mapped inputs,
value-width rules and the continuation's exact limits.

Mapped Objects
--------------

The custom object kinds are `.kv` and `.index`. Physical identities use 32
lowercase hexadecimal digits, split into paths such as
`ab/cd/0123456789abcdef0123456789abcd.kv`. The two directory prefixes are removed
from the leaf name. Logical keys never become filesystem paths.

`mapped_file` provides read-only shared ownership of an opened regular file.
Bounded slices retain the mapping after the original owner is released.
`file<P>::open(path)` checks the 96-byte header and exact file extent, including
magic, version, policy and header CRC32C, without reading the body. An explicit
`file<P>::scan()` checks the whole body's CRC32C and bit padding when recovery
or a scrub calls for it. Opening an object does not certify its payload.
`multiverse<P>` opens these objects beneath an existing backing directory and
exposes their associated policy-bound types.

`encode_native_sections` and `encode_index_sections` package the existing encoded
arrays into portable file sections. They borrow the arrays while writing: we do
not front-code the keys again or rebuild Elias–Fano. A prepared query chain can
then be reopened with `multiverse<P>::open_query(saved_head_identity)`. Its
`mapped_query_root` retains the exact native/index mappings and uses the same
bounded cursor operations as the in-memory root. Typed opening reads fixed
metadata; `mapped_blob::scan()` explicitly verifies contents, navigation and
exact downstream samples. See [mapped blobs](docs/mapped-blobs.md) for the
layout, lifetime and trust contracts.

`multiverse<P>::seal_object` writes a body under caller-reserved object and
attempt identities. It accepts a contiguous span or borrowed chunks, including
mmap-backed input. The writer computes CRC32C while streaming, seals a private
file, installs its name without replacing an existing object, and flushes the
directory chain. A failed operation retains surviving outputs and reports its
identities and last acknowledged stage. The [sealing protocol](docs/object-writer.md)
spells out Linux/macOS barriers and the caller's recovery obligations. Sealing
an object produces the receipt that `sqlite_catalog::record_sealed` records
before the pair is registered and saved.

For already trusted objects, pass `file_open_mode::trusted` to `open`,
`from_slice`, or `multiverse<P>::open_object`. This avoids reading even the
header page. `body()` returns the physical bytes after the 96-byte envelope;
requesting `header()` explicitly reads and validates the metadata, returning it
by value. `scan()` still performs full validation. Trusted opening assumes the
caller already knows the object's type, policy and format; checked opening is
the default.

The optional `sqlite_catalog<P>` reserves objects before writing, records their
seal receipts, registers exact query chains and retains immutable named saves
and reader pins. We can close it, reopen a save and query its mmap chain. Enable
`EVERETT_ENABLE_SQLITE` and link `everett::sqlite`; the ordinary core target has
no SQLite dependency. The [working catalog guide](docs/sqlite-catalog.md) gives a
complete publication/reopen example and explains operation replay and failed
commits. The [catalog design](docs/catalog.md) extends this to mutable timelines,
retirement and merge progress. This leaves two immutable object kinds for us to
manage. The [durability protocol](docs/durability.md)
orders verified output, durable publication, and old-pin release, with explicit
recovery states after failed synchronization.

Performance
-----------

The compact representation also gives us useful units of parallel work. Packed
rank classes can be summed together, byte prefixes can be compared sixteen
bytes at a time, and bit streams can be copied or framed in word-sized chunks.
The implementations keep loads within the supplied spans, including the last
partial byte. Compiler targets select the available instructions; the package
does not add runtime dispatch or impose ISA flags on consumers.

I measure dependent queries as well as independent throughput. The next step
through a fractional index depends on the previous answer, and the fastest
bulk kernel need not have the lowest latency for that chain. In particular,
groups of three keep a scalar packed sum, while seven and thirty-one use NEON
on little-endian AArch64. The dense select scan also stays scalar: its SIMD
candidate was slower in the measured cases.

The [blob and pipeline comparison](bench/blob_pipeline.md) measures complete
in-memory builds, three-link index construction and searches within a known
window. It checks the resulting bytes and answers against the same inputs and
an independent catalog oracle. The component reports cover
[key and bit operations](bench/key_bits.md),
[Elias–Fano](bench/select_compare.md),
[grouped and bitmap rank](bench/other_rank.md), and
[rank15](bench/rank_compare.md), with source, raw trials and reproduction
commands. The [whole-query comparison](bench/query_compare.md) includes root
preparation and traversal through every catalog, with owned results. Ordinary FC
with exact cut comparisons uses 38–48% less median query time in its M2 Max
fixtures, with counted backing arrays changing by less than 1%. Root preparation
has mixed results. These are resident-memory measurements; disk faults remain
a separate cost.

A [count-decoding follow-up](bench/small_count.md) measures the common bit
counts from one bounded field. It reduces complete bit-query time by another
26.0–26.9% against its ordinary-FC baseline, without changing the encoded arrays.
The byte-profile timing ranges overlap. Each report gives its own exact
baseline, fixtures and validation; these are separate measurements.

The incremental writers keep the previous key buffer and replace only its
changed suffix after an append succeeds. Complete append/finalize measurements
show [26–46% less time for borrowed keys](bench/borrowed_prefix.md) and
[18–44% less for native key/value records](bench/native_prefix.md). These runs
verify identical encoded sections and decoded contents. The retained buffer
capacity can grow to the largest key seen; output bytes and format do not change.

Proofs
------

The [Lean model](proof/README.md) checks the algebra, ownership and fractional
indexing rules behind the construction. Its theorems cover chronological composition,
disjoint updates, endpoint contributions, snapshot adoption and retention of
exact target dependencies. An adjacent-merge theorem connects the composition
law to adoption of a new representation.

For fractional indexing, a search over sampled occurrences followed by a local
window search agrees with a full predecessor search. Endpoint ranks project
that window into native and borrowed ranges sharing one `K`-entry budget.
False-borrow recovery finds an equal native key even when it lies before the
window. Equal borrowed occurrences remain distinct, and indexes retain their
exact targets across catalog extension and safe reclamation.

These are abstract sequence proofs. Compressed rank, Elias–Fano, front coding,
complete cascade execution, scheduling and the filesystem protocol still need
their own refinements. The proof project builds independently of C++ and records
those boundaries explicitly.

The string-comparison modules also check the ordered-triple LCP identity and
associative mismatch transfers used by the
[ordinary-FC design](docs/comparison-fc.md). An exact LCP at each cut can repair
the preceding borrowed comparison state without repeating its prefix. Connecting
those theorems to encoded metadata and the complete cursor is the next proof
obligation. The complete query tests separately check the encoded implementation
against independent native-array oracles.

Building
--------

Use CMake 3.20 or later and a C++20 compiler. The mapping backend uses the
platform's native read-only mapping API. I use one pinned
[fast-crc32 generator](https://github.com/corsix/fast-crc32) for portable, ARM
and x86 CRC32C kernels. Generated code ships with the headers, so a consumer
build needs no download, generator, or additional linked library. The compiler
target selects eligible kernels; buffers too small to benefit from parallel
folding use a scalar path. The checksum and file format stay the same.

```sh
cmake -S . -B build -DEVERETT_BUILD_TESTS=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

The tests check decoded results against independent oracles, group boundaries,
byte/bit policy combinations, exact ownership, partition permutations, and
simulated publication and recovery failures. Run AddressSanitizer and
UndefinedBehaviorSanitizer on supported compilers with:

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DEVERETT_BUILD_TESTS=ON -DEVERETT_SANITIZERS=ON
cmake --build build-sanitize --parallel 4
ctest --test-dir build-sanitize --output-on-failure
```

Install the package:

```sh
cmake --install build --prefix /path/to/everett-install
```

Configure your consumer with that prefix in `CMAKE_PREFIX_PATH`, then link the
interface target:

```cmake
find_package(everett CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE everett::everett)
```

An embedded checkout supports `add_subdirectory(path/to/everett)` and the same
target. Tests default off when embedded. `EVERETT_USE_CCACHE=ON` enables a
compiler cache for test builds when `ccache` is available.

With Doxygen and Python 3 installed, generate and check the API documentation:

```sh
cmake -S . -B build-docs -DEVERETT_BUILD_DOCS=ON
cmake --build build-docs --target everett_docs
ctest --test-dir build-docs -R '^everett.doxygen$' --output-on-failure
```

The optional documentation check verifies file metadata, declaration ownership,
overloads, and source locations. See [Doxygen conventions](docs/doxygen.md) and
[contributor instructions](AGENTS.md) for the corresponding source conventions.

Further Reading
---------------

The functional
[`Data.Vector.Map`](https://hackage.haskell.org/package/structures-0.2/docs/Data-Vector-Map.html)
and its deamortized variant in `structures` supply the starting point. Everett's
levels use the redundant COLA scheme from
[Cache-Oblivious Streaming B-trees](https://people.cs.georgetown.edu/~jfineman/papers/sbtree.pdf).
String locality and front-compression background come from
[Cache-Oblivious String B-trees](https://people.csail.mit.edu/bradley/papers/BenderFaKu06.pdf),
particularly Section 3.2. The sparse offset representation follows the
Elias–Fano techniques discussed in
[Quasi-Succinct Indices](https://vigna.di.unimi.it/ftp/papers/QuasiSuccinctIndices.pdf).

The [design's annotated references](docs/design.md#11-references-and-their-roles)
include fractional cascading, succinct rank, persistent streaming indexes,
compressed string merging, and live-size rebuilding. For a particular concern:

- [Sampling and index construction](docs/sampling.md) gives the exact occurrence and work accounting.
- [Network admission](docs/network-admission.md) explains reuse of received native bytes.
- [Sorts and keys](docs/keys.md) describes units, framing, and prefix-free sort codes.
- [Strong deletion](docs/rebuild.md) connects tombstones to rebuilding credit.
- [Categorical updates](docs/arrows.md) develops composition beyond replacement tables.

License
-------

Copyright 2026 Edward Kmett. My code is available under
[BSD-2-Clause](LICENSES/BSD-2-Clause.txt) **OR**
[Apache-2.0](LICENSES/Apache-2.0.txt), at your option. Both complete license texts
are included in source and installed packages; see [LICENSE](./LICENSE).
The CRC generator and its output retain Peter Cawley's **MIT OR zlib** terms;
the [third-party notices](THIRD_PARTY.md) and complete upstream licenses ship
with the package.

Contact Information
-------------------

Contributions, examples, and bug reports are welcome. Please use
[GitHub issues](https://github.com/ekmett/everett/issues) for reproducible problems
or design discussion, or contact me at [ekmett@gmail.com](mailto:ekmett@gmail.com).

-Edward Kmett
