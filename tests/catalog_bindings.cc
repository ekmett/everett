/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks concurrent catalog bindings, retry, shared dependencies and owner lifetimes.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/catalog_bindings.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <future>
#include <iostream>
#include <latch>
#include <thread>

namespace {
  using namespace diet;
  using namespace std::chrono_literals;

  void check(bool value, char const * message) {
    if (!value) throw std::runtime_error(message);
  }

  template <class F> void rejects(F && action) {
    bool caught = false;
    try { action(); } catch (std::exception const &) { caught = true; }
    check(caught, "expected producer rejection");
  }

  struct temporary {
    std::filesystem::path root, first, second;
    temporary() {
      auto base = std::filesystem::temp_directory_path();
      auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
      for (unsigned i = 0; i != 100; ++i) {
        auto candidate = base / ("diet-binding-" + std::to_string(nonce) + "-" + std::to_string(i));
        if (!std::filesystem::create_directory(candidate)) continue;
        root = std::move(candidate);
        std::filesystem::create_directory(root / "first");
        std::filesystem::create_directory(root / "second");
        first = std::filesystem::canonical(root / "first");
        second = std::filesystem::canonical(root / "second");
        return;
      }
      throw std::runtime_error("cannot create binding fixture directory");
    }
    ~temporary() {
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
    }
  };

