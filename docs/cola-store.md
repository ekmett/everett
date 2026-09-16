Saving and merging mapped COLA roots
===================================

We can write native files, build two-route indexes directly over their mappings,
save the resulting roots and publish a later native merge. The native inputs
stay immutable throughout. A secondary route uses the native file of its
target; it needs no separate secondary index.

This complete program takes an existing empty directory as its argument. As in
the [streamed timeline example](streamed-timeline.md), the caller establishes
that directory and its ancestors durably. Fresh catalogs let the example use a
simple deterministic identity allocator. An application supplies its own
allocator and retains its operation identities for retry.

```cpp
#include <everett/native_file_merge.h>
#include <everett/sqlite_catalog.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace everett;
using P = storage_policy<everett::tip<everett::encoded_sort<everett::byte_encoding<>>>>;
using catalog = sqlite_catalog<P>;
using pair_type = mapped_cola_blob<P>::pair_type;

std::vector<bit_string> values(mapped_cola_query_root<P> const & root,
                               std::string_view text) {
  auto key = bit_string::from_bytes(text);
  auto cursor = root.cursor(key.view());
  std::vector<bit_string> result;
  while (!cursor.done()) {
    cursor.step(1);
    while (cursor.has_match()) result.push_back(cursor.take_match().value);
  }
  return result;
}

int main(int argc, char ** argv) {
  if (argc != 2) return 64;
  auto directory = std::filesystem::canonical(argv[1]);
  {
    unsigned next = 1;
    auto fresh = [&] {
      char text[33];
      std::snprintf(text, sizeof text, "%032x", next++);
      return object_id(text);
    };
    auto metadata = catalog::create_cola(directory, fresh());
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
    auto complete = [&](reservation const & r, object_seal_receipt receipt,
                        pair_type main = {}, pair_type secondary = {}) {
      metadata.record_sealed("native-" + r.ids.index.hex(), receipt);
      auto native = std::make_shared<mapped_native<P> const>(
        mapped_native<P>::open(receipt.path));
      auto side = secondary ? secondary->native_object() :
        std::shared_ptr<mapped_native<P> const>{};
      auto side_id = secondary ? std::optional{secondary->identity().native} : std::nullopt;
      mapped_cola_index_builder<P> builder(native, main, side);
      while (!builder.done()) builder.step(16);
      auto artifact = builder.finish();
      auto encoded = encode_cola_sections(artifact, r.ids.native,
        main ? std::optional{main->identity()} : std::nullopt, side_id);
      auto sealed = encoded.seal(directory, r.ids.index, r.attempt);
      metadata.record_sealed("index-" + r.ids.index.hex(), sealed);
      auto index = std::make_shared<mapped_cola_index<P> const>(
        mapped_cola_index<P>::open(sealed.path));
      auto pair = mapped_cola_blob<P>::bind(r.ids, native, index, main, side, side_id);
      auto root = mapped_cola_query_root<P>::adopt_prepared(pair);
      metadata.register_chain("register-" + r.ids.index.hex(), root);
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

    // Keep both inputs searchable while the native merge proceeds.
    auto routing = reserve(inputs);
    native_file_writer<P> empty(directory, routing.ids.native, routing.attempt);
    auto combined = complete(routing, empty.finish(), older.head(), newer.head());
    metadata.save("save-combined", "combined", combined.head()->identity());
    auto second = metadata.publish_timeline("publish-combined", first, combined.head()->identity());
    if (!second.published) return 1;

    auto output = reserve(inputs);
    native_file_merge<P, mapped_native<P>> merge(directory, output.ids.native,
      output.attempt, older.head()->native_object(), newer.head()->native_object());
    while (!merge.done()) merge.step(1);
    auto merged = complete(output, merge.finish());
    if (!metadata.publish_timeline("publish-merged", second.head,
        merged.head()->identity()).published) return 2;
  }

  auto metadata = catalog::open(directory);
  auto before_pin = metadata.acquire_save("read-before", "before", "reader-before");
  auto combined_pin = metadata.acquire_save("read-combined", "combined", "reader-combined");
  auto before = open_mapped_cola_query<P>(directory, before_pin.head);
  auto combined = open_mapped_cola_query<P>(directory, combined_pin.head);
  auto current = metadata.find_timeline("main");
  if (!current) return 3;
  auto merged = open_mapped_cola_query<P>(directory, current->head);
  if (values(before, "alpha") != std::vector{bit_string::from_bytes("before")}) return 4;
  auto both = values(combined, "alpha");
  if (both.size() != 2 || std::count(both.begin(), both.end(), bit_string::from_bytes("before")) != 1 ||
      std::count(both.begin(), both.end(), bit_string::from_bytes("after")) != 1) return 5;
  if (values(merged, "alpha") != std::vector{bit_string::from_bytes("after")} ||
      values(merged, "beta") != std::vector{bit_string::from_bytes("retained")} ||
      values(merged, "gamma") != std::vector{bit_string::from_bytes("added")}) return 6;
}
```

Link with `everett::sqlite`. The example's native writer streams its payload;
the COLA builder reads mapped native inputs and retains the two encoded borrowed
outputs and compact directories in memory until sealing. It never rewrites the
input `.kv` files. The small tables fit one root group. Larger roots need a
prepared routing prefix before publication.

The combined save retains two chronological contributions for `alpha`; query
encounter order does not choose their meaning. Here the merge receives older
then newer input and uses replacement composition, so the merged root contains
`after`. Both saved representations remain readable after the timeline moves.

The program drives each step explicitly. It demonstrates durable completed
objects and root publication; the [scheduler model](cola-scheduling.md) supplies
the separate main/secondary/shadow work and visibility rules. Restoring an
interrupted private merge is covered by the
[continuation design](merge-resumption.md).
