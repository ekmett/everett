/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Injects real forwarded SQLite write/sync errors around retained catalog roots.
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/sqlite_catalog.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
namespace {
  using namespace diet;
  using policy = storage_policy<profile_unit::byte, variable_values, 3>;
  using catalog = sqlite_catalog<policy>;

  void require(bool value, char const * message) {
    if (!value) throw std::runtime_error(message);
  }
  struct event {
    enum class kind { write, sync } operation;
    int file_flags;
    int argument;
    sqlite3_int64 offset;
    bool forwarded = false;
    int underlying_result = SQLITE_OK;
  };
  struct injection {
    // Callbacks cannot allocate or throw across SQLite's C ABI.
    std::array<event, 1024> events{};
    std::size_t count = 0, fail_at = std::numeric_limits<std::size_t>::max();
    bool enabled = false, after = false, triggered = false, overflow = false;
    void start(std::optional<std::size_t> target = {}, bool after_call = false) noexcept {
      count = 0; fail_at = target.value_or(std::numeric_limits<std::size_t>::max());
      enabled = true; after = after_call; triggered = false; overflow = false;
    }
    void stop() noexcept { enabled = false; }
    event * record(event value) noexcept {
      if (!enabled) return nullptr;
      if (count == events.size()) { overflow = true; return nullptr; }
      events[count] = value; return &events[count++];
    }
    bool selected(event const * value) const noexcept {
      return value && !triggered && std::size_t(value - events.data()) == fail_at;
    }
  };

  // A forwarding shim, not an alternative filesystem. All reads, locking,
  // WAL shared-memory methods, truncation and name operations stay with the
  // original VFS. Only one chosen xWrite/xSync returns an injected IOERR, either
  // without invoking its delegate or after the delegate acknowledged success.
  // Contracts: https://sqlite.org/c3ref/vfs.html and
  // https://sqlite.org/c3ref/io_methods.html; shims: https://sqlite.org/vfs.html.
  struct forwarding_vfs {
    struct file_state {
      sqlite3_file base{};
      sqlite3_io_methods methods{};
      sqlite3_file * real = nullptr;
      forwarding_vfs * owner = nullptr;
      int flags = 0;
    };
    static_assert(alignof(file_state) <= 8); // SQLite's allocation guarantee.
    sqlite3_vfs wrapper{};
    sqlite3_vfs * delegate = nullptr;
    injection fault;
    std::size_t live_files = 0;

