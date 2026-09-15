COLA indexes and mapped queries
==============================

I use COLA's main/secondary arrangement to let merge construction proceed while
the old representation remains searchable. A main array indexes the next
level's main and secondary arrays. The main route continues; the secondary
route ends after searching that native array. This gives us two searchable
arrays per level without making a branching search tree.

The [scheduling design](cola-scheduling.md) explains when these arrays become
visible. This page covers the implemented index representation, construction,
file format and queries. Array placement and chronological composition remain
the caller's responsibility.

Two routes over one order
-------------------------

`cola_index<P>` owns an immutable native array and two borrowed streams:

| Stream | Sample source | Exact dependency |
| --- | --- | --- |
| Main borrow | Every `K`th occurrence in the next main's augmented catalog | Native/index pair |
| Secondary borrow | Every `K`th native entry in the next secondary | Native file |

Native, main-borrow and secondary-borrow keys form one virtual sorted order.
On equality, native precedes main-borrow, which precedes secondary-borrow.
Occurrences are retained, including borrowed copies of the same key. There is
no stored three-way merged key array.

Two `rank_groups<K>` directories count the borrowed populations at each cut.
Subtract both prefixes from the virtual ordinal to obtain the native prefix.
The three projected intervals share one budget of at most `K` occurrences.
Each borrowed stream has its own false-borrow bits and exact cut LCPs. Both
routes can recover the same local native entry; we emit that entry once.
Matching native entries in different arrays remain distinct contributions.

Each stream uses its own FC bytes and Elias–Fano physical block offsets. The
native bytes remain unchanged when we build a different index. Borrowed block
offsets use `P::codec_block_size`; virtual cuts and target sampling use
`P::group_size`. These are independent policy parameters.

With `K=15`, each rank directory uses four bits per virtual cut and a 64-bit
checkpoint per 128 cuts. There are two directories. The two exact cut-LCP
arrays currently use sixteen bytes per cut together. Each borrowed occurrence
adds one false-borrow bit in its own stream. Empty routes have zero population
and still occupy their navigation directories in IX03.

The [space measurements](../bench/space_accounting.md) account for all occupied
native and index arrays, including these zero-population directories. They
separate per-level capacity bounds from total stored bytes.

Constructing and searching
-------------------------

`cola_index_builder<P>` adopts an existing immutable native array and pins its
two targets. `step(budget)` consumes at most that many local merged occurrences.
Advancing a sampled target can decode up to `K` source occurrences. String
bytes, allocation and final Elias–Fano construction are additional work.
`finish()` returns the completed index; it does not change the native array.
With neither target present, all occurrences are native. Construction reads
only the admitted native count and emits zero navigation per group; it never
decodes native keys or accesses their payload pages. Native validation remains
the admission or explicit `scan` operation's responsibility.

`mapped_cola_index_builder<P>` does the same work with a pinned `mapped_native`
input, a mapped main pair and a mapped secondary native file. Its completed
`mapped_cola_artifact<P>` retains those mappings and owns the newly encoded
borrowed streams. `encode_cola_sections` seals that artifact as IX03; reopening
it gives us a homogeneous mapped query chain. No native data is copied or
re-encoded. The [save and merge example](cola-store.md) takes this path through
native construction, publication, snapshots and reopening.

`cola_query_root<P>::build(main, secondary)` includes both level-zero arrays.
It builds an empty-native routing parent where needed, then adds main-only
parents until the first augmented catalog fits in one group. An existing
prepared head can instead use `adopt_prepared`.

This complete program finds both contributions to one key:

