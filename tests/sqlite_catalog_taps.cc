/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks atomic named-tap roots, checkpoints, replay and uncertain commits.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/sqlite_catalog.h>

#include <array>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

#include <unistd.h>

namespace {
  using namespace diet;
  using P = storage_policy<tip<encoded_sort<bit_encoding<>>>>;
  using catalog = sqlite_catalog<P>;
  void require(bool value, char const * message) { if (!value) throw std::runtime_error(message); }
  template <class F> void rejects(F && action) {
    try { action(); } catch (std::exception const &) { return; }
    throw std::runtime_error("expected rejection");
  }
  object_id id(unsigned n) {
    char text[33]; std::snprintf(text, sizeof text, "%032x", n); return object_id(text);
  }
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto name = (std::filesystem::temp_directory_path() / "diet-taps-XXXXXX").string();
      if (!::mkdtemp(name.data())) throw std::runtime_error("mkdtemp");
      root = name;
    }
    ~temporary() { std::error_code error; std::filesystem::remove_all(root, error); }
  };
  blob_identity persist(catalog & db, unsigned seed) {
    auto native = std::make_shared<profile_array<P> const>(profile_array<P>::build(std::array{
      profile_record{bit_string::from_bits("01"), bit_string::from_bytes(std::to_string(seed))}}));
    auto index = cola_index<P>::adopt_native(native);
    blob_identity pair{id(seed), id(seed + 1)};
    object_attempt_id attempt(id(seed + 2).hex());
    std::array outputs{catalog_object_reservation{pair.native, file_kind::native_blob},
      catalog_object_reservation{pair.index, file_kind::fractional_index}};
    auto op = std::to_string(seed);
    db.reserve("reserve-" + op, attempt, "builder-" + op, {}, outputs);
    db.record_sealed("native-" + op, encode_native_sections(*native).seal(db.root(), pair.native, attempt));
    db.record_sealed("index-" + op, encode_cola_sections(index, pair.native).seal(db.root(), pair.index, attempt));
    db.register_chain("register-" + op, open_mapped_cola_query<P>(db.root(), pair), catalog_admission::scan);
    return pair;
  }
  std::vector<std::byte> checkpoint(unsigned n) {
    return {std::byte{0}, std::byte(n), std::byte{0xff}, std::byte{0}};
  }
  struct commit_control { bool fail = false, committed = false; };
  struct commit_ops {
    std::shared_ptr<commit_control> state;
    int commit(sqlite3 * db) noexcept {
      if (!state->fail) return sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
      if (state->committed) {
        auto result = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
        if (result != SQLITE_OK) return result;
      }
      return SQLITE_IOERR_FSYNC;
    }
  };
  void lifecycle() {
    temporary dir;
    auto db = catalog::create_taps(dir.root, id(1));
    require(db.schema_version() == 4, "tap catalog version");
    auto a = persist(db, 100), b = persist(db, 200);
    require(!db.find_tap("absent"), "unknown tap found");
    std::string name("earth-616\0\xff", 11);
    auto first = db.create_tap("create", name, a, checkpoint(1));
    require(first.timeline.generation == 0 && first.checkpoint == checkpoint(1), "creation contents");
    require(db.create_tap("create", name, a, checkpoint(1)) == first, "creation replay");
    rejects([&] { db.create_tap("create", name, a, checkpoint(2)); });
    rejects([&] { db.create_tap("duplicate", name, a, {}); });
    auto next = db.publish_tap("publish", first, b, checkpoint(2));
    require(next.published && next.head.timeline.generation == 1 && next.head.timeline.head == b &&
      next.head.checkpoint == checkpoint(2), "root and checkpoint publication");
    auto stale = db.publish_tap("stale", first, a, checkpoint(3));
    require(!stale.published && stale.head == next.head, "stale comparison");
    auto forged = next.head; forged.checkpoint = checkpoint(99);
    require(!db.publish_tap("forged-checkpoint", forged, a, checkpoint(3)).published, "CAS omitted checkpoint");
    rejects([&] { db.publish_timeline("plain-write", next.head.timeline, a); });
    require(!db.lookup_operation("plain-write"), "plain-write failure committed");
    auto third = db.publish_tap("same-root", next.head, b, {});
    require(third.published && third.head.checkpoint.empty(), "empty checkpoint is not absence");
    auto fourth = db.publish_tap("aba", third.head, a, checkpoint(1));
    require(fourth.published && !db.publish_tap("stale-aba", first, b, {}).published, "ABA admitted");
    require(db.publish_tap("publish", first, b, checkpoint(2)) == next, "successful historical replay");
    require(db.publish_tap("stale", first, a, checkpoint(3)) == stale, "conflict replay followed latest head");
    auto fork = db.fork_tap("fork", "earth-617", next.head);
    require(fork.timeline.generation == 0 && fork.timeline.head == b && fork.checkpoint == checkpoint(2), "historical fork");
    require(db.fork_tap("fork", "earth-617", next.head) == fork, "fork replay");
    rejects([&] { db.fork_tap("forged-fork", "earth-618", forged); });
    db.save_tap("save-second", "second", next.head);
    db.save_tap("save-second", "second", next.head);
    require(db.find_saved_tap("second") == next.head && db.find_save("second") == b,
      "snapshot save lost root or checkpoint");
    rejects([&] { db.save_tap("save-second", "second", first); });
    require(!db.find_saved_tap("absent"), "absent saved tap found");
    auto reopened = catalog::open(dir.root);
    require(reopened.find_tap(name) == fourth.head && reopened.find_tap("earth-617") == fork, "reopened checkpoint");
    auto competing = reopened.publish_tap("competitor", fourth.head, b, checkpoint(5));
    require(competing.published && !db.publish_tap("lost-race", fourth.head, a, checkpoint(6)).published,
      "independent connection CAS");
    require(reopened.find_saved_tap("second") == next.head, "saved tap changed with live tap");
    auto ordinary = db.create_timeline("ordinary", "timeline", a);
    rejects([&] { db.find_tap("timeline"); });
    require(db.publish_timeline("ordinary-next", ordinary, b).published, "version4 ordinary timelines");
  }
  void commit_failure(bool committed) {
    temporary dir;
    auto db = catalog::create_taps(dir.root, id(1));
    auto a = persist(db, 100), b = persist(db, 200);
    auto first = db.create_tap("create", "earth-616", a, checkpoint(1), {{b}, {b.native}});
    auto state = std::make_shared<commit_control>();
    {
      auto faulty = sqlite_catalog<P, commit_ops>::open(dir.root, {}, commit_ops{state});
      state->fail = true; state->committed = committed;
      bool uncertain = false;
      try { (void)faulty.publish_tap("uncertain", first, b, checkpoint(2), {{a}, {a.native}}); }
      catch (catalog_error const & error) { uncertain = error.outcome_unknown; }
      require(uncertain && faulty.poisoned(), "commit failure did not poison handle");
      rejects([&] { faulty.find_tap("earth-616"); });
    }
    auto opened = catalog::open(dir.root);
    auto current = opened.find_tap("earth-616");
    require(current.has_value(), "tap lost at commit failure");
    require(current->timeline.head == (committed ? b : a) &&
      current->checkpoint == checkpoint(committed ? 2 : 1), "root/checkpoint torn by failure");
    require(current->auxiliary == (committed ? catalog_auxiliary_roots{{a}, {a.native}} : first.auxiliary),
      "hidden roots torn by failure");
    auto retry = opened.publish_tap("uncertain", first, b, checkpoint(2), {{a}, {a.native}});
    require(retry.published && retry.head.timeline.generation == 1 && retry.head.checkpoint == checkpoint(2),
      "uncertain publication did not replay exactly");
  }
  void auxiliary_pins() {
    temporary dir;
    auto db = catalog::create_taps(dir.root, id(1));
    auto a = persist(db, 100), b = persist(db, 200), c = persist(db, 300);
    catalog_auxiliary_roots roots{{b, b, a}, {c.native, c.native}};
    auto first = db.create_tap("create-aux", "hidden", a, checkpoint(1), roots);
    require(first.auxiliary == catalog_auxiliary_roots{{b}, {c.native}}, "auxiliary roots not canonical");
    require(db.create_tap("create-aux", "hidden", a, checkpoint(1), roots) == first, "auxiliary create replay");
    rejects([&] { db.create_tap("create-aux", "hidden", a, checkpoint(1)); });
    auto wrong = first; wrong.auxiliary.natives.clear();
    require(!db.publish_tap("wrong-aux", wrong, b, {}).published, "CAS omitted auxiliary roots");
    auto second = db.publish_tap("next-aux", first, b, checkpoint(2), {{a}, {c.native}});
    require(second.published && second.head.auxiliary.pairs == std::vector<blob_identity>{a}, "hidden publication pins");
    auto fork = db.fork_tap("fork-aux", "branch", first);
    require(fork.auxiliary == first.auxiliary, "fork lost hidden roots");
    db.save_tap("save-aux", "old", first);
    auto reopened = catalog::open(dir.root);
    require(reopened.find_tap("hidden") == second.head && reopened.find_tap("branch") == fork &&
      reopened.find_saved_tap("old") == first, "reopen lost hidden roots");
    require(reopened.publish_tap("next-aux", first, b, checkpoint(2), {{a}, {c.native}}) == second,
      "publication replay lost hidden roots");
    rejects([&] { db.publish_tap("unsealed-aux", second.head, a, {}, {{}, {id(999)}}); });
    require(db.find_tap("hidden") == second.head, "bad hidden root partially published");
    sqlite3 * raw = nullptr;
    require(sqlite3_open((dir.root / "catalog.sqlite3").c_str(), &raw) == SQLITE_OK, "open auxiliary SQL");
    {
      catalog_detail::statement pairs(raw, "SELECT count(*) FROM owner_roots WHERE owner_kind='save' AND owner_id=?");
      pairs.key(1, "old"); require(pairs.row() && pairs.integer(0) == 2, "save did not pin hidden pair");
      catalog_detail::statement natives(raw, "SELECT count(*) FROM owner_objects WHERE owner_kind='save' AND owner_id=?");
      natives.key(1, "old"); require(natives.row() && natives.integer(0) == 1, "save did not pin hidden native");
    }
    sqlite3_close(raw);
  }
  void schema_checks() {
    temporary dir;
    auto db = catalog::create_cola(dir.root, id(1));
    rejects([&] { db.find_tap("absent"); });
    require(catalog::open(dir.root).schema_version() == 3, "version3 changed on open");
    temporary corrupt;
    { auto initial = catalog::create_taps(corrupt.root, id(2)); }
    sqlite3 * raw = nullptr;
    require(sqlite3_open((corrupt.root / "catalog.sqlite3").c_str(), &raw) == SQLITE_OK, "open raw SQL");
    require(sqlite3_exec(raw, "DROP TRIGGER immutable_tap_checkpoints_UPDATE", nullptr, nullptr, nullptr) == SQLITE_OK,
      "drop checkpoint protection");
    sqlite3_close(raw);
    rejects([&] { (void)catalog::open(corrupt.root); });
  }
}
int main() {
  try { lifecycle(); auxiliary_pins(); commit_failure(false); commit_failure(true); schema_checks(); }
  catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
