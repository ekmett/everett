# Streaming a table into a persistent timeline

We can write sorted records directly to a private `.kv` attempt, merge mapped
inputs into another attempt, and publish the completed result through SQLite.
The payload need not accumulate in an in-memory array. The writer keeps a
64 KiB payload buffer, bounded control scratch and one residual offset per
physical block; finishing builds the Elias–Fano directory from those offsets.
The checked record writer also retains its previous key. The default merge
retains its two input keys as references into the pinned source files.

This complete program takes an existing empty object directory as its only
argument. The caller establishes that directory and its ancestors durably.
The example allocates deterministic identities because it creates a fresh
catalog; an application supplies its own fresh identity allocator.

```cpp
#include <everett/native_file_merge.h>
#include <everett/sqlite_catalog.h>
#include <array>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace everett;
using P = storage_policy<profile_unit::byte>;
using catalog = sqlite_catalog<P>;

std::optional<bit_string> lookup(mapped_query_root<P> const & root,
                                 std::string_view text) {
  auto key = bit_string::from_bytes(text);
  auto cursor = root.cursor(key.view());
  while (!cursor.done()) {
    cursor.step(1);
    if (cursor.has_match()) return cursor.take_match().value;
  }
  return std::nullopt;
}

int main(int argc, char ** argv) {
  if (argc != 2) return 64;
  std::filesystem::path directory = std::filesystem::canonical(argv[1]);
  {
    unsigned next = 1;
    auto fresh = [&] {
      char digits[33];
      std::snprintf(digits, sizeof digits, "%032x", next++);
      return object_id(digits);
    };
    auto metadata = catalog::create(directory, fresh());
    struct reservation { blob_identity ids; object_attempt_id attempt; };
    auto reserve = [&](std::span<blob_identity const> inputs) {
      reservation r{{fresh(), fresh()}, object_attempt_id(fresh().hex())};
      std::array outputs{
        catalog_object_reservation{r.ids.native, file_kind::native_blob},
        catalog_object_reservation{r.ids.index, file_kind::fractional_index}};
      metadata.reserve("reserve-" + r.ids.index.hex(), r.attempt,
        "builder-" + r.ids.index.hex(), inputs, outputs);
      return r;
    };
    auto complete = [&](reservation const & r, object_seal_receipt receipt) {
      metadata.record_sealed("native-" + r.ids.index.hex(), receipt);
      auto native = std::make_shared<mapped_native<P> const>(
        mapped_native<P>::open(receipt.path));
      auto index = profile_index<P>::native_only(native->size());
      auto encoded = encode_index_sections(index, r.ids.native);
      auto sealed = encoded.seal(directory, r.ids.index, r.attempt);
      metadata.record_sealed("index-" + r.ids.index.hex(), sealed);
      auto mapped = std::make_shared<mapped_index<P> const>(
        mapped_index<P>::open(sealed.path));
      auto root = mapped_query_root<P>::adopt_prepared(
        mapped_blob<P>::bind(r.ids, std::move(native), std::move(mapped)));
      metadata.register_chain("register-" + r.ids.index.hex(), root,
        catalog_admission::trusted);
      return root;
    };
    auto write = [&](std::initializer_list<std::pair<std::string_view, std::string_view>> records) {
      auto r = reserve({});
      native_file_writer<P> writer(directory, r.ids.native, r.attempt);
      for (auto const & [name, contents] : records) {
        auto key = bit_string::from_bytes(name);
        auto value = bit_string::from_bytes(contents);
        writer.append(key.view(), value.view());
      }
      return complete(r, writer.finish());
    };

    auto older = write({{"alpha", "before"}, {"beta", "retained"}});
    auto first = metadata.create_timeline("create-main", "main", older.head()->identity());
    metadata.save("save-before", "before", first.head);
    auto newer = write({{"alpha", "after"}, {"gamma", "added"}});
    std::array inputs{older.head()->identity(), newer.head()->identity()};
    auto output = reserve(inputs);
    native_file_merge<P, mapped_native<P>> merge(directory,
      output.ids.native, output.attempt,
      older.head()->native_object(), newer.head()->native_object());
    while (!merge.done()) merge.step(1);
    auto candidate = complete(output, merge.finish());
    auto published = metadata.publish_timeline("publish-main", first, candidate.head()->identity());
    if (!published.published) return 1;
  }

  auto metadata = catalog::open(directory);
  auto saved = metadata.acquire_save("read-before", "before", "example-reader");
  auto before = open_mapped_query<P>(directory, saved.head);
  auto current = metadata.find_timeline("main");
  if (!current) return 2;
  auto after = open_mapped_query<P>(directory, current->head);
  if (lookup(before, "alpha") != bit_string::from_bytes("before") || lookup(before, "gamma")) return 3;
  if (lookup(after, "alpha") != bit_string::from_bytes("after") ||
      lookup(after, "beta") != bit_string::from_bytes("retained") ||
      lookup(after, "gamma") != bit_string::from_bytes("added")) return 4;
}
```

Link with `everett::sqlite` as described in the [catalog guide](sqlite-catalog.md).
The example uses trusted admission because the checked writers establish the
sorted-key encoding and the terminal indexes contain no borrowed samples.
Received or uncertain objects can require explicit scans instead.

The tables here have at most $K$ records, so a terminal pair is already a
prepared query root. A larger head needs an empty-native routing prefix built
from its augmented samples before publication. The
[mapped index builder](sampling.md#constructing-samples-from-a-pinned-pair)
builds those links without rewriting the existing native files.

`step` limits distinct keys, not bytes or elapsed time. Large literals, values,
composition callbacks and final metadata construction have their own costs.
The underlying output streams complete bytes and CRC state as construction
proceeds, then backpatch the private directory at finalization without a body
readback. Both public file wrappers are nonmovable; their input mappings stay
pinned while a merge pauses or fails.

Only a successful `finish` seals an object. An interrupted private output is
retained for reconciliation, but the merge does not yet restore its construction
state after a process restart. The [continuation contract](merge-resumption.md)
separates that work from the already durable publication of completed files.