    forwarding_vfs() {
      delegate = sqlite3_vfs_find(nullptr);
      require(delegate && delegate->szOsFile > 0, "missing default SQLite VFS");
      wrapper.iVersion = std::min(delegate->iVersion, 3);
      wrapper.mxPathname = delegate->mxPathname;
      wrapper.szOsFile = sizeof(file_state);
      wrapper.zName = "diet-forwarding-fault-test";
      wrapper.pAppData = this;
      wrapper.pNext = nullptr;
      wrapper.xOpen = open;
      wrapper.xDelete = remove;
      wrapper.xAccess = access;
      wrapper.xFullPathname = full_path;
      if (delegate->xDlOpen) wrapper.xDlOpen = dl_open;
      if (delegate->xDlError) wrapper.xDlError = dl_error;
      if (delegate->xDlSym) wrapper.xDlSym = dl_sym;
      if (delegate->xDlClose) wrapper.xDlClose = dl_close;
      wrapper.xRandomness = randomness;
      wrapper.xSleep = sleep;
      wrapper.xCurrentTime = current_time;
      if (delegate->xGetLastError) wrapper.xGetLastError = last_error;
      if (delegate->iVersion >= 2 && delegate->xCurrentTimeInt64) wrapper.xCurrentTimeInt64 = current_time64;
      if (delegate->iVersion >= 3) {
        if (delegate->xSetSystemCall) wrapper.xSetSystemCall = set_system_call;
        if (delegate->xGetSystemCall) wrapper.xGetSystemCall = get_system_call;
        if (delegate->xNextSystemCall) wrapper.xNextSystemCall = next_system_call;
      }
      require(sqlite3_vfs_register(&wrapper, 1) == SQLITE_OK, "register forwarding VFS");
    }
    forwarding_vfs(forwarding_vfs const &) = delete;
    forwarding_vfs & operator=(forwarding_vfs const &) = delete;
    ~forwarding_vfs() {
      assert(live_files == 0);
      sqlite3_vfs_unregister(&wrapper);
      sqlite3_vfs_register(delegate, 1);
    }
    static forwarding_vfs & self(sqlite3_vfs * vfs) noexcept {
      return *static_cast<forwarding_vfs *>(vfs->pAppData);
    }
    static file_state & file(sqlite3_file * handle) noexcept {
      return *reinterpret_cast<file_state *>(handle);
    }
    static int open(sqlite3_vfs * vfs, sqlite3_filename name, sqlite3_file * handle,
                    int flags, int * output) noexcept {
      auto & owner = self(vfs);
      auto & state = *std::construct_at(reinterpret_cast<file_state *>(handle));
      state.owner = &owner; state.flags = flags;
      state.real = static_cast<sqlite3_file *>(sqlite3_malloc64(sqlite3_uint64(owner.delegate->szOsFile)));
      if (!state.real) return SQLITE_NOMEM;
      std::memset(state.real, 0, std::size_t(owner.delegate->szOsFile));
      int result = owner.delegate->xOpen(owner.delegate, name, state.real, flags, output);
      if (!state.real->pMethods) {
        sqlite3_free(state.real); state.real = nullptr;
        return result;
      }
      ++owner.live_files;
      // A failed delegated xOpen can still require xClose: retain its methods.
      state.methods.iVersion = std::min(state.real->pMethods->iVersion, 3);
      state.methods.xClose = close;
      state.methods.xRead = read;
      state.methods.xWrite = write;
      state.methods.xTruncate = truncate;
      state.methods.xSync = sync;
      state.methods.xFileSize = size;
      state.methods.xLock = lock;
      state.methods.xUnlock = unlock;
      state.methods.xCheckReservedLock = reserved;
      state.methods.xFileControl = control;
      state.methods.xSectorSize = sector;
      state.methods.xDeviceCharacteristics = characteristics;
      if (state.methods.iVersion >= 2) {
        if (state.real->pMethods->xShmMap) state.methods.xShmMap = shm_map;
        if (state.real->pMethods->xShmLock) state.methods.xShmLock = shm_lock;
        if (state.real->pMethods->xShmBarrier) state.methods.xShmBarrier = shm_barrier;
        if (state.real->pMethods->xShmUnmap) state.methods.xShmUnmap = shm_unmap;
      }
      if (state.methods.iVersion >= 3) {
        if (state.real->pMethods->xFetch) state.methods.xFetch = fetch;
        if (state.real->pMethods->xUnfetch) state.methods.xUnfetch = unfetch;
      }
      state.base.pMethods = &state.methods;
      return result;
    }
    static int close(sqlite3_file * handle) noexcept {
      auto & state = file(handle);
      int result = state.real->pMethods->xClose(state.real);
      sqlite3_free(state.real); state.real = nullptr; state.base.pMethods = nullptr;
      --state.owner->live_files;
      return result;
    }
    static int read(sqlite3_file * handle, void * output, int size, sqlite3_int64 offset) noexcept {
      auto real = file(handle).real; return real->pMethods->xRead(real, output, size, offset);
    }
    static int write(sqlite3_file * handle, void const * input, int size, sqlite3_int64 offset) noexcept {
      auto & state = file(handle); auto & fault = state.owner->fault;
      auto recorded = fault.record({event::kind::write, state.flags, size, offset});
      bool selected = fault.selected(recorded);
      if (selected && !fault.after) { fault.triggered = true; return SQLITE_IOERR_WRITE; }
      int result = state.real->pMethods->xWrite(state.real, input, size, offset);
      if (recorded) { recorded->forwarded = true; recorded->underlying_result = result; }
      if (selected && result == SQLITE_OK) { fault.triggered = true; return SQLITE_IOERR_WRITE; }
      return result;
    }
    static int sync(sqlite3_file * handle, int flags) noexcept {
      auto & state = file(handle); auto & fault = state.owner->fault;
      auto recorded = fault.record({event::kind::sync, state.flags, flags, 0});
      bool selected = fault.selected(recorded);
      if (selected && !fault.after) { fault.triggered = true; return SQLITE_IOERR_FSYNC; }
      int result = state.real->pMethods->xSync(state.real, flags);
      if (recorded) { recorded->forwarded = true; recorded->underlying_result = result; }
      if (selected && result == SQLITE_OK) { fault.triggered = true; return SQLITE_IOERR_FSYNC; }
      return result;
    }
    static int truncate(sqlite3_file * handle, sqlite3_int64 size) noexcept {
      auto real = file(handle).real; return real->pMethods->xTruncate(real, size);
    }
    static int size(sqlite3_file * handle, sqlite3_int64 * output) noexcept {
      auto real = file(handle).real; return real->pMethods->xFileSize(real, output);
    }
    static int lock(sqlite3_file * handle, int value) noexcept {
      auto real = file(handle).real; return real->pMethods->xLock(real, value);
    }
    static int unlock(sqlite3_file * handle, int value) noexcept {
      auto real = file(handle).real; return real->pMethods->xUnlock(real, value);
    }
    static int reserved(sqlite3_file * handle, int * output) noexcept {
      auto real = file(handle).real; return real->pMethods->xCheckReservedLock(real, output);
    }
    static int control(sqlite3_file * handle, int op, void * argument) noexcept {
      auto real = file(handle).real; return real->pMethods->xFileControl(real, op, argument);
    }
    static int sector(sqlite3_file * handle) noexcept {
      auto real = file(handle).real; return real->pMethods->xSectorSize(real);
    }
    static int characteristics(sqlite3_file * handle) noexcept {
      auto real = file(handle).real; return real->pMethods->xDeviceCharacteristics(real);
    }
    static int shm_map(sqlite3_file * handle, int page, int size, int extend, void volatile ** output) noexcept {
      auto real = file(handle).real; return real->pMethods->xShmMap(real, page, size, extend, output);
    }
    static int shm_lock(sqlite3_file * handle, int offset, int count, int flags) noexcept {
      auto real = file(handle).real; return real->pMethods->xShmLock(real, offset, count, flags);
    }
    static void shm_barrier(sqlite3_file * handle) noexcept {
      auto real = file(handle).real; real->pMethods->xShmBarrier(real);
    }
    static int shm_unmap(sqlite3_file * handle, int remove) noexcept {
      auto real = file(handle).real; return real->pMethods->xShmUnmap(real, remove);
    }
    static int fetch(sqlite3_file * handle, sqlite3_int64 offset, int size, void ** output) noexcept {
      auto real = file(handle).real; return real->pMethods->xFetch(real, offset, size, output);
    }
    static int unfetch(sqlite3_file * handle, sqlite3_int64 offset, void * value) noexcept {
      auto real = file(handle).real; return real->pMethods->xUnfetch(real, offset, value);
    }
    static int remove(sqlite3_vfs * vfs, char const * name, int sync) noexcept {
      auto p = self(vfs).delegate; return p->xDelete(p, name, sync);
    }
    static int access(sqlite3_vfs * vfs, char const * name, int flags, int * output) noexcept {
      auto p = self(vfs).delegate; return p->xAccess(p, name, flags, output);
    }
    static int full_path(sqlite3_vfs * vfs, char const * name, int size, char * output) noexcept {
      auto p = self(vfs).delegate; return p->xFullPathname(p, name, size, output);
    }
    static void * dl_open(sqlite3_vfs * vfs, char const * name) noexcept {
      auto p = self(vfs).delegate; return p->xDlOpen(p, name);
    }
    static void dl_error(sqlite3_vfs * vfs, int size, char * output) noexcept {
      auto p = self(vfs).delegate; p->xDlError(p, size, output);
    }
    static void (*dl_sym(sqlite3_vfs * vfs, void * handle, char const * symbol) noexcept)(void) {
      auto p = self(vfs).delegate; return p->xDlSym(p, handle, symbol);
    }
    static void dl_close(sqlite3_vfs * vfs, void * handle) noexcept {
      auto p = self(vfs).delegate; p->xDlClose(p, handle);
    }
    static int randomness(sqlite3_vfs * vfs, int size, char * output) noexcept {
      auto p = self(vfs).delegate; return p->xRandomness(p, size, output);
    }
    static int sleep(sqlite3_vfs * vfs, int duration) noexcept {
      auto p = self(vfs).delegate; return p->xSleep(p, duration);
    }
    static int current_time(sqlite3_vfs * vfs, double * output) noexcept {
      auto p = self(vfs).delegate; return p->xCurrentTime(p, output);
    }
    static int last_error(sqlite3_vfs * vfs, int size, char * output) noexcept {
      auto p = self(vfs).delegate; return p->xGetLastError(p, size, output);
    }
    static int current_time64(sqlite3_vfs * vfs, sqlite3_int64 * output) noexcept {
      auto p = self(vfs).delegate; return p->xCurrentTimeInt64(p, output);
    }
    static int set_system_call(sqlite3_vfs * vfs, char const * name, sqlite3_syscall_ptr value) noexcept {
      auto p = self(vfs).delegate; return p->xSetSystemCall(p, name, value);
    }
    static sqlite3_syscall_ptr get_system_call(sqlite3_vfs * vfs, char const * name) noexcept {
      auto p = self(vfs).delegate; return p->xGetSystemCall(p, name);
    }
    static char const * next_system_call(sqlite3_vfs * vfs, char const * name) noexcept {
      auto p = self(vfs).delegate; return p->xNextSystemCall(p, name);
    }
  };