```cpp
#include <everett/cola_query.h>

#include <array>
#include <cassert>
#include <memory>

int main() {
  using namespace everett;
  using P = storage_policy<profile_unit::byte>;
  using node = cola_index<P>;
  std::array older{profile_record{bit_string::from_bytes("path"),
                                 bit_string::from_bytes("before")}};
  std::array newer{profile_record{bit_string::from_bytes("path"),
                                 bit_string::from_bytes("after")}};
  auto main = std::make_shared<node const>(node::build(older));
  auto secondary = std::make_shared<profile_array<P> const>(
    profile_array<P>::build(newer));
  auto root = cola_query_root<P>::build(main, secondary);
  auto key = bit_string::from_bytes("path");
  auto cursor = root.cursor(key.view());
  unsigned matches = 0;
  while (!cursor.done()) {
    cursor.step(1);
    while (cursor.has_match()) {
      auto match = cursor.take_match();
      assert(match.ordinal == 0);
      assert(match.value == bit_string::from_bytes(
        match.secondary ? "after" : "before"));
      ++matches;
    }
  }
  assert(matches == 2);
}
```

A cursor visit searches one main window and at most one terminal secondary
window. It can produce two pending matches. Consume them before asking for
more work; each result owns its value and pins the node that identified its
source. A secondary result belongs to `source->secondary_target()`.

The cursor enumerates contributions. Its encounter order is not chronological
precedence. A store combines them using the history attached to those arrays.
Snapshots can share every immutable node while their query cursors advance
independently.

Portable IX03 files
-------------------

`encode_cola_sections(node, native_id, main_id, secondary_id)` borrows the
encoded streams and compact directories. Its `chunks()` can be sealed without
copying the entire body into another buffer. Keep both encoder and source alive
until the chunks have been consumed. `materialize()` produces complete file
bytes when an owning byte vector is useful.

The ordinary file envelope remains version 1. Native files retain KV02.
Two-route index files have an IX03 body with a 448-byte fixed directory and
18 sections. Integers and numeric sections use little-endian encoding; section
starts are aligned to eight bytes, with zero padding.

| Body byte offset | Field |
| ---: | --- |
| 0 | Four-byte `IX03` magic |
| 4, 6 | 16-bit version 3 and section count 18 |
| 8 | 64-bit virtual count |
| 16, 24 | Main and secondary borrowed counts |
| 32, 40 | FC extents, in policy units |
| 48, 56 | Terminal key lengths, in policy units |
| 64, 72 | Elias–Fano universes |
| 80, 81 | Elias–Fano low widths, one byte each |
| 82, 83 | Target-presence flags |
| 84 | Four reserved zero bytes |
| 88 | Native object identity, sixteen bytes |
| 104, 120 | Main target's native and index identities |
| 136 | Secondary target's native identity |
| 152 | Eight reserved zero bytes |
| 160 | Eighteen 16-byte `(offset, length)` section descriptors |

Sections 0–4 and 5–9 hold each borrowed stream's FC bytes, EF low words, high
words, sample records and sparse positions. Sections 10–13 hold main rank
classes/checkpoints followed by secondary rank classes/checkpoints. Sections
14–15 hold the two false-borrow vectors; 16–17 hold the two cut-LCP arrays.
The outer record count is the sum of the two borrowed counts. Index values
have fixed width zero. Absent target identities encode zero bytes.

`mapped_cola_index<P>::open` reads the fixed metadata and validates section
shapes. It does not scan FC, rank or EF contents. `mapped_cola_blob<P>::bind`
checks target declarations and sample counts, then pins the exact native
objects and main pair. The caller authenticates physical object identities.

`open_mapped_cola_query<P>(directory, head)` follows the main chain, opens each
secondary native file and caches shared native mappings. It adopts the already
prepared root without rebuilding indexes. `multiverse<P>::open_cola_query`
provides the same operation through the policy-bound backing directory.

For recovery or admission checks, `head->scan()` explicitly verifies checksums,
FC and EF contents, rank populations, false-borrow flags, cut LCPs and exact
target samples throughout the graph. Ordinary open and query leave that full
scan out of the path.

The [core tests](../tests/cola_index.cc) use original input bits to derive the
three-way order and expected query results independently. They cover byte/bit
policies, K=3/7/15/31, distinct codec block sizes, equality across all three
origins, empty and prefix keys, two pending matches, cursor ownership, malformed
population sums and inaccessible old literal prefixes.
