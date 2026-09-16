/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks shared output allowances, rollback and concurrent owner retirement.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/output_budget.h>

#include <barrier>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

namespace {
  using namespace diet;
  void check(bool value, char const * message) {
    if (!value) throw std::runtime_error(message);
  }
  void accounting() {
    output_budget budget(100);
    auto alias = budget;
    auto first = budget.try_acquire(70);
    check(first && alias.used() == 70, "copied budget did not share allowance");
    check(!alias.try_acquire(31) && budget.used() == 70, "failed acquisition changed allowance");
    auto second = alias.try_acquire(30);
    check(second && budget.used() == 100, "exact capacity rejected");
    first->shrink(40);
    check(alias.used() == 70 && first->bytes() == 40, "shrink lost accounting");
    bool rejected = false;
    try { first->shrink(41); } catch (std::invalid_argument const &) { rejected = true; }
    check(rejected && budget.used() == 70 && first->bytes() == 40, "invalid growth changed accounting");
    *first = std::move(*second);
    check(budget.used() == 30 && first->bytes() == 30 && !second->bytes(), "move assignment leaked charge");
    first->reset(); first->reset(); second.reset();
    check(!budget.used(), "repeated reset returned charge twice");
    auto zero = budget.try_acquire(0);
    check(zero && !zero->bytes() && !budget.used(), "zero charge rejected");
    output_budget empty(0);
    check(empty.try_acquire(0).has_value() && !empty.try_acquire(1), "zero limit failed");
    output_budget huge(std::numeric_limits<std::size_t>::max());
    auto full = huge.try_acquire(huge.limit());
    check(full && huge.used() == huge.limit() && !huge.try_acquire(1), "allowance arithmetic wrapped");
  }

  struct observed {
    output_budget budget;
    bool * destroyed;
    observed(output_budget value, bool & flag) : budget(std::move(value)), destroyed(&flag) {}
    observed(observed const &) = delete;
    observed(observed && other) noexcept : budget(other.budget), destroyed(std::exchange(other.destroyed, nullptr)) {}
    ~observed() {
      if (destroyed) { *destroyed = budget.used() == 80; }
    }
  };
  struct throwing {
    throwing() = default;
    throwing(throwing const &) = delete;
    throwing(throwing &&) { throw std::runtime_error("move failed"); }
  };
  void owners() {
    output_budget budget(100);
    bool destroyed = false;
    auto charge = budget.try_acquire(80);
    auto owner = output_budget::attach(observed(budget, destroyed), std::move(*charge));
    auto copy = owner;
    owner.reset();
    check(budget.used() == 80 && !destroyed, "intermediate owner returned charge");
    std::jthread retire([copy = std::move(copy)]() mutable { copy.reset(); });
    retire.join();
    check(!budget.used() && destroyed, "charge returned before value destruction");
    auto failed = budget.try_acquire(100);
    bool caught = false;
    try { (void)output_budget::attach(throwing{}, std::move(*failed)); }
    catch (std::runtime_error const &) { caught = true; }
    check(caught && !budget.used(), "throwing holder leaked charge");

    // The allowance's original wrapper need not outlive a completed output.
    auto independent = [] {
      output_budget local(9);
      return output_budget::attach(42, std::move(*local.try_acquire(9)));
    }();
    check(*independent == 42, "output lost ownership with context");
  }
  void concurrent() {
    output_budget budget(64);
    std::barrier ready(4);
    std::atomic<bool> valid{true};
    std::vector<std::jthread> threads;
    for (unsigned i = 0; i != 4; ++i) threads.emplace_back([&, i] {
      ready.arrive_and_wait();
      for (unsigned j = 0; j != 1000; ++j) {
        auto charge = budget.try_acquire(8 + i);
        if (budget.used() > budget.limit()) valid = false;
        if (charge) {
          auto owner = output_budget::attach(j, std::move(*charge));
          if (*owner != j) valid = false;
        }
      }
    });
    threads.clear();
    check(valid && !budget.used(), "concurrent allowances exceeded or leaked capacity");
  }
}

int main() {
  try { accounting(); owners(); concurrent(); }
  catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
