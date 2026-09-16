/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks bounded native retention, one-pass spill and durable failure isolation.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/sort_runtime_context.h>

#include <cassert>
#include <iostream>
#include <thread>

namespace {
  using namespace everett;
  using P = string_policy;
  using strings = unsorted<std::optional<std::string>>;
  using family = streaming_sort_runtime_family<P>;
  using storage = family::storage_type;
  using native = family::native_type;
  using pointer = std::shared_ptr<native const>;
  using core = typed_engine<P, wrapping_fingerprint_algebra, 256, family>;
  object_id id(unsigned n = 1) { char text[33]; std::snprintf(text, sizeof text, "%032x", n); return object_id(text); }
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto name = (std::filesystem::temp_directory_path() / "everett-adaptive-native-XXXXXX").string();
      if (!::mkdtemp(name.data())) throw std::runtime_error("mkdtemp");
      root = name;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  template <class F> void rejects(F && fn) {
    try { fn(); } catch (std::exception const &) { return; }
    throw std::runtime_error("expected adaptive rejection");
  }
  pointer row(std::string key, std::string value) {
    auto command = core::put(key, value); return storage::singleton(command.records().front());
  }
  template <class Context, class Compose = replace_native_value>
  pointer merge(std::shared_ptr<Context> const & context, pointer a, pointer b, Compose compose = {}) {
    auto job = context->make_merge(std::move(a), std::move(b), std::move(compose));
    while (!job->done()) job->step(1);
    return context->finish_merge(*job);
  }
  std::vector<std::byte> wire(sort_profile_array<P> const & value) {
    auto encoded = encoded_sort_sections<P>::from(value);
    std::vector<std::byte> result;
    for (auto part : encoded.chunks()) result.insert(result.end(), part.begin(), part.end());
    return result;
  }
  void same_wire(pointer const & actual, sort_profile_array<P> const & expected) {
    auto wanted = wire(expected);
    if (auto owned = actual->owned()) assert(wire(*owned) == wanted);
    else {
      assert(actual->sealed());
      auto source = file<P>::open(actual->sealed()->receipt.path);
      auto body = source.body();
      assert(std::ranges::equal(body.bytes(), wanted));
      actual->mapped()->scan();
    }
  }
  sort_profile_array<P> expected(pointer a, pointer b) {
    sort_profile_merge_builder<P, native> out(std::move(a), std::move(b));
    while (!out.done()) out.step(1);
    return out.finish();
  }
  std::size_t files(std::filesystem::path const & root) {
    std::size_t result = 0;
    for (auto const & entry : std::filesystem::recursive_directory_iterator(root))
      if (entry.path().extension() == ".kv" || entry.path().extension() == ".index") ++result;
    return result;
  }
  struct memory_stream {
    std::vector<std::byte> bytes;
    void append(std::span<std::byte const> part) { bytes.insert(bytes.end(), part.begin(), part.end()); }
    std::uint64_t body_bytes() const noexcept { return bytes.size(); }
  };
  void bit_tails() {
    for (unsigned tail = 0; tail != 8; ++tail) {
      memory_stream out;
      sort_profile_detail::file_bit_sink<P, posix_object_ops, memory_stream> sink(out);
      bit_string expected; sort_bit_writer oracle(expected);
      auto ones = (std::uint64_t{1} << (16 + tail)) - 1;
      sink.write_bits(ones, 16 + tail); oracle.write_bits(ones, 16 + tail);
      sink.flush_complete(); sink.flush_complete();
      assert(out.bytes.size() == 2 && sink.buffered_bits() == tail && sink.position() == 16 + tail);
      assert(compare_bits(sink.buffered_payload(), expected.view().subview(16, tail)) == 0);
      sink.write_bits(0, 11); oracle.write_bits(0, 11);
      sink.finish_payload(); assert(out.bytes == expected.bytes);

      // A first physical flush must preserve every possible incoming bit tail.
      memory_stream spilled;
      sort_profile_detail::file_bit_sink<P, posix_object_ops, memory_stream> second(spilled);
      bit_string reference; sort_bit_writer reference_out(reference);
      second.write_bits(ones, 16 + tail); reference_out.write_bits(ones, 16 + tail);
      auto large = bit_string::from_bytes(std::string(100'001, char(0xa5)));
      second.append(large.view()); reference_out.append(large.view());
      assert(!spilled.bytes.empty()); second.finish_payload();
      assert(spilled.bytes == reference.bytes);
    }
  }

  void small_and_budget() {
    temporary dir; auto catalog = sqlite_catalog<P>::create_sessions(dir.root, id());
    auto disk = storage::open(dir.root); auto context = disk.context();
    auto empty = merge(context, disk.empty(), disk.empty());
    assert(empty->owned() && !empty->size()); empty->view().scan(); empty.reset();
    auto a = row("a", "one"), b = row("b", "two");
    auto result = merge(context, a, b);
    assert(result->owned() && !result->sealed() && !context->sealed_outputs() && !files(dir.root));
    same_wire(result, expected(a, b));
    assert(context->retained_output_bytes() == result->owned()->retained_bytes() + sizeof(output_budget::lease));
    assert(context->retained_output_bytes() <= context->output_limit());
    // Doomed private results neither reserve catalog objects nor leave files.
    for (unsigned i = 0; i != 20; ++i) { auto discarded = merge(context, a, b); assert(discarded->owned()); }
    assert(!files(dir.root));
    auto charge = context->retained_output_bytes();
    auto last = result; result.reset(); assert(context->retained_output_bytes() == charge);
    std::jthread retire([last = std::move(last)]() mutable { last.reset(); }); retire.join();
    assert(!context->retained_output_bytes());

    // One retained output fits, two do not. Exhaustion spills without replay,
    // and retiring the owner returns room to the same execution context.
    auto limited = storage::open(dir.root, {}, {}, {}, {}, {2048, 2048}).context();
    auto large = row("b", std::string(1200, 'v'));
    auto first = merge(limited, a, large);
    assert(first->owned() && limited->retained_output_bytes() > 1024 && limited->retained_output_bytes() <= 2048);
    auto second = merge(limited, a, large);
    assert(second->mapped() && second->sealed() && limited->sealed_outputs() == 1);
    same_wire(second, expected(a, large));
    first.reset(); assert(!limited->retained_output_bytes());
    auto third = merge(limited, a, large); assert(third->owned());
    std::weak_ptr<storage::context_type> old_context = limited;
    limited.reset(); assert(old_context.expired());
    third->view().scan(); // The allowance lease owns no SQLite/context lifetime.
  }

  void eager_and_spill() {
    temporary dir; auto catalog = sqlite_catalog<P>::create_sessions(dir.root, id());
    auto a = row("prefix/a", "one"), b = row("prefix/b", "two");
    for (auto options : {runtime_output_options{0, 128 * 1024}, runtime_output_options{8 * 1024 * 1024, 0},
                         runtime_output_options{8 * 1024 * 1024, 512}}) {
      auto context = storage::open(dir.root, {}, {}, {}, {}, options).context();
      auto job = context->make_merge(a, b, replace_native_value{});
      assert(job->spilled()); // Zero allowance or navigation alone forces streaming.
      while (!job->done()) job->step(1);
      auto result = context->finish_merge(*job); assert(result->mapped());
      same_wire(result, expected(a, b));
    }
    auto context = storage::open(dir.root).context();
    auto big = row("prefix/c", std::string(150'001, 'x'));
    auto job = context->make_merge(a, big, replace_native_value{});
    assert(!job->spilled());
    while (!job->done()) job->step(1);
    assert(job->spilled());
    auto result = context->finish_merge(*job);
    assert(result->mapped() && !context->retained_output_bytes());
    same_wire(result, expected(a, big));
  }

  struct expanding {
    std::shared_ptr<unsigned> calls;
    std::shared_ptr<bit_string const> value;
    bit_view operator()(bit_view, bit_view) const { ++*calls; return value->view(); }
  };
  void composition_once() {
    temporary dir; auto catalog = sqlite_catalog<P>::create_sessions(dir.root, id());
    auto context = storage::open(dir.root).context();
    auto a = row("same", "a"), b = row("same", "b");
    auto command = core::put("same", std::string(200'000, 'q'));
    auto value = std::make_shared<bit_string const>(command.records().front().value);
    auto calls = std::make_shared<unsigned>(0);
    auto result = merge(context, a, b, expanding{calls, value});
    assert(*calls == 1 && result->mapped() && result->size() == 1);
    sort_profile_writer<P> oracle; oracle.append<strings>("same", std::optional<std::string>(std::string(200'000, 'q')));
    same_wire(result, oracle.finish());
  }

  struct faults { int file = 0, fail_commit = 0, commits = 0; bool after = false; };
  struct file_ops : posix_object_ops {
    std::shared_ptr<faults> state;
    explicit file_ops(std::shared_ptr<faults> value = {}) : state(std::move(value)) {}
    std::ptrdiff_t write(int fd, std::span<std::byte const> bytes) noexcept {
      if (state && state->file == 1) { state->file = 0; errno = EIO; return -1; }
      return posix_object_ops::write(fd, bytes);
    }
    int sync_file(int fd) noexcept {
      if (state && state->file == 2) { state->file = 0; errno = EIO; return -1; }
      return posix_object_ops::sync_file(fd);
    }
  };
  struct catalog_ops {
    std::shared_ptr<faults> state;
    explicit catalog_ops(std::shared_ptr<faults> value = {}) : state(std::move(value)) {}
    int commit(sqlite3 * db) noexcept {
      auto fail = state && ++state->commits == state->fail_commit;
      if (fail && !state->after) return SQLITE_IOERR_FSYNC;
      auto result = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
      return fail && result == SQLITE_OK ? SQLITE_IOERR_FSYNC : result;
    }
  };
  void spill_failures() {
    using F = streaming_sort_runtime_family<P, registry_selector<string_registry>, random_object_ids, catalog_ops, file_ops>;
    for (unsigned mode = 0; mode != 6; ++mode) {
      temporary dir; auto catalog = sqlite_catalog<P>::create_sessions(dir.root, id());
      auto state = std::make_shared<faults>();
      auto context = F::storage_type::open(dir.root, {}, {}, catalog_ops(state), file_ops(state)).context();
      auto a = row("a", "old"), b = row("b", std::string(100'001, 'b'));
      auto job = context->make_merge(a, b, replace_native_value{});
      assert(!job->spilled() && !state->commits);
      if (mode == 0) state->file = 1;
      else if (mode >= 2) { state->fail_commit = mode < 4 ? 1 : 2; state->after = mode & 1; }
      rejects([&] {
        while (!job->done()) job->step(1);
        if (mode == 1) state->file = 2;
        (void)context->finish_merge(*job);
      });
      assert(context->failed() && !context->sealed_outputs() && !context->retained_output_bytes());
      a->view().scan(); b->view().scan();
      rejects([&] { (void)context->make_merge(a, b, replace_native_value{}); });
      auto reopened = sqlite_catalog<P>::open(dir.root); assert(reopened.identity() == id());
    }
  }
}
int main() {
  try { bit_tails(); small_and_budget(); eager_and_spill(); composition_once(); spill_failures(); }
  catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
