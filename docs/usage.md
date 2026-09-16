Detailed Usage
==============

The [README](../README.md#quick-start) starts with the recommended named session API.
The [connection guide](connection.md) covers mutable commands, asynchronous
tickets, snapshots and forks. This guide goes underneath that API to file
construction, explicit updates and merges, then explains representation and
tuning choices. We keep the same policy from input records to mapped queries.

For an ordinary byte-aligned string table, use `everett::multiverse<>`, whose
policy is `everett::storage_policy<>`. The [byte table guide](byte-transport.md)
covers its raw string framing, schema and measured tradeoffs.

For a bit table, select the policy explicitly:

```cpp
#include <everett/connection.h>

using bit_store = everett::multiverse<everett::string_policy>;
auto storage = bit_store::create("bit-data");
auto db = storage.connect("main");
db.put("name", "Everett");
```

This uses the bit registry and order-zero exponential-Golomb backspaces, with
the same connection, snapshot and transaction operations. Choose its matching
policy when reopening the store; a policy choice does not convert existing files.
I use bytes as the starting point and measure the whole workload before choosing
bit packing for space. Sorts with bit-oriented keys can declare that grammar
directly.

For low-level encoded blobs, use a sort exposing `using encoding = byte_encoding<>` for byte strings,
or `bit_encoding<>` for packed bits, then use `storage_policy<tip<YourSort>>`.
The registry derives the storage units from its sorts. Both encodings default
to variable values, 15:1 sampling and physical blocks of 15 records; bit
backspaces use order-zero exponential-Golomb.

- [Persistent tables and saves](#persistent-tables-and-saves) walks through writing and reopening a table.
- [Examples](#examples) covers policy variations, individual blobs, complete chains and partitioned updates.
- [Incremental native merges](#incremental-native-merges) combines ordered updates without changing older snapshots.
- [Mapped objects](#mapped-objects) covers opening, validation, sealing and publication.
- [Field guide](#field-guide) explains indexing, compression, ownership and work accounting.
- [Performance](#performance), [proofs](#proofs) and [building](#building-and-testing) collect measurement and verification details.

Persistent Tables and Saves
---------------------------

The persistent workflow has three parts: build immutable table files, publish a
root in the catalog, and open that root through a multiverse. Naming another save
of a registered root only retains it; it does not serialize the table again.
The catalog keeps its small mutable records in SQLite, while queries read the
key/value files through mmap.

### Create a table and reopen it

Link this program with `everett::sqlite`. Run `example create /path/to/store` once,
then `example read /path/to/store` in another process. The backing directory
must already exist; the application establishes its durability before creating
the catalog. The create mode expects a fresh store. Its fixed identities are
for this example only: allocate fresh object, attempt and operation identities
for new work in an application.

```cpp
#include <everett/multiverse.h>
#include <everett/sqlite_catalog.h>
#include <array>
#include <string_view>

using strings = everett::encoded_sort<everett::byte_encoding<>>;
using P = everett::storage_policy<everett::tip<strings>>;
using store_type = everett::multiverse<P>;
using catalog = everett::sqlite_catalog<P>;

int main(int argc, char ** argv) {
  if (argc != 3) return 64;
  std::string_view mode = argv[1];
  if (mode != "create" && mode != "read") return 64;
  store_type store(argv[2]);
  auto text = [](char const * s) { return everett::bit_string::from_bytes(s); };

  if (mode == "create") {
    auto metadata = catalog::create(store.root(),
      everett::object_id("00000000000000000000000000000001"));
    everett::blob_identity ids{
      everett::object_id("00000000000000000000000000000002"),
      everett::object_id("00000000000000000000000000000003")};
    everett::object_attempt_id attempt("00000000000000000000000000000004");
    std::array outputs{
      everett::catalog_object_reservation{ids.native, everett::file_kind::native_blob},
      everett::catalog_object_reservation{ids.index, everett::file_kind::fractional_index}};
    metadata.reserve("reserve-table", attempt, "table-builder", {}, outputs);

    std::array records{
      everett::profile_record{text("alpha"), text("one")},
      everett::profile_record{text("beta"), text("two")}};
    auto table = store_type::blob::build(records);
    auto native = everett::encode_native_sections(table.native());
    auto index = everett::encode_index_sections(table, ids.native);
    metadata.record_sealed("seal-native",
      native.seal(store.root(), ids.native, attempt));
    metadata.record_sealed("seal-index",
      index.seal(store.root(), ids.index, attempt));
    auto query = store.open_query(ids);
    metadata.register_chain("register-table", query);
    metadata.save("save-initial", "initial", ids);
    return 0;
  }

  auto metadata = catalog::open(store.root());
  auto pin = metadata.acquire_save("read-initial", "initial", "example-reader");
  auto saved = store.open_query(pin.head);
  auto key = text("alpha");
  auto query = saved.cursor(key.view());
  while (!query.done()) {
    query.step(1);
    if (query.has_match()) return query.take_match().value == text("one") ? 0 : 1;
  }
  return 2;
}
```

The reader acquires its pin before opening the files. Registration uses the
normal metadata checks, trusting the contents produced by our encoders; pass
`catalog_admission::scan` when admission should verify the complete contents.
The two-entry table fits the default fifteen-entry search head. A larger table
needs a prepared chain, including its routing prefix, before publication.

The [catalog guide](sqlite-catalog.md#a-complete-publication-path) gives the
reservation, sealing and registration calls. For more than fifteen entries,
prepare a root with `query_root<P>::build` and persist every pair in its chain;
see the [mapped example](mapped-blobs.md). For incremental output, follow the
[streamed timeline example](streamed-timeline.md). The
[COLA store example](cola-store.md) adds two-route indexes and publishes a merge
while the old saved states remain readable.

Examples
--------

Each C++ example below is a complete program. Include the component you use
and link the CMake interface target described under [building](#building-and-testing).

### Choose byte or bit units

Policies make representation choices visible in types. Arrays, blobs, indexes,
and files that share a policy agree on the units used by their metadata.

```cpp
#include <everett/profile.h>

int main() {
  using strings = everett::encoded_sort<everett::byte_encoding<everett::fixed_values<8>>>;
  using packed = everett::encoded_sort<everett::bit_encoding<everett::fixed_values<3>>>;
  using bytes = everett::storage_policy<everett::tip<strings>, 15,
    everett::exponential_golomb<0>, 16>;
  using bits = everett::storage_policy<everett::tip<packed>, 7, everett::golomb<3>>;

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

`variable_values` selects independently framed values in a leaf encoding.
For several sorts, use `bin<L,R>` or `sort_list<S...>` and reserve holes with
`sort_undefined`; [sort registries](keys.md#extending-a-registry) explains
dispatch and extension. Existing files retain their own value widths when a
new sort broadens the registry. `bit_string` owns a
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
  using policy = storage_policy<everett::tip<everett::encoded_sort<everett::byte_encoding<>>>, 15>;
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
  using policy = storage_policy<everett::tip<everett::encoded_sort<everett::byte_encoding<>>>, 3>;
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
  using policy = storage_policy<everett::tip<everett::encoded_sort<everett::byte_encoding<>>>, 3>;
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
  using policy = storage_policy<everett::tip<everett::encoded_sort<everett::byte_encoding<>>>, 3>;
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
[query contract](query.md) describes preparation, trust and work bounds.

### Fork a world and apply disjoint updates

Use `reference_world` to exercise the update semantics with byte-string keys and
unsigned 64-bit values. Each batch is prepared against the same base. Partition
ownership determines which keys it may change; different workers may read that
base while preparing their assigned writes.

```cpp
#include <everett/world.h>

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
  return compacted.resolved() == next.resolved() ? 0 : 1;
}
```

Admission checks old values, partition ownership, overlap, and the advertised
fingerprint contribution. Identical batch replay is recognized. Calls to a
receiver's `apply()` are serialized; preparing disjoint batches can happen
independently. `snapshot()` shares the existing immutable state; compaction
preserves both the resolved entries and their fingerprint.

The reference model also has a `.rc` debug dump for inspecting or round-tripping
all live entries. This is a debugging feature, not an intended access pattern;
ordinary saves retain object roots through the catalog. See
[debug dumps](file-lifecycle.md#debug-dumps).

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
  using strings = everett::encoded_sort<everett::byte_encoding<>>;
  using P = everett::storage_policy<everett::tip<strings>>;
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
[native writing and merging](native-merges.md) for mapped inputs,
value-width rules and the continuation's exact limits.

For output larger than memory, `native_file_writer` and `native_file_merge`
stream their payload into a reserved private file and return a seal receipt at
completion. They retain sparse offsets until final Elias–Fano construction.
The [streamed timeline example](streamed-timeline.md) combines this path
with mapped inputs, a new terminal index and persistent timeline publication.

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
exact downstream samples. See [mapped blobs](mapped-blobs.md) for the
layout, lifetime and trust contracts.

We can also build a new index directly over a retained `mapped_native<P>`.
`index_builder<P, mapped_native<P>>` leaves its FC bytes and sparse offsets in
place; `finish_index` returns the new borrowed stream and navigation structures
as a `profile_index<P>`. `sample_cursor<P, mapped_blob<P>>` supplies samples from
an exact mapped target. For a terminal pair, `profile_index<P>::native_only`
constructs the zero directories from the native count without reading its keys.
See [sampling](sampling.md) for ownership and target-validation contracts.

`file_index_builder<P, mapped_native<P>>` sends the borrowed stream directly to
a private `.index` attempt. `file_index_pipeline<P>` builds several links
together, passing front-coded samples through bounded queues and sealing the
results from target to head. The [file-index example](file-indexes.md)
constructs and queries a complete mapped chain without collecting its borrowed
payloads in memory.

`multiverse<P>::seal_object` writes a body under caller-reserved object and
attempt identities. It accepts a contiguous span or borrowed chunks, including
mmap-backed input. The writer computes CRC32C while streaming, seals a private
file, installs its name without replacing an existing object, and flushes the
directory chain. A failed operation retains surviving outputs and reports its
identities and last acknowledged stage. The [sealing protocol](object-writer.md)
spells out Linux/macOS barriers and the caller's recovery obligations. Sealing
an object produces the receipt that `sqlite_catalog::record_sealed` records
before the pair is registered and saved.

`object_stream<P>` accepts body chunks across calls when the final extent is
not yet known. It can reserve a directory prefix and fill it at completion;
CRC combination accounts for that replacement without rereading the body.
An unfinished stream retains a private attempt, and `finish` seals it using
the same persistence protocol.

For already trusted objects, pass `file_open_mode::trusted` to `open`,
`from_slice`, or `multiverse<P>::open_object`. This avoids reading even the
header page. `body()` returns the physical bytes after the 96-byte envelope;
requesting `header()` explicitly reads and validates the metadata, returning it
by value. `scan()` still performs full validation. Trusted opening assumes the
caller already knows the object's type, policy and format; checked opening is
the default.

The optional `sqlite_catalog<P>` reserves objects before writing, records their
seal receipts, registers exact query chains and retains named saves, timeline
generations and reader pins. We can close it, reopen a save and query its mmap chain. Enable
`EVERETT_ENABLE_SQLITE` and link `everett::sqlite`; the ordinary core target has
no SQLite dependency. The [working catalog guide](sqlite-catalog.md) gives a
complete publication/reopen example and explains operation replay and failed
commits. Timelines support conditional publication and forks from exact historical
generations; a stale publication returns the observed head, and replay returns
that same outcome. Generations currently retain their pins. The
[catalog design](catalog.md) covers retirement and merge progress.
This leaves two immutable object kinds for us to
manage. The [durability protocol](durability.md)
orders verified output, durable publication, and old-pin release, with explicit
recovery states after failed synchronization.

Field Guide
-----------

### Blobs, worlds, and ownership

A **native blob** holds sorted key/value records. A **fractional index** holds
selected keys borrowed from a particular target. Together they form a searchable
pair. A **world** is one logical state, retaining a collection of objects. A
timeline describes successive worlds; a branch point retains a place from which
another timeline can grow. The **multiverse** holds the backing store.
Uppercase **COLA** names the cache-oblivious lookahead-array organization behind
the indexes. Worlds share immutable objects in a multiverse; a session follows the current world.

| Component | What it gives you |
| --- | --- |
| `storage_policy` | A sort registry plus sampling, physical block size and backspace code; units and width hints come from its sorts. |
| `elias_fano`, `rank_groups` | Monotone offsets and grouped origin counts, independent of the key representation. |
| `profile_array`, `profile_view`, `profile_cursor` | Encoded records, borrowed views, and sequential decoding. |
| `profile_native_writer`, `native_merge_builder` | Incremental native encoding and ordered per-key value composition. |
| `native_file_writer`, `native_file_merge` | The same native framing and merge semantics with bounded payload buffering into private files. |
| `profile_blob`, `profile_index` | A complete native/index pair, or an independently constructed index for existing native storage. |
| `sample_cursor`, `index_builder`, `index_pipeline` | Sampling an existing pair and building new index links incrementally. |
| `file_index_builder`, `file_index_pipeline` | Streaming those index links to files while retaining the original mapped native data. |
| `query_root`, `query_root_builder`, `query_cursor` | Preparing a bounded search head and visiting matching native entries through an exact index chain. |
| `cola_index`, `cola_index_builder`, `cola_query_root` | Two-route main/secondary catalogs, incremental construction and all matching native contributions. |
| `cola_local_merge_job` | A native merge, its destination index and replacement routing, with explicit stage boundaries and retained inputs. |
| `mapped_file`, `file`, `multiverse` | Retained read-only mappings and policy-checked object access. |
| `encode_native_sections`, `encode_index_sections`, `mapped_blob`, `mapped_query_root` | Portable blob files and queries over exact pinned mmap chains. |
| `encode_cola_sections`, `mapped_cola_blob`, `mapped_cola_query_root` | IX03 indexes over unchanged native files, with one recursive main route and one terminal secondary route. |
| `mapped_cola_index_builder` | Constructing those two routes directly from pinned mappings, without rewriting native files. |
| `object_writer`, `object_stream`, `multiverse::seal_object` | Immutable object construction with explicit persistence barriers and retained failure identities. |
| `sqlite_catalog` | Durable reservations, exact file graphs, named saves, timeline generations and reader pins. |
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
their dependencies simultaneously. The [file lifecycle](file-lifecycle.md)
and [catalog design](catalog.md) describe that transition on disk.

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
level growth are separate choices. The [sampling analysis](sampling.md)
explains their interaction, including why sampling counts augmented occurrences
rather than distinct keys.

For redundant COLA levels, a main catalog borrows from two targets. The main
target continues the cascade; the secondary target ends at its native array.
We keep two borrowed streams and two population directories over their shared
three-way order. Native rank follows by subtracting both borrowed counts. This
lets a query visit at most two arrays per level while shadow construction
proceeds separately. The [COLA guide](cola-indexes.md) includes a complete
two-target example and the mapped file layout; the
[scheduling design](cola-scheduling.md) explains visibility and work.
The [mapped save and merge example](cola-store.md) writes files, retains
both input contributions, publishes their merge and reopens the saved states.
The [local merge example](cola-merges.md) drives both main and secondary
destination plans while retaining the old query root.
The [space report](../bench/space_accounting.md) measures the resulting arrays.
The [construction benchmark](../bench/cola_frontier.md) measures the effect of
carrying prefix comparisons through sampling and merging.

### String compression and offsets

Sorted strings share prefixes, so we can encode a key by backspacing from its
predecessor and appending a suffix. Each physical stream uses ordinary front
coding. A search carries comparison state against its query: the known prefix
agreement, comparison direction and the full key length when known. It compares the next
literal without reconstructing the inherited prefix.

The borrowed predecessor before a window needs one extra scalar: its exact LCP
with the incoming boundary. Because those two keys and the query are ordered,
the smaller adjacent LCP gives its agreement with the query. This scalar belongs
to the exact index view. Rebuilding an index recomputes it while retaining the
native bytes. The [comparison argument](comparison-fc.md) covers ties,
endpoints and the separate roles of physical and virtual boundaries.

The byte profile counts lengths and offsets in bytes. The bit profile works
with densely packed, most-significant-bit-first strings and counts in bits.
Both use the same policy-bound interfaces. A fixed value width counts the chosen
leaf encoding's units: `byte_encoding<fixed_values<3>>` means three bytes,
and `bit_encoding<fixed_values<3>>` means three bits. A byte leaf nested in a
bit registry still has a 24-bit value. Borrowed records carry zero value bits while retaining the
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
For consecutive positions, `view.cursor().next()` walks the high words forwards
and retains the unconsumed bits. Native profile cursors use this traversal at
block boundaries; random lookups still use `select`.
Fixed-width values give us an additional saving: their contribution to an offset is predictable, so we subtract it before
encoding and add it back on access. If width is `w` and record ordinal is `i`,
the contribution is `w * i` in the same address units. The fixed payload stride
consequently does not inflate the residual offset universe. The terminal sample
uses the actual record count, including a short final block. Physical block
width `W` is independent of cascade stride `K`; both are part of the policy.
Each block starts with an absolute retained-prefix length, then uses relative
backspaces. We can parse controls before the selected lane without reading the
preceding block or reconstructing those earlier keys.

The native and borrowed streams each have a sparse offset directory. A
single-route pair has two such directories and one grouped rank; a two-route
COLA pair has three directories and two grouped ranks. Exact cut LCPs connect
their physical byte/bit streams to one virtual order. See
[key policies](keys.md) for the framing and reconstruction contracts.

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
[strong-deletion protocol](rebuild.md) uses that accounting to replace
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
[categorical update design](arrows.md) treats each key's update as an arrow
between states. A list can describe prepends, appends and deletions as composable
edits, while another key can choose a different vocabulary of changes. This keeps
the storage mechanism useful beyond one interpretation of a map.

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

The [blob and pipeline comparison](../bench/blob_pipeline.md) measures complete
in-memory builds, three-link index construction and searches within a known
window. It checks the resulting bytes and answers against the same inputs and
an independent catalog oracle. The component reports cover
[key and bit operations](../bench/key_bits.md),
[Elias–Fano](../bench/select_compare.md),
[grouped and bitmap rank](../bench/other_rank.md), and
[rank15](../bench/rank_compare.md), with source, raw trials and reproduction
commands. The [whole-query comparison](../bench/query_compare.md) includes root
preparation and traversal through every catalog, with owned results. Ordinary FC
with exact cut comparisons uses 38–48% less median query time in its M2 Max
fixtures, with counted backing arrays changing by less than 1%. Root preparation
has mixed results. These are resident-memory measurements; disk faults remain
a separate cost.

A [count-decoding follow-up](../bench/small_count.md) measures the common bit
counts from one bounded field. It reduces complete bit-query time by another
26.0–26.9% against its ordinary-FC baseline, without changing the encoded arrays.
The byte-profile timing ranges overlap. Each report gives its own exact
baseline, fixtures and validation; these are separate measurements.

The incremental writers keep the previous key buffer and replace only its
changed suffix after an append succeeds. Complete append/finalize measurements
show [26–46% less time for borrowed keys](../bench/borrowed_prefix.md) and
[18–44% less for native key/value records](../bench/native_prefix.md). These runs
verify identical encoded sections and decoded contents. The retained buffer
capacity can grow to the largest key seen; output bytes and format do not change.

Proofs
------

The [Lean model](../proof/README.md) checks the algebra, ownership and fractional
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
[ordinary-FC design](comparison-fc.md). An exact LCP at each cut can repair
the preceding borrowed comparison state without repeating its prefix. Connecting
those theorems to encoded metadata and the complete cursor is the next proof
obligation. The complete query tests separately check the encoded implementation
against independent native-array oracles.

Building and Testing
--------------------

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
overloads, and source locations. See [Doxygen conventions](doxygen.md) and
[contributor instructions](../AGENTS.md#coding-style) for the corresponding source conventions.

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

The [design's annotated references](design.md#11-references-and-their-roles)
include fractional cascading, succinct rank, persistent streaming indexes,
compressed string merging, and live-size rebuilding. For a particular concern:

- [Sampling and index construction](sampling.md) gives the exact occurrence and work accounting.
- [Network admission](network-admission.md) explains reuse of received native bytes.
- [Sorts and keys](keys.md) describes units, framing, and prefix-free sort codes.
- [Strong deletion](rebuild.md) connects tombstones to rebuilding credit.
- [Categorical updates](arrows.md) develops composition beyond replacement tables.
