Building a COLA merge
====================

`cola_local_merge_job<P, Compose>` joins the native merge and fractional-index
builders into one local job. We give it two native inputs in chronological
order and an exact destination plan. It retains those inputs while constructing
the destination and the empty-native routing index that will replace the
consumed source arrays. I call that routing index the **carrier**.

The completed result contains immutable objects. Publishing them, assigning
logical slots and choosing the next job belong to the
[COLA scheduler](cola-scheduling.md). We can keep querying an old root throughout
construction because the job never changes its inputs or their indexes.

Choose a destination
--------------------

| Plan | Destination | Carrier targets |
| --- | --- | --- |
| `cola_destination_plan<P>::for_main(main, secondary)` | A new main native/index pair, retaining the specified deeper targets | New main pair |
| `cola_destination_plan<P>::for_secondary(main)` | A new native-only secondary alongside an existing main | Existing main pair and new secondary native |

The main plan accepts no targets for a terminal destination. A secondary plan
requires an existing main. All handles are exact immutable dependencies;
equivalent table contents do not make a different target interchangeable.

The caller establishes that the two source natives represent adjacent
chronological factors. Sorting keys cannot establish that history premise.
Default composition keeps the newer value on equal keys. A custom `Compose`
uses the same key-aware or value-only interface as
[`native_merge_builder`](native-merges.md).

Drive the stages
----------------

| Phase | `step(budget)` work | `finish_stage()` transition |
| --- | --- | --- |
| `native_merge` | At most `budget` distinct output keys | Finalize native encoding; start destination indexing or the carrier |
| `destination_index` | At most `budget` local augmented occurrences | Finalize the main index; start the carrier |
| `carrier_index` | At most `budget` local augmented occurrences | Finalize the carrier; mark the result ready |

`stage_done()` indicates that a stage has consumed its input. `step` never
crosses a stage boundary implicitly. Construction and `finish_stage` can parse
initial keys, allocate output and finalize Elias–Fano/rank metadata. Their work
is separate from the record budget. Key bytes and composition cost also need
their own service accounting.

After `done()` becomes true, `finish()` transfers the completed result. Its
`main` and `secondary` fields name the destination level, `merged_native` is the
new data, and `carrier` is the source level's empty-native routing artifact.
A local carrier need not fit a single root window; prepare a query root before
using it as a published head.

This complete program runs both destination plans and checks the old snapshot:

```cpp
#include <utility>

#include <array>
#include <cassert>
#include <memory>
#include <vector>

import everett;

int main() {
  using namespace everett;
  using P = storage_policy<everett::tip<everett::encoded_sort<everett::byte_encoding<>>>>;
  using node = cola_index<P>;
  auto record = [](char const * key, char const * value) {
    return profile_record{bit_string::from_bytes(key), bit_string::from_bytes(value)};
  };
  std::array old_rows{record("item", "before"), record("stay", "retained")};
  std::array new_rows{record("item", "after"), record("new", "added")};
  std::array deep_rows{record("older", "deeper")};
  auto older = std::make_shared<profile_array<P> const>(profile_array<P>::build(old_rows));
  auto newer = std::make_shared<profile_array<P> const>(profile_array<P>::build(new_rows));
  auto deeper = std::make_shared<node const>(node::build(deep_rows));
  auto original = std::make_shared<node const>(node::adopt_native(older));
  auto saved = cola_query_root<P>::build(original, newer);
  auto values = [](auto const & root, char const * text) {
    auto key = bit_string::from_bytes(text);
    auto cursor = root.cursor(key.view());
    std::vector<bit_string> result;
    while (!cursor.done()) {
      cursor.step(1);
      while (cursor.has_match()) result.push_back(cursor.take_match().value);
    }
    return result;
  };
  for (bool secondary : {false, true}) {
    auto plan = secondary ? cola_destination_plan<P>::for_secondary(deeper) :
      cola_destination_plan<P>::for_main(deeper);
    cola_local_merge_job<P> job(older, newer, std::move(plan));
    while (!job.done()) {
      if (job.stage_done()) job.finish_stage();
      else job.step(1);
    }
    auto result = job.finish();
    auto current = cola_query_root<P>::build(result.carrier);
    assert(values(current, "item") == std::vector{bit_string::from_bytes("after")});
    assert(values(current, "stay") == std::vector{bit_string::from_bytes("retained")});
    assert(values(current, "new") == std::vector{bit_string::from_bytes("added")});
    assert(values(current, "older") == std::vector{bit_string::from_bytes("deeper")});
    assert(values(saved, "item").size() == 2);
  }
}
```

Moving a job transfers stable builder objects, including composition state.
Execution or finalization failure poisons the job and retains its source and
plan owners. Rejected preconditions, such as finishing an incomplete stage,
leave a valid job usable. No partial result can be obtained through `finish`.

This component owns its encoded output in memory. The
[mapped persistence example](cola-store.md) separately demonstrates native file
merging, mapped index construction, sealing and catalog publication. A durable
local job still needs the [continuation contract](merge-resumption.md) between
those operations.
