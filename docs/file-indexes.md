# Streaming a fractional-index chain to disk

We can build several `.index` files together while keeping their native `.kv`
files unchanged. `file_index_pipeline<P>` samples the exact mapped target pair
once. Each stage merges those samples with its own native keys and passes a
front-coded sample stream to the next stage. A downstream result need not be
sealed before it supplies samples to an upstream builder.

The stages are ordered nearest the existing target first, new head last. Each
stage carries a pinned native owner, the new pair's reserved identity and a
private output attempt. `step` budgets cursor events through bounded queues.
`seal_next` finalizes one `.index`, proceeding from the target toward the head.
The completed receipts remain available if a later stage fails.

This complete example takes an existing empty object directory. It writes some
native files to stand in for already received or merged data, then indexes them
without recoding those files. Deterministic IDs are suitable for this fresh
example directory; a store reserves fresh IDs and durable dependency pins through
its catalog before opening the pipeline.

```cpp
#include <everett/file_index_pipeline.h>
#include <everett/native_file_writer.h>
#include <array>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace everett;
using P = storage_policy<profile_unit::byte, fixed_values<1>>;

bit_string key(unsigned i) {
  char text[16];
  std::snprintf(text, sizeof text, "k%06u", i);
  return bit_string::from_bytes(text);
}

int main(int argc, char ** argv) {
  if (argc != 2) return 64;
  auto directory = std::filesystem::canonical(argv[1]);
  unsigned next = 1;
  auto fresh = [&] {
    char text[33];
    std::snprintf(text, sizeof text, "%032x", next++);
    return object_id(text);
  };
  struct native_file {
    object_id id;
    std::shared_ptr<mapped_native<P> const> owner;
  };
  auto write = [&](unsigned count, unsigned stride, char tag) {
    auto id = fresh();
    native_file_writer<P> writer(directory, id, object_attempt_id(fresh().hex()));
    auto value = bit_string::from_bytes(std::string(1, tag));
    for (unsigned i = 0; i != count; ++i) {
      auto k = key(i * stride);
      writer.append(k.view(), value.view());
    }
    auto receipt = writer.finish();
    return native_file{id, std::make_shared<mapped_native<P> const>(
      mapped_native<P>::open(receipt.path))};
  };

  auto base = write(1024, 1, 'a');
  blob_identity base_id{base.id, fresh()};
  auto terminal = profile_index<P>::native_only(base.owner->size());
  auto terminal_receipt = encode_index_sections(terminal, base.id).seal(
    directory, base_id.index, object_attempt_id(fresh().hex()));
  auto target = mapped_blob<P>::bind(base_id, base.owner,
    std::make_shared<mapped_index<P> const>(mapped_index<P>::open(terminal_receipt.path)));

  auto middle = write(64, 8, 'b');
  auto upper = write(8, 64, 'c');
  auto empty = write(0, 1, ' ');
  std::vector<file_index_stage<P>> stages;
  for (auto const & native : std::array{middle, upper, empty})
    stages.push_back({native.owner, {native.id, fresh()}, object_attempt_id(fresh().hex())});

  file_index_pipeline<P> pipeline(directory, target, stages);
  while (!pipeline.done()) pipeline.step(256);
  auto head = target;
  while (pipeline.seal_next()) {
    auto i = pipeline.completed_receipts().size() - 1;
    auto const & receipt = pipeline.completed_receipts().back();
    auto index = std::make_shared<mapped_index<P> const>(mapped_index<P>::open(receipt.path));
    head = mapped_blob<P>::bind(stages[i].identity, stages[i].source, std::move(index), head);
  }
  auto root = mapped_query_root<P>::adopt_prepared(head);
  auto query = key(0);
  auto cursor = root.cursor(query.view());
  std::vector<bit_string> values;
  while (!cursor.done()) {
    cursor.step(1);
    if (cursor.has_match()) values.push_back(cursor.take_match().value);
  }
  return values == std::vector{bit_string::from_bytes("c"),
    bit_string::from_bytes("b"), bit_string::from_bytes("a")} ? 0 : 1;
}
```

The final empty-native stage makes this example's head fit in one sampling
group. The low-level query returns every matching native value; here the
chosen chain order gives `c`, `b`, then `a`. Resolving those matches according
to semantic chronology or an arrow policy belongs to the world owner.

Each file stage buffers at most 64 KiB of encoded payload, plus bounded control
scratch, current key contexts and navigation under construction. Navigation
includes rank classes, false-borrow flags, cut LCPs and sampled residual offsets;
it grows with the index. Sampling reduces occurrence counts, not necessarily
string bytes. Finishing builds Elias–Fano and rank metadata and performs file
barriers, outside the `step` event budget.

Only sealed receipts may be recorded as completed objects. The
[SQLite catalog](sqlite-catalog.md) records those receipts, registers the exact
prepared chain and conditionally publishes its root. Until then, `planned_head`
is an intended identity. It is not an admission or durability receipt. On any
failure, keep catalog ownership of inputs and surviving outputs while resolving
the outcome. These builders pause in a live process; they do not restore an
unfinished pipeline after restart.