  object_id id(unsigned number) {
    char text[33]; std::snprintf(text, sizeof text, "%032x", number); return object_id(text);
  }
  object_attempt_id attempt(unsigned number) { return object_attempt_id(id(number).hex()); }
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto path = (std::filesystem::temp_directory_path() / "diet-vfs-XXXXXX").string();
      auto result = ::mkdtemp(path.data());
      if (!result) throw std::runtime_error("create temporary VFS directory");
      root = result;
    }
    ~temporary() { std::error_code error; std::filesystem::remove_all(root, error); }
  };
  struct sql_reader {
    sqlite3 * db = nullptr;
    explicit sql_reader(std::filesystem::path const & root) {
      auto result = sqlite3_open_v2((root / "catalog.sqlite3").c_str(), &db,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
      if (result != SQLITE_OK) {
        if (db) sqlite3_close(db);
        db = nullptr; throw std::runtime_error("open independent catalog reader");
      }
    }
    ~sql_reader() { if (db) sqlite3_close(db); }
    sql_reader(sql_reader const &) = delete;
    sql_reader & operator=(sql_reader const &) = delete;
    std::int64_t count(char const * sql) const {
      sqlite3_stmt * statement = nullptr;
      require(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK, "prepare independent catalog count");
      int result = sqlite3_step(statement);
      auto value = result == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
      sqlite3_finalize(statement);
      require(result == SQLITE_ROW, "read independent catalog count"); return value;
    }
    void integrity() const {
      sqlite3_stmt * statement = nullptr;
      require(sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &statement, nullptr) == SQLITE_OK, "prepare integrity check");
      int result = sqlite3_step(statement);
      auto text = result == SQLITE_ROW ? sqlite3_column_text(statement, 0) : nullptr;
      bool okay = text && std::strcmp(reinterpret_cast<char const *>(text), "ok") == 0;
      if (okay) okay = sqlite3_step(statement) == SQLITE_DONE;
      sqlite3_finalize(statement); require(okay, "catalog failed integrity check");
      require(count("SELECT count(*) FROM pragma_foreign_key_check") == 0, "catalog foreign key violation");
    }
  };
  struct fixture {
    temporary directory;
    std::optional<catalog> metadata;
    blob_identity head{id(2), id(3)};
    fixture() {
      metadata.emplace(catalog::create(directory.root, id(1)));
      std::array outputs{catalog_object_reservation{head.native, file_kind::native_blob},
                         catalog_object_reservation{head.index, file_kind::fractional_index}};
      metadata->reserve("baseline-reserve", attempt(4), "baseline-build", {}, outputs);
      std::array records{profile_record{bit_string::from_bytes("key"), bit_string::from_bytes("value")}};
      auto pair = profile_blob<policy>::build(records);
      auto native = encode_native_sections(pair.native());
      auto index = encode_index_sections(pair, head.native);
      metadata->record_sealed("baseline-native", native.seal(directory.root, head.native, attempt(4)));
      metadata->record_sealed("baseline-index", index.seal(directory.root, head.index, attempt(4)));
      auto query = open_mapped_query<policy>(directory.root, head);
      metadata->register_chain("baseline-register", query, catalog_admission::scan);
      metadata->save("baseline-save", "baseline", head);
      metadata->acquire_save("baseline-reader", "baseline", "baseline-reader");
    }
  };
  enum class operation { reserve, save, acquire };
  char const * name(operation value) {
    switch (value) {
      case operation::reserve: return "fault-reserve";
      case operation::save: return "fault-save";
      case operation::acquire: return "fault-acquire";
    }
    throw std::logic_error("unknown VFS operation");
  }
  void apply(fixture & value, operation selected) {
    switch (selected) {
      case operation::reserve: {
        std::array outputs{catalog_object_reservation{id(20), file_kind::native_blob}};
        std::array inputs{value.head};
        value.metadata->reserve(name(selected), attempt(21), "fault-owner", inputs, outputs); break;
      }
      case operation::save: value.metadata->save(name(selected), "candidate", value.head); break;
      case operation::acquire: value.metadata->acquire_save(name(selected), "baseline", "fault-reader"); break;
    }
  }
  void verify(fixture const & value, operation selected, catalog_operation const & expected) {
    auto reopened = catalog::open(value.directory.root);
    require(reopened.find_save("baseline") == value.head, "lost baseline save");
    auto observed = reopened.lookup_operation(name(selected));
    bool committed = bool(observed);
    if (committed)
      require(observed->kind == expected.kind && observed->request == expected.request && observed->outcome == expected.outcome,
              "visible operation disagrees with its exact request/outcome");
    sql_reader sql(value.directory.root);
    sql.integrity();
    require(sql.count("SELECT count(*) FROM operations") == 6 + committed, "partial operation outcome");
    require(sql.count("SELECT count(*) FROM pairs") == 1, "unexpected pair mutation");
    require(sql.count("SELECT count(*) FROM pairs WHERE native_id='00000000000000000000000000000002' AND index_id='00000000000000000000000000000003' AND target_native IS NULL AND target_index IS NULL AND native_count=1 AND borrowed_count=0 AND virtual_count=1") == 1,
            "lost exact baseline pair");
    require(sql.count("SELECT count(*) FROM objects WHERE bytes IS NOT NULL") == 2, "lost sealed baseline objects");
    require(sql.count("SELECT count(*) FROM owner_objects WHERE owner_kind='attempt' AND owner_id=X'626173656c696e652d6275696c64'") == 2,
            "lost baseline output retention");
    require(sql.count("SELECT count(*) FROM owner_roots WHERE owner_kind='save' AND owner_id=X'626173656c696e65' AND native_id='00000000000000000000000000000002' AND index_id='00000000000000000000000000000003'") == 1,
            "lost baseline save pin");
    require(sql.count("SELECT count(*) FROM owner_roots WHERE owner_kind='reader' AND owner_id=X'626173656c696e652d726561646572' AND native_id='00000000000000000000000000000002' AND index_id='00000000000000000000000000000003'") == 1,
            "lost baseline reader pin");
    require(sql.count("SELECT count(*) FROM owner_roots") == 2 + committed, "partial root publication");
    require(sql.count("SELECT count(*) FROM owners") == 3 + committed, "partial owner publication");
    bool reserved = selected == operation::reserve && committed;
    require(sql.count("SELECT count(*) FROM attempts") == 1 + reserved, "partial attempt reservation");
    require(sql.count("SELECT count(*) FROM objects") == 2 + reserved, "partial object reservation");
    require(sql.count("SELECT count(*) FROM owner_objects") == 2 + reserved, "partial output retention");
    require(sql.count("SELECT count(*) FROM saves") == 1 + (selected == operation::save && committed), "partial save publication");
    auto query = open_mapped_query<policy>(value.directory.root, value.head);
    auto key = bit_string::from_bytes("key");
    auto cursor = query.cursor(key.view());
    cursor.step(1); require(cursor.has_match(), "baseline query no longer finds its key");
    auto match = cursor.take_match();
    auto wanted = bit_string::from_bytes("value");
    require(compare_bits(match.value.view(), wanted.view()) == 0, "baseline query value changed");
    require(cursor.done(), "unexpected baseline query suffix");
  }
  struct trace { std::vector<event> events; catalog_operation result; };
  trace probe(forwarding_vfs & vfs, operation selected) {
    fixture value;
    vfs.fault.start(); apply(value, selected); vfs.fault.stop();
    require(!vfs.fault.overflow && vfs.fault.count != 0, "transaction has no bounded VFS trace");
    auto result = value.metadata->lookup_operation(name(selected));
    require(bool(result), "successful trace has no operation outcome");
    trace found{{vfs.fault.events.begin(), vfs.fault.events.begin() + vfs.fault.count}, *result};
    bool writes = false, syncs = false;
    for (auto const & current : found.events) {
      require(current.forwarded && current.underlying_result == SQLITE_OK, "fault-free delegate failed");
      writes = writes || current.operation == event::kind::write;
      syncs = syncs || current.operation == event::kind::sync;
    }
    require(writes && syncs, "transaction did not exercise both real write and sync");
    verify(value, selected, found.result); return found;
  }
  void failures(forwarding_vfs & vfs, operation selected, trace const & trace) {
    for (std::size_t at = 0; at != trace.events.size(); ++at) {
      for (bool after : {false, true}) {
        fixture value;
        vfs.fault.start(at, after);
        bool failed = false;
        try { apply(value, selected); }
        catch (catalog_error const & error) {
          failed = true;
          auto expected = trace.events[at].operation == event::kind::write ? SQLITE_IOERR_WRITE : SQLITE_IOERR_FSYNC;
          require(error.code == expected && error.operation == name(selected) && error.outcome_unknown,
                  "VFS I/O error lost its catalog uncertainty or identity");
        }
        vfs.fault.stop();
        require(failed && vfs.fault.triggered && !vfs.fault.overflow, "selected VFS failure was not observed");
        auto const & injected = vfs.fault.events[at];
        require(injected.operation == trace.events[at].operation && injected.file_flags == trace.events[at].file_flags &&
                injected.argument == trace.events[at].argument && injected.offset == trace.events[at].offset,
                "transaction prefix differed from its independent trace");
        require(injected.forwarded == after && injected.underlying_result == SQLITE_OK,
                "injection did not occur on the requested side of the real call");
        require(value.metadata->poisoned(), "I/O failure did not poison catalog handle");
        bool blocked = false;
        try { value.metadata->save("after-error", "forbidden", value.head); }
        catch (std::logic_error const &) { blocked = true; }
        require(blocked, "poisoned handle accepted another mutation");
        // Observe with the failed connection alive, then again after normal
        // close/reopen. Each observation may see the old or complete new txn;
        // neither is claimed to establish survival after an actual power loss.
        verify(value, selected, trace.result);
        value.metadata.reset();
        verify(value, selected, trace.result);
      }
    }
  }
}
#endif

int main() {
#if defined(__APPLE__) || defined(__linux__)
  forwarding_vfs vfs;
  std::size_t cases = 0;
  for (auto op : {operation::reserve, operation::save, operation::acquire}) {
    auto observed = probe(vfs, op);
    failures(vfs, op, observed); cases += 2 * observed.events.size();
    auto writes = std::count_if(observed.events.begin(), observed.events.end(),
      [](event const & value) { return value.operation == event::kind::write; });
    std::cout << name(op) << ": " << writes << " writes, " << observed.events.size() - std::size_t(writes)
              << " syncs\n";
  }
  std::cout << "SQLite " << catalog::runtime_version() << ", " << vfs.delegate->zName << ": "
            << cases << " injected I/O failures; no power-loss simulation\n";
#else
  std::cout << "SQLite VFS catalog fixture requires POSIX object sealing\n";
#endif
}