  object_id identity(unsigned value) {
    std::string text(32, '0');
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i != 8; ++i) text[31 - i] = hex[(value >> (4 * i)) & 15];
    return object_id(std::move(text));
  }

  template <class F> void parallel(std::size_t count, F && action) {
    std::barrier ready(static_cast<std::ptrdiff_t>(count));
    std::vector<std::exception_ptr> errors(count);
    std::vector<std::jthread> threads;
    for (std::size_t i = 0; i != count; ++i) {
      threads.emplace_back([&, i] {
        ready.arrive_and_wait();
        try { action(i); } catch (...) { errors[i] = std::current_exception(); }
      });
    }
    threads.clear();
    for (auto const & error : errors) if (error) std::rethrow_exception(error);
  }

  void same_slot(std::filesystem::path const & root) {
    catalog_bindings<unsigned> bindings;
    auto id = identity(1);
    check(!bindings.find(id, root), "absent find produced a value");
    std::atomic<unsigned> calls = 0;
    std::latch producing(1), release(1);
    std::vector<catalog_bindings<unsigned>::pointer> results(24);
    auto workers = std::async(std::launch::async, [&] {
      parallel(results.size(), [&](std::size_t i) {
        results[i] = bindings.get_or_create(id, root, [&] {
          if (calls.fetch_add(1) == 0) producing.count_down();
          release.wait();
          return std::make_shared<unsigned const>(1729);
        });
      });
    });
    producing.wait();
    auto observer = std::async(std::launch::async, [&] { return bindings.find(id, root); });
    release.count_down();
    workers.get();
    check(calls == 1 && *results[0] == 1729, "same slot ran multiple successful producers");
    for (auto const & result : results) check(result == results[0], "same slot returned distinct values");
    check(observer.get() == results[0], "concurrent find missed acknowledged value");
    check(bindings.get_or_create(id, root, []() -> catalog_bindings<unsigned>::pointer {
      throw std::runtime_error("cached producer must not run");
    }) == results[0], "cached lookup changed value");
  }

  void retries(std::filesystem::path const & root) {
    catalog_bindings<unsigned> bindings;
    auto id = identity(2);
    std::atomic<unsigned> attempts = 0;
    std::latch producing(1), release(1);
    auto failed = std::async(std::launch::async, [&] {
      rejects([&] {
        bindings.get_or_create(id, root, [&]() -> catalog_bindings<unsigned>::pointer {
          ++attempts;
          producing.count_down();
          release.wait();
          throw std::runtime_error("unacknowledged backend result");
        });
      });
    });
    producing.wait();
    std::vector<catalog_bindings<unsigned>::pointer> results(16);
    auto healthy = std::async(std::launch::async, [&] {
      parallel(results.size(), [&](std::size_t i) {
        results[i] = bindings.get_or_create(id, root, [&] {
          ++attempts;
          return std::make_shared<unsigned const>(99);
        });
      });
    });
    release.count_down();
    failed.get();
    healthy.get();
    check(attempts == 2, "failed producer prevented single healthy retry");
    for (auto const & result : results) check(result == results[0] && *result == 99, "retry result mismatch");

    auto other = identity(3);
    rejects([&] { bindings.get_or_create(other, root, [] { return catalog_bindings<unsigned>::pointer{}; }); });
    check(!bindings.find(other, root), "null producer installed a value");
    auto value = bindings.get_or_create(other, root, [] { return std::make_shared<unsigned const>(100); });
    check(value && *value == 100, "null producer poisoned healthy retry");
  }

  void namespaces(temporary const & directories) {
    catalog_bindings<unsigned> bindings;
    auto first = identity(4), second = identity(5);
    unsigned calls = 0;
    auto make = [&] { return std::make_shared<unsigned const>(++calls); };
    auto a = bindings.get_or_create(first, directories.first, make);
    auto b = bindings.get_or_create(second, directories.first, make);
    auto c = bindings.get_or_create(first, directories.second, make);
    auto d = bindings.get_or_create(second, directories.second, make);
    check(calls == 4 && a != b && a != c && b != d && c != d, "catalog/root namespaces aliased");
    check(bindings.find(first, directories.first) == a && bindings.find(first, directories.second) == c,
      "lookup crossed copied catalog roots");
    auto canonical = std::filesystem::canonical(directories.first / ".");
    check(bindings.get_or_create(first, canonical, make) == a && calls == 4, "canonical root failed reuse");

    // Production occurs after releasing the bindings-vector lock. A producer
    // blocked in one namespace must not block a different namespace slot.
    std::latch entered(1), release(1);
    auto held = std::async(std::launch::async, [&] {
      return bindings.get_or_create(identity(6), directories.first, [&] {
        entered.count_down();
        release.wait();
        return std::make_shared<unsigned const>(1);
      });
    });
    entered.wait();
    auto independent = std::async(std::launch::async, [&] {
      return bindings.get_or_create(identity(6), directories.second, [] {
        return std::make_shared<unsigned const>(2);
      });
    });
    auto status = independent.wait_for(10s);
    release.count_down();
    check(*held.get() == 1 && *independent.get() == 2 && status == std::future_status::ready,
      "unrelated namespace producer was serialized");
  }

  struct lifetime {
    std::atomic<unsigned> owners = 0, values = 0, calls = 0;
  };

  struct value {
    unsigned id;
    std::vector<std::shared_ptr<value const>> dependencies;
    std::shared_ptr<lifetime> counts;
    value(unsigned id_, std::vector<std::shared_ptr<value const>> inputs, std::shared_ptr<lifetime> tracker)
      : id(id_), dependencies(std::move(inputs)), counts(std::move(tracker)) { ++counts->values; }
    ~value() { --counts->values; }
  };

  struct owner {
    unsigned id;
    std::vector<std::shared_ptr<owner const>> dependencies;
    std::shared_ptr<lifetime> counts;
    mutable std::atomic<unsigned> produced = 0;
    catalog_bindings<value> bindings;
    owner(unsigned id_, std::vector<std::shared_ptr<owner const>> inputs, std::shared_ptr<lifetime> tracker)
      : id(id_), dependencies(std::move(inputs)), counts(std::move(tracker)) { ++counts->owners; }
    ~owner() { --counts->owners; }
  };

  std::shared_ptr<value const> seal(std::shared_ptr<owner const> const & source,
      object_id const & catalog, std::filesystem::path const & root) {
    return source->bindings.get_or_create(catalog, root, [&] {
      ++source->produced;
      ++source->counts->calls;
      std::this_thread::yield();
      std::vector<std::shared_ptr<value const>> inputs;
      for (auto const & child : source->dependencies) inputs.push_back(seal(child, catalog, root));
      return std::make_shared<value const>(source->id, std::move(inputs), source->counts);
    });
  }

  void shared_graph(std::filesystem::path const & root) {
    auto counts = std::make_shared<lifetime>();
    std::vector<std::shared_ptr<owner const>> nodes;
    // A shared chain plus diamonds makes callers arrive at the same suffix
    // through different parents. Every recursive dependency has a smaller ID.
    for (unsigned i = 0; i != 80; ++i) {
      std::vector<std::shared_ptr<owner const>> inputs;
      if (i) inputs.push_back(nodes[i - 1]);
      if (i > 2) inputs.push_back(nodes[i / 2]);
      nodes.push_back(std::make_shared<owner const>(i, std::move(inputs), counts));
    }
    std::vector<std::shared_ptr<owner const>> roots;
    for (unsigned i = 0; i != 12; ++i) {
      std::vector<std::shared_ptr<owner const>> inputs{nodes[79], nodes[32 + i]};
      roots.push_back(std::make_shared<owner const>(80 + i, std::move(inputs), counts));
    }
    std::vector<std::weak_ptr<owner const>> weak_owners(nodes.begin(), nodes.end());
    weak_owners.insert(weak_owners.end(), roots.begin(), roots.end());
    std::vector<std::shared_ptr<value const>> results(roots.size());
    auto catalog = identity(7);
    parallel(roots.size(), [&](std::size_t i) { results[i] = seal(roots[i], catalog, root); });
    check(counts->calls == 92 && counts->values == 92, "shared suffix did not ripple exactly once");
    for (auto const & node : nodes) {
      check(node->produced == 1, "shared owner produced repeatedly");
      auto binding = node->bindings.find(catalog, root);
      check(binding && binding->id == node->id && binding->dependencies.size() == node->dependencies.size(),
        "binding lost immutable dependency shape");
      for (std::size_t i = 0; i != node->dependencies.size(); ++i)
        check(binding->dependencies[i] == node->dependencies[i]->bindings.find(catalog, root),
          "dependency did not use shared child binding");
    }
    parallel(roots.size(), [&](std::size_t i) {
      check(seal(roots[i], catalog, root) == results[i], "retained head changed its binding");
    });
    check(counts->calls == 92, "retained head revisited producer suffix");
    std::vector<std::weak_ptr<value const>> weak_values(results.begin(), results.end());
    roots.clear();
    nodes.clear();
    for (auto const & weak : weak_owners) check(weak.expired(), "binding registry retained an owner cycle");
    check(counts->owners == 0 && counts->values == 92, "external result lost dependency value pins");
    results.clear();
    for (auto const & weak : weak_values) check(weak.expired(), "last value reference was retained globally");
    check(counts->values == 0, "binding values outlived all owners and results");

    // A failed parent may leave an acknowledged dependency cached. Its retry
    // must reuse that dependency and must not preserve the failed parent.
    auto child = std::make_shared<owner const>(1, std::vector<std::shared_ptr<owner const>>{}, counts);
    auto parent = std::make_shared<owner const>(2, std::vector<std::shared_ptr<owner const>>{child}, counts);
    rejects([&] {
      parent->bindings.get_or_create(catalog, root, [&]() -> std::shared_ptr<value const> {
        (void)seal(child, catalog, root);
        throw std::runtime_error("parent backend failure after dependency acknowledgment");
      });
    });
    check(!parent->bindings.find(catalog, root) && child->produced == 1, "failed parent changed child acknowledgment");
    auto recovered = seal(parent, catalog, root);
    check(recovered->dependencies[0] == child->bindings.find(catalog, root) && child->produced == 1,
      "parent retry regenerated acknowledged child");
    recovered.reset();
    parent.reset();
    child.reset();
    check(counts->owners == 0 && counts->values == 0, "retry left retained owners or bindings");
  }
}

int main() {
  temporary directories;
  same_slot(directories.first);
  retries(directories.first);
  namespaces(directories);
  shared_graph(directories.first);
  std::cout << "catalog binding tests passed\n";
}
