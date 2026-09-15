/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Diet's storage cola behavior.
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include "diet/cola.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
  using namespace diet;

  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }

  template <class F> void rejects(F && operation, char const * message) {
    try { operation(); }
    catch (std::exception const &) { return; }
    throw std::runtime_error(message);
  }

  struct partition {
    std::uint64_t operator()(std::string_view key) const {
      return key.empty() ? 0 : static_cast<unsigned char>(key.back()) % 3;
    }
  };

  // GF(2^8), represented in the low byte with x^8+x^4+x^3+x+1.
  // Deliberately supplies no division, comparison or multiplicative inverse.
  struct binary_field {
    using element = std::uint64_t;
    static constexpr element zero() { return 0; }
    static constexpr element lift(std::uint64_t x) { return x & 255; }
    static constexpr element add(element x, element y) { return x ^ y; }
    static constexpr element subtract(element x, element y) { return x ^ y; }
    static constexpr element multiply(element x, element y) {
      element result = 0;
      for (unsigned i = 0; i < 8; ++i) {
        if (y & 1) result ^= x;
        x = (x << 1) ^ ((x & 128) ? 0x11b : 0);
        y >>= 1;
      }
      return result;
    }
  };

  template <class A> void commuting_partitions() {
    using cola = reference_cola<std::uint64_t, A>;
    auto initial = cola::from_records({{"a", 10}, {"b", 20}, {"c", 30},
      {"other-a", 40}, {"", 50}, {std::string("zero\0b", 6), 60}});
    partition_round producer(initial, std::string("round-17"), partition{});
    std::array batches{
      producer.make_batch("owner-0", 0, {{"c", std::nullopt}, {"new-c", 31}}),
      producer.make_batch("owner-1", 1, {{"a", 11}, {"other-a", std::nullopt}}),
      producer.make_batch("owner-2", 2, {{"new-b", 21}, {"b", 22}})
    };
    auto expected = cola::from_records({{"a", 11}, {"b", 22}, {"new-c", 31},
      {"new-b", 21}, {"", 50}, {std::string("zero\0b", 6), 60}});
    auto save = initial.snapshot();
    std::array order{0, 1, 2};
    std::string canonical_export;
    do {
      partition_round receiver(initial, std::string("round-17"), partition{});
      for (auto index : order) {
        require(receiver.apply(batches[index]) == cola_apply_result::applied,
          "first delivery applies");
        require(receiver.snapshot().owner().entries().back().contribution == batches[index].delta,
          "published run contribution matches independently checked advertised batch delta");
        auto count = receiver.snapshot().live_size();
        auto sig = receiver.snapshot().signature();
        require(receiver.apply(batches[index]) == cola_apply_result::replay,
          "identical retransmit is acknowledged without reapplying");
        require(receiver.snapshot().live_size() == count && receiver.snapshot().signature() == sig,
          "replay does not change live accounting or signature");
      }
      auto result = receiver.snapshot();
      require(result.resolved() == expected.resolved(), "all partition permutations resolve equally");
      require(result.signature() == expected.signature(), "all partition permutations have same signature");
      require(result.signature() == result.recompute_signature(), "incremental signature matches contents");
      require(result.signature() == result.owner().recompute_signature(),
        "cola signature is the sum of its owned entry contributions");
      require(result.live_size() == expected.live_size(), "live count tracks inserts and genuine deletes");
      require(result.pins().size() == 4, "each update pins one new run");
      require(result.pins().front() == initial.pins().front(), "unchanged base run is shared");
      auto compacted = result.compact();
      require(compacted.pins().size() == 1 && compacted.signature() == result.signature(),
        "compaction changes physical pins without changing logical signature");
      require(compacted.resolved() == result.resolved(), "compaction discards shadows and tombstones");
      require(compacted.owner().entries()[0].contribution == result.signature(),
        "compacted entry owns the complete replaced contribution");

      std::ostringstream bytes(std::ios::binary);
      result.export_rc(bytes);
      if (canonical_export.empty()) canonical_export = bytes.str();
      require(bytes.str() == canonical_export, "debug export ignores physical merge history");
      std::istringstream input(bytes.str(), std::ios::binary);
      auto imported = cola::import_rc(input);
      require(imported.resolved() == result.resolved() && imported.signature() == result.signature(),
        "debug import preserves strings, fixed values and logical signature");
    } while (std::next_permutation(order.begin(), order.end()));
    require(save.get("a") == 10 && save.get("c") == 30 && save.get("other-a") == 40,
      "save pins the earlier cola across overwrites and deletions");
    partition_round fork(save, std::string("fork-17"), partition{});
    fork.apply(fork.make_batch("fork-b", 2, {{"b", 999}}));
    require(fork.snapshot().get("b") == 999 && save.get("b") == 20,
      "fork advances independently from saved state");
  }

  void validation() {
    using cola = reference_cola<>;
    auto base = cola::from_records({{"a", 1}, {"b", 2}});
    partition_round round(base, std::string("round-5"), partition{});
    rejects([&] { round.make_batch("bad", 1, {{"absent-a", std::nullopt}}); },
      "absent delete cannot create credits");
    rejects([&] { round.make_batch("bad", 2, {{"a", 3}}); }, "partition assignment checked by builder");
    rejects([&] { round.make_batch("bad", 1, {{"a", 3}, {"a", 4}}); }, "duplicate write rejected");
    rejects([&] { cola::from_records({{"a", 1}, {"a", 2}}); }, "duplicate initial key rejected");
    rejects([&] { cola::from_records({{"a", std::nullopt}}); }, "initial absent delete rejected");

    auto valid = round.make_batch("one", 1, {{"a", 3}});
    auto expected = cola::from_records({{"a", 3}, {"b", 2}});
    require(valid.delta == wrapping_fingerprint_algebra::subtract(expected.signature(), base.signature()),
      "builder's advertised contribution equals the full table fingerprint change");
    auto bad = valid;
    bad.edits[0].before = 7;
    rejects([&] { round.apply(bad); }, "old value checked independently on receipt");
    bad = valid;
    bad.owner = 2;
    rejects([&] { round.apply(bad); }, "partition ownership checked independently on receipt");
    bad = valid;
    bad.round_id = "another-round";
    rejects([&] { round.apply(bad); }, "wrong round rejected");
    bad = valid;
    bad.base_signature ^= 1;
    rejects([&] { round.apply(bad); }, "wrong base signature rejected");
    bad = valid;
    bad.delta ^= 1;
    rejects([&] { round.apply(bad); }, "tampered advertised contribution rejected before publication");
    bad = valid;
    bad.edits[0] = {"absent-a", std::nullopt, std::nullopt};
    rejects([&] { round.apply(bad); }, "receiver rejects fabricated absent deletion");
    require(round.accepted_batches() == 0 && round.snapshot().get("a") == 1,
      "rejection has no partially applied state");

    round.apply(valid);
    bad = valid;
    bad.delta ^= 1;
    rejects([&] { round.apply(bad); }, "replay with a changed contribution is not accepted");
    require(round.apply(valid) == cola_apply_result::replay,
      "original advertised contribution remains replay-safe after rejected tampering");
    bad = valid;
    bad.edits[0].after = 4;
    rejects([&] { round.apply(bad); }, "reusing batch identity for different content rejected");
    bad.batch_id = "two";
    rejects([&] { round.apply(bad); }, "two batches cannot claim one key");
    require(round.snapshot().get("a") == 3 && round.accepted_batches() == 1,
      "failed overlap preserves accepted batch and state");

    // A valid prefix before an invalid second edit must not partially commit.
    auto mixed = round.make_batch("mixed", 2, {{"b", 6}, {"new-b", 9}});
    mixed.edits[1].before = 123;
    rejects([&] { round.apply(mixed); }, "all edit preconditions checked before publish");
    require(round.snapshot().get("b") == 2 && !round.snapshot().get("new-b"),
      "invalid suffix cannot publish valid prefix");

    // Same-owner batches may still commute if they touch separate keys.
    auto disjoint = round.make_batch("second-a", 1, {{"other-a", 42}});
    round.apply(disjoint);
    require(round.snapshot().get("other-a") == 42, "partition is not restricted to one batch");
    auto empty = round.make_batch("empty", 0, {});
    require(empty.delta == 0, "empty changeset advertises zero contribution");
    auto pin_count = round.snapshot().pins().size();
    auto signature = round.snapshot().signature();
    round.apply(empty);
    require(round.snapshot().pins().size() == pin_count && round.snapshot().signature() == signature,
      "empty changeset needs no new run or signature change");
    require(round.apply(empty) == cola_apply_result::replay, "empty changeset replays without applying twice");
  }

  void pin_lifetime() {
    using cola = reference_cola<>;
    std::weak_ptr<cola::run const> old_run;
    std::optional<cola> save;
    std::optional<cola> current;
    {
      auto base = cola::from_records({{"a", 1}});
      old_run = base.pins()[0];
      save = base.snapshot();
      partition_round round(base, std::string("pin-round"), partition{});
      round.apply(round.make_batch("delete", 1, {{"a", std::nullopt}}));
      auto changed = round.snapshot();
      auto const & deletion = changed.owner().entries().back();
      require(deletion.native_signature == 0 && deletion.contribution ==
        wrapping_fingerprint_algebra::subtract(0, base.signature()),
        "delete run owns negative old binding despite its zero native hash");
      require(changed.owner().signature() == 0 && changed.owner().recompute_signature() == 0,
        "base and deletion contributions cancel in actual current pin owner");
      current = round.snapshot().compact();
    }
    require(!old_run.expired(), "saved snapshot pins old data after active compaction");
    require(current->live_size() == 0 && current->pins().empty(), "empty compaction drops active old runs");
    save.reset();
    require(old_run.expired(), "old run reclaimed after its last save pin goes away");
  }

  void rc_dumps() {
    using cola = reference_cola<>;
    auto base = cola::from_records({{"", 0}, {std::string("a\0b", 3), 255}, {"z", UINT64_MAX}});
    std::ostringstream output(std::ios::binary);
    base.export_rc(output);
    auto bytes = output.str();
    require(bytes.size() >= 32 && std::string_view(bytes.data(), 8) == std::string_view{"DIET.RC\0", 8},
      "Diet debug dump signature differs from golden bytes");
    for (std::size_t size = 0; size < bytes.size(); ++size) {
      std::istringstream in(bytes.substr(0, size), std::ios::binary);
      rejects([&] { cola::import_rc(in); }, "truncated debug export rejected at every byte boundary");
    }
    auto invalid = bytes;
    invalid[0] = 'X';
    std::istringstream bad_magic(invalid, std::ios::binary);
    rejects([&] { cola::import_rc(bad_magic); }, "wrong file magic rejected");
    invalid = bytes;
    invalid[0] ^= 0x01; invalid[1] ^= 0x1f; invalid[2] ^= 0x17;
    std::istringstream incompatible_magic(invalid, std::ios::binary);
    rejects([&] { cola::import_rc(incompatible_magic); }, "incompatible debug export identifier rejected");
    invalid = bytes;
    invalid[7] = 1;
    std::istringstream bad_terminator(invalid, std::ios::binary);
    rejects([&] { cola::import_rc(bad_terminator); }, "signature terminating zero is required");
    for (auto offset : {8, 15}) {
      invalid = bytes;
      invalid[std::size_t(offset)] = 2;
      std::istringstream bad_version(invalid, std::ios::binary);
      rejects([&] { cola::import_rc(bad_version); }, "unsupported debug export format version rejected");
    }
    invalid = bytes;
    invalid[16] = 2;
    std::istringstream bad_codec(invalid, std::ios::binary);
    rejects([&] { cola::import_rc(bad_codec); }, "wrong fixed-value codec rejected");
    std::istringstream record_limit(bytes, std::ios::binary);
    rejects([&] { cola::import_rc(record_limit, u64_cola_codec{}, u64_table_hash{}, {2, 100}); },
      "untrusted record allocation bounded");
    std::istringstream byte_limit(bytes, std::ios::binary);
    rejects([&] { cola::import_rc(byte_limit, u64_cola_codec{}, u64_table_hash{}, {10, 2}); },
      "untrusted key allocation bounded");

    // Exercise actual file streams in a unique temporary directory. This does
    // not claim fsync/crash publication or catalog-root persistence.
    auto directory = std::filesystem::temp_directory_path() /
      ("diet-cola-" + std::to_string(reinterpret_cast<std::uintptr_t>(&base)));
    require(std::filesystem::create_directory(directory), "create unique test debug dump directory");
    struct cleanup {
      std::filesystem::path path;
      ~cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } guard{directory};
    {
      std::ofstream file(directory / "dump.rc", std::ios::binary);
      base.export_rc(file);
    }
    std::ifstream file(directory / "dump.rc", std::ios::binary);
    auto imported = cola::import_rc(file);
    require(imported.resolved() == base.resolved(), "debug dump round trip preserves embedded NUL and extreme values");
    require(imported.signature() == base.signature(), "debug import recomputes signature");

    auto empty = cola{};
    std::ostringstream empty_bytes;
    empty.export_rc(empty_bytes);
    constexpr std::array<unsigned char, 32> empty_golden{
      'D', 'I', 'E', 'T', '.', 'R', 'C', 0, // eight-byte signature
      1, 0, 0, 0, 0, 0, 0, 0, // debug dump format version
      1, 0, 0, 0, 0, 0, 0, 0, // value codec tag
      0, 0, 0, 0, 0, 0, 0, 0}; // record count
    auto encoded_empty = empty_bytes.str();
    require(encoded_empty.size() == empty_golden.size() &&
      std::equal(empty_golden.begin(), empty_golden.end(), encoded_empty.begin()),
      "empty debug export golden bytes changed");
    std::istringstream empty_input(encoded_empty);
    auto empty_imported = cola::import_rc(empty_input);
    require(empty_imported.live_size() == 0 && empty_imported.signature() == 0 &&
      empty_imported.pins().empty(), "empty debug dump round trip");
  }
}

int main() {
  try {
    commuting_partitions<wrapping_fingerprint_algebra>();
    commuting_partitions<binary_field>();
    validation();
    pin_lifetime();
    rc_dumps();
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
