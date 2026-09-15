Everett: Persistent Storage Through Composable Change
====================================================

I'm building Everett, a C++20 library, around immutable sorted blobs,
small changes, and explicit ownership. I want compressed string
keys, fractional cascading, shared snapshots, partitioned updates, and
fingerprints that survive changes in physical representation.

My starting point is simple: a large table should be able to accept a small
change as a small object. Readers should keep using the exact objects they
already own. Compaction should create something new, and a snapshot should be
cheap enough to take before trying an idea. Once those choices are made,
searching, compression, reclamation, and scheduling become parts of the same
design.

Everett 0.1.0 is experimental. The header-only library provides encoded storage
components, a mapped read side, and an in-memory reference world; the persistent
runtime is still being integrated. I track the tested contracts in the
[implementation ledger](docs/implementation.md). APIs and persisted formats may
change during this work.

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
| `storage_policy` | One choice of byte/bit units, value layout, and group size throughout a type family. |
| `profile_array`, `profile_view`, `profile_cursor` | Encoded records, borrowed views, and sequential decoding. |
| `profile_blob` | Native records, a separate borrowed stream, group navigation, and false-borrow flags. |
| `sample_cursor`, `index_builder`, `index_pipeline` | Sampling an existing pair and building new index links incrementally. |
| `mapped_file`, `file`, `multiverse` | Retained read-only mappings and policy-checked object access. |
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

Independent binary searches through many sorted runs repeat work. Fractional
cascading carries information from one search into the next: selected target
keys establish a small interval in the following pair.

I represent the virtual merge of native and borrowed keys using grouped
counts. At a group boundary, rank tells us how many occurrences came from the
borrowed stream; subtraction gives the native count. The projected ranges
contain at most `K` occurrences in total. Native entries come first on equality,
and borrowed duplicates remain distinct. A borrowed key also present natively
gets a **false-borrow** flag, preserving both its lookup meaning and its position.

The group size is a policy choice of the form `K = 2^r - 1`:

| `K` | Bits per group count | Sampling positions |
| ---: | ---: | --- |
| 3 | 2 | 0, 3, 6, … |
| 7 | 3 | 0, 7, 14, … |
| 15 | 4 | 0, 15, 30, … |
| 31 | 5 | 0, 31, 62, … |

I've made fifteen the default. Smaller groups spend more index space to narrow
the next search; larger groups amortize metadata over more records. Group size and
level growth are separate choices. The [sampling analysis](docs/sampling.md)
explains their interaction, including why sampling counts augmented occurrences
rather than distinct keys.

### String compression and offsets

Sorted strings share prefixes. Front coding records how far to backspace from
the preceding key and which suffix to append. I use locality-preserving front
coding (LPFC) for native arrays, so a lookup has a controlled starting context.
I encode borrowed streams separately so they can exploit the context known at
shared group boundaries. Rebuilding a borrowed stream therefore leaves the
native bytes intact.

The byte profile counts lengths and offsets in bytes. The bit profile works
with densely packed, most-significant-bit-first strings and counts in bits.
Both use the same policy-bound interfaces. A fixed value width counts the chosen
units: `fixed_values<3>` means three bytes under a byte policy and three bits
under a bit policy. Borrowed records carry zero value bits while retaining the
same policy family.

Each physical stream marks group starts and an end sentinel. Elias–Fano encodes
those monotone offsets. With fixed-width values, the offset directory subtracts
the predictable value contribution before encoding it, then adds it back during
access. If width is `w` and record ordinal is `i`, the contribution is `w * i`
in the same address units. The fixed payload stride consequently does not inflate
the residual offset universe; value width can still affect where native LPFC
chooses to restart. The terminal sample uses the actual record count,
including a short final group.

This is why the blob has two sparse offset structures and a grouped rank
structure: two physical byte/bit streams, one virtual order. See
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
applies the shared-cut constraints required by its final index.

