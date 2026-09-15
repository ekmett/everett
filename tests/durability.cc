/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/durability.h>

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
  using namespace everett;
  constexpr auto durable = persistence_result::durable_verified;
  constexpr auto failed = persistence_result::failure;
  constexpr auto verified = recovery_evidence::durable_state_verified;

  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F action) {
    bool threw = false;
    try { action(); } catch (std::exception const &) { threw = true; }
    require(threw, "invalid transition accepted");
  }

  merge_identity identity() { return {"byte-order/front-v1/keep-tombstones", {"input-A-v1", "input-B-v3"}}; }
  merge_publication publication() { return {identity(), "manifest-old", "output-attempt-1"}; }
  merge_checkpoint checkpoint(std::string id = "checkpoint-1") {
    return {std::move(id), identity(), "output-attempt-1",
            {{"input-A-v1", 15, 101, "abcdefgh"}, {"input-B-v3", 12, 92, "abcdefgz"}},
            {{"sealed-extent-1", 123, "digest-extent-1"}}, "abcdefgz",
            "codec-v1:rank-class=7;select-offset-spool=93;resolved-count=27", 27, 400};
  }

  void retained(merge_publication const & model) {
    require(model.old_pins_retained(), "old durable pins released after failure");
    require(model.durable_manifest() == "manifest-old", "old recovery root forgotten");
    require(!model.can_release_old_pins(), "uncertain output became reclaim authority");
  }

  void test_publication_failures() {
    for (unsigned point = 0; point < 4; ++point) {
      auto model = publication();
      rejects([&] { model.release_old_pins(); });
      rejects([&] { model.prepare_manifest("too-early"); });
      if (point == 0) model.complete_checkpoint(checkpoint(), failed);
      else {
        model.complete_checkpoint(checkpoint(), durable);
        if (point == 1) model.complete_output(failed);
        else {
          model.complete_output(durable);
          if (point == 2) model.report_io_failure();
          else {
            model.prepare_manifest("manifest-candidate");
            model.complete_manifest(failed);
          }
        }
      }
      require(model.stage() == publication_stage::uncertain, "failure did not stop publication");
      retained(model);
      rejects([&] { model.release_old_pins(); });
      rejects([&] { model.complete_output(durable); });
      rejects([&] { model.complete_manifest(durable); });
      rejects([&] { model.resume("output-attempt-2", recovery_evidence::unverified); });
      rejects([&] { model.resume("output-attempt-1", verified); });
      if (point == 3) {
        rejects([&] { model.resume("output-attempt-2", verified); });
        model.resume("output-attempt-2", verified, std::nullopt, selector_recovery::old_root_selected);
      } else model.resume("output-attempt-2", verified);
      require(!model.checkpoint(), "full regeneration kept cursor state");
      model.complete_output(durable);
      model.prepare_manifest("manifest-new");
      model.complete_manifest(durable);
      require(model.can_release_old_pins() && model.old_pins_retained(), "publication released pins eagerly");
      require(model.durable_manifest() == "manifest-new", "new durable root missing");
      require(model.release_old_pins(), "first retirement did not release");
      require(!model.release_old_pins(), "retirement was not idempotent");
    }
  }

  void test_checkpoint_resumption() {
    auto model = publication();
    auto first = checkpoint();
    model.complete_checkpoint(first, durable);
    auto second = checkpoint("checkpoint-2");
    second.output_records = 40;
    second.completed_work = 700;
    second.inputs[0].ordinal = 28;
    second.inputs[0].byte_offset = 202;
    second.sealed_output.push_back({"sealed-extent-2", 47, "digest-extent-2"});
    model.complete_checkpoint(second, failed);
    require(model.checkpoint() == first, "failed checkpoint superseded durable checkpoint");
    rejects([&] { model.resume("output-attempt-2", verified, second); });
    auto altered = first;
    altered.inputs[0].predecessor_key = "wrong-context";
    rejects([&] { model.resume("output-attempt-2", verified, altered); });
    rejects([&] { model.resume("output-attempt-2", recovery_evidence::unverified, first); });
    model.resume("output-attempt-2", verified, first);
    require(model.checkpoint() == first && model.checkpoint()->sealed_bytes() == 123,
            "verified resume context missing");
    require(model.checkpoint()->inputs[1].predecessor_key == "abcdefgz", "input decoding context lost");
    require(model.checkpoint()->previous_output_key == "abcdefgz", "output encoding context lost");
    rejects([&] { model.complete_checkpoint(second, durable); }); // stale attempt
    second.output_generation = "output-attempt-2";
    model.complete_checkpoint(second, durable);
    require(model.checkpoint()->sealed_bytes() == 170, "checkpoint extent accounting");
    model.report_io_failure();
    rejects([&] { model.resume("output-attempt-1", verified, second); }); // no reused generation
    model.resume("output-attempt-3", verified, second);
    model.complete_output(durable);
    model.prepare_manifest("manifest-new");
    model.complete_manifest(durable);
    require(model.release_old_pins(), "resumed merge could not publish");
  }

  void test_manifest_uncertainty() {
    auto model = publication();
    model.complete_output(durable);
    model.prepare_manifest("manifest-maybe-durable");
    model.complete_manifest(failed);
    retained(model);
    rejects([&] { model.accept_recovered_publication(recovery_evidence::unverified); });
    model.accept_recovered_publication(verified);
    require(model.durable_manifest() == "manifest-maybe-durable", "verified publication was rolled back");
    require(model.old_pins_retained(), "recovery eagerly released old roots");
    model.release_old_pins();

    auto no_candidate = publication();
    no_candidate.complete_output(failed);
    rejects([&] { no_candidate.accept_recovered_publication(verified); });
    auto fresh = publication();
    fresh.complete_output(durable);
    fresh.prepare_manifest("manifest-attempt-1");
    fresh.complete_manifest(failed);
    rejects([&] { fresh.resume("output-attempt-2", verified); });
    require(fresh.pending_manifest() == "manifest-attempt-1", "unresolved selector was forgotten");
    fresh.resume("output-attempt-2", verified, std::nullopt, selector_recovery::old_root_selected);
    fresh.complete_output(durable);
    rejects([&] { fresh.prepare_manifest("manifest-attempt-1"); });
  }

  void test_clean_cache_failure() {
    // Minimal fake backend with the failure characteristic documented by
    // Rebello et al.: failed writeback clears dirty state but retains new
    // cache bytes. A later successful flush does not repair persisted bytes.
    struct fake_file {
      std::string cache;
      std::string disk;
      bool dirty = false;
      void write(std::string value) { cache = std::move(value); dirty = true; }
      bool flush(bool fail) {
        if (fail) { dirty = false; return false; }
        if (dirty) disk = cache;
        dirty = false;
        return true;
      }
    } output;
    auto model = publication();
    output.write("merged-blob");
    require(!output.flush(true), "injected writeback failure missing");
    model.complete_output(failed);
    require(output.flush(false), "retry was expected to report success");
    require(output.cache == "merged-blob" && output.disk.empty(), "fake cache masked failure incorrectly");
    rejects([&] { model.complete_output(durable); });
    retained(model);
    model.resume("fresh-output-object", verified);
    fake_file regenerated;
    regenerated.write("merged-blob");
    require(regenerated.flush(false) && regenerated.disk == "merged-blob", "regeneration did not persist");
    model.complete_output(durable);
    model.prepare_manifest("manifest-regenerated");
    model.complete_manifest(durable);
    require(model.release_old_pins(), "regenerated output could not replace inputs");
  }

  void test_checkpoint_rejection() {
    auto model = publication();
    auto bad = checkpoint();
    bad.identity.recipe = "another-precedence";
    rejects([&] { model.complete_checkpoint(bad, durable); });
    bad = checkpoint(); bad.inputs[0].input_version = "input-A-v2";
    rejects([&] { model.complete_checkpoint(bad, durable); });
    bad = checkpoint(); bad.sealed_output[0].checksum.clear();
    rejects([&] { model.complete_checkpoint(bad, durable); });
    bad = checkpoint(); bad.sealed_output.push_back(bad.sealed_output[0]);
    rejects([&] { model.complete_checkpoint(bad, durable); });
    bad = checkpoint(); bad.sealed_output[0].bytes = std::numeric_limits<std::uint64_t>::max();
    bad.sealed_output.push_back({"other", 1, "digest"});
    rejects([&] { model.complete_checkpoint(bad, durable); });
    model.complete_checkpoint(checkpoint(), durable);
    bad = checkpoint("checkpoint-behind"); bad.inputs[0].ordinal = 0;
    rejects([&] { model.complete_checkpoint(bad, durable); });
    require(model.stage() == publication_stage::building, "rejected metadata mutated protocol");
  }
}

int main() {
  try {
    test_publication_failures();
    test_checkpoint_resumption();
    test_manifest_uncertainty();
    test_clean_cache_failure();
    test_checkpoint_rejection();
    std::cout << "Merge publication, failure retention, and checkpoint resumption checks passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Everett's storage durability behavior.
 */
