/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks tombstone caps against current immutable admission targets.
 *
 * \license
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/sort_runtime.h>
#include <everett/replacement_rebuild.h>

#include <iostream>

namespace {
  using namespace everett;
  using p = string_policy;
  using strings = unsorted<std::optional<std::string>>;
  void check(bool value, char const * why) { if (!value) throw std::runtime_error(why); }
  struct storage : sort_runtime_storage<p> {
    using base = sort_runtime_storage<p>;
    inline static std::optional<std::uint64_t> forced, observed;
    inline static unsigned admissions = 0;
    static auto sorted_native(std::span<profile_record const> input) {
      std::vector<profile_record> records(input.begin(), input.end());
      if (forced) for (auto & record : records) record.retained_limit_bits = forced;
      return base::sorted_native(records);
    }
    static auto singleton(profile_record const & record) {
      auto target = sort_profile_query<p, strings>("ab9");
      if (compare_bits(record.key.view(), target.view()) == 0)
        observed = record.retained_limit_bits;
      ++admissions;
      return base::singleton(record);
    }
  };
  using family = sort_runtime_family<p, registry_selector<string_registry>, storage>;
  using engine = typed_engine<p, wrapping_fingerprint_algebra, 256, family>;
  template <class E = engine> E make(std::optional<std::uint64_t> forced = {}) {
    E result;
    storage::forced = forced;
    auto batch = E::batch(); batch.put("a0", "anchor").put("ab9", "target");
    result.contribute(std::move(batch).finish());
    while (result.pending()) result.advance(4096);
    storage::forced.reset();
    return result;
  }
  std::uint64_t retention(engine::world_type const & state) {
    auto cursor = state.runtime().cursor_owned(sort_profile_query<p, strings>("ab9"));
    while (!cursor.done()) {
      cursor.step(1);
      if (cursor.has_match()) {
        auto hit = cursor.take_match();
        return hit.secondary ? hit.source->secondary_target()->view().encoded_at(hit.ordinal).retained :
          hit.source->view().native().encoded_at(hit.ordinal).retained;
      }
    }
    throw std::runtime_error("missing fixture target");
  }
  void current_target() {
    auto original = make(); auto base = original.snapshot();
    auto depth = retention(base);
    check(depth > 1, "fixture did not retain key-local bits");
    auto conditional = base.erase("ab9");
    check(conditional.records()[0].retained_limit_bits == depth, "base lookup lost physical depth");
    auto alternate = make(1); auto saved = alternate.snapshot();
    check(retention(saved) == 1 && saved.metadata() == base.metadata(), "alternate layout is not equivalent");
    storage::observed.reset(); storage::admissions = 0;
    auto deleted = alternate.contribute(conditional);
    check(storage::admissions == 1 && storage::observed == 1, "admission trusted the base's different encoding");
    check(!deleted.get("ab9") && deleted.get("a0") == "anchor" && saved.get("ab9") == "target",
      "deletion changed retained snapshot or another key");
    while (alternate.pending()) alternate.advance(4096);
    check(!alternate.snapshot().get("ab9") && alternate.snapshot().metadata() == deleted.metadata(),
      "merge changed tombstone semantics");
    // A lower cap already captured by the contribution must not be raised.
    auto conservative = saved.erase("ab9");
    original.contribute(conservative);
    check(storage::observed == 1, "admission raised an existing conservative cap");
  }
  void unconditional() {
    for (bool explicit_erase : {false, true}) {
      auto live = make(); auto base = live.snapshot();
      auto depth = retention(base);
      auto command = explicit_erase ? engine::erase("ab9") : engine::change("ab9", std::nullopt);
      check(!command.records()[0].retained_limit_bits, "unbound command invented target metadata");
      storage::observed.reset(); live.contribute(command);
      check(storage::observed == depth, "unbound tombstone missed current target metadata");
      auto count = storage::admissions;
      try {
        live.contribute(engine::erase("absent"));
        throw std::runtime_error("accepted absent deletion");
      } catch (std::invalid_argument const &) {}
      check(storage::admissions == count && !live.failed(), "invalid delete reached storage");
      live.contribute(engine::put("ab9", "revived"));
      check(!storage::observed && live.snapshot().get("ab9") == "revived", "revival retained a tombstone cap");
    }
  }
  void rebuilding() {
    using rebuilding_engine = replacement_rebuild_engine<p, wrapping_fingerprint_algebra, 256, family>;
    auto conservative = make<rebuilding_engine>(1);
    auto deletion = conservative.snapshot().erase("ab9");
    check(deletion.records()[0].retained_limit_bits == 1, "wrapper base cap missing");
    auto live = make<rebuilding_engine>();
    storage::observed.reset();
    auto result = live.contribute(deletion);
    check(storage::observed == 1 && !result.get("ab9"), "rebuild wrapper lost lower contribution cap");
  }
}
int main() {
  try { current_target(); unconditional(); rebuilding(); }
  catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