Stages are supplied nearest the target first. Bounded queues let a downstream
stage pause its producer. `step(budget)` advances the pipeline in work quanta;
`finish()` assembles the completed directories and binds the resulting pairs
to their targets. A quantum can include up to `K` source occurrences. Key
reconstruction, comparisons, and final directory construction have their own
costs, so an entry budget is not a byte or wall-clock deadline.

### Updates and agreement

A replacement table represents deletion with an absent value. Updates carry
both the old and new binding, allowing admission to verify that an actual live
entry is being deleted. That check matters for live-size accounting: an invented
tombstone cannot earn rebuilding credit. The [strong-deletion protocol](docs/rebuild.md)
uses that accounting to replace accumulated history with a smaller live table.

I summarize the reference world's resolved contents with
`sum(h_key(key) * h_value(value))`, taking the hash of an absent value as zero.
An update adds the difference between its new and old binding. The result is
independent of compaction and of the admission order of disjoint changesets.
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

I've kept each C++ example below a complete program. Include the component you
use and link the CMake interface target described under [building](#building).

### Choose byte or bit units

Policies make representation choices visible in types. Arrays, blobs, indexes,
and files that share a policy agree on the units used by their metadata.

```cpp
#include <everett/profile.h>

int main() {
  using bytes = everett::storage_policy<
    everett::profile_unit::byte, everett::fixed_values<8>, 15>;
  using bits = everett::storage_policy<
    everett::profile_unit::bit, everett::fixed_values<3>, 7>;

  static_assert(bytes::bits_per_unit == 8);
  static_assert(bits::bits_per_unit == 1);
  static_assert(*bytes::value_width == 8);
  static_assert(*bits::value_width == 3);

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
  auto boundary = profile_anchor<policy>::complete(records.front().key.view());
  auto found = blob.search_window(query.view(), 0, boundary);
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

Mapped Objects
--------------

The custom object kinds are `.kv` and `.index`. Physical identities use 32
lowercase hexadecimal digits, split into paths such as
`ab/cd/0123456789abcdef0123456789abcd.kv`. The two directory prefixes are removed
from the leaf name. Logical keys never become filesystem paths.

`mapped_file` provides read-only shared ownership of an opened regular file.
Bounded slices retain the mapping after the original owner is released.
`file<P>::open(path)` checks the object's magic, version, policy, extents,
padding, and CRC32C integrity; its current whole-body checksum validation reads
the entire object on open. `multiverse<P>` opens these objects beneath an
existing backing directory and exposes their associated policy-bound types.

I've chosen SQLite for worlds, pins, and merge progress, using ordinary metadata
tables as described in the [catalog design](docs/catalog.md). That leaves us
two immutable object kinds to manage. The [durability protocol](docs/durability.md)
orders verified output, durable publication, and old-pin release, with explicit
recovery states after failed synchronization.

Building
--------

Use CMake 3.20 or later and a C++20 compiler. Codec headers use the standard
library; the mapping backend uses the platform's native read-only mapping API.

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

I started with my functional
[`Data.Vector.Map`](https://hackage.haskell.org/package/structures-0.2/docs/Data-Vector-Map.html)
and its deamortized variant in `structures`. For Everett's levels, I use the
redundant COLA scheme from
[Cache-Oblivious Streaming B-trees](https://people.cs.georgetown.edu/~jfineman/papers/sbtree.pdf).
String locality and LPFC come from
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

Copyright 2026 Edward Kmett. Everett is available under
[BSD-2-Clause](LICENSES/BSD-2-Clause.txt) **OR**
[Apache-2.0](LICENSES/Apache-2.0.txt), at your option. Both complete license texts
are included in source and installed packages; see [LICENSE](LICENSE).

Contact Information
-------------------

Contributions, examples, and bug reports are welcome. Please use
[GitHub issues](https://github.com/ekmett/everett/issues) for reproducible problems
or design discussion, or contact me at [ekmett@gmail.com](mailto:ekmett@gmail.com).

-Edward Kmett
