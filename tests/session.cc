/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks session publication, bounded admission, cancellation and worker failure.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/session.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {
  using namespace everett;
  using namespace std::chrono_literals;

  void check(bool condition, char const * message) {
    if (!condition) { std::cerr << message << '\n'; std::abort(); }
  }
  template <class E, class F> void rejects(F && operation, char const * message) {
    bool caught = false;
    try { operation(); }
    catch (E const &) { caught = true; }
    catch (...) { check(false, "unexpected exception type"); }
    check(caught, message);
  }

  // A timeout is only a deadlock watchdog, never a scheduling assumption.
  struct watchdog {
    std::mutex mutex;
    std::condition_variable changed;
    bool finished = false;
    std::thread worker{[this] {
      std::unique_lock lock(mutex);
      check(changed.wait_for(lock, 30s, [&] { return finished; }), "session test deadlocked");
    }};
    ~watchdog() {
      { std::lock_guard lock(mutex); finished = true; }
      changed.notify_all(); worker.join();
    }
  };
  struct signal {
    std::mutex mutex;
    std::condition_variable changed;
    bool ready = false;
    void set() {
      { std::lock_guard lock(mutex); ready = true; }
      changed.notify_all();
    }
    void wait() {
      std::unique_lock lock(mutex);
      check(changed.wait_for(lock, 10s, [&] { return ready; }), "session test signal missing");
    }
  };
  struct control {
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<int> blocked_id;
    bool active_entered = false, active_released = false;
    bool gate_maintenance = false;
    bool fail_maintenance = false;
    int fail_pending_id = 0;
    unsigned maintenance_entered = 0, maintenance_released = 0;
    std::vector<std::uint64_t> budgets;
    std::optional<std::thread::id> worker;

    void record_worker() {
      auto current = std::this_thread::get_id();
      check(!worker || *worker == current, "Engine was accessed by multiple workers");
      worker = current;
    }
    void enter(int id) {
      std::unique_lock lock(mutex); record_worker();
      if (blocked_id == id) {
        active_entered = true; changed.notify_all();
        changed.wait(lock, [&] { return active_released; });
      }
    }
    void wait_active() {
      std::unique_lock lock(mutex);
      check(changed.wait_for(lock, 10s, [&] { return active_entered; }), "worker did not claim contribution");
    }
    void release_active() {
      { std::lock_guard lock(mutex); active_released = true; }
      changed.notify_all();
    }
    void enter_maintenance(std::uint64_t budget) {
      std::unique_lock lock(mutex); record_worker();
      budgets.push_back(budget);
      auto ordinal = ++maintenance_entered; changed.notify_all();
      if (gate_maintenance) changed.wait(lock, [&] { return maintenance_released >= ordinal; });
    }
    void wait_maintenance(unsigned count) {
      std::unique_lock lock(mutex);
      check(changed.wait_for(lock, 10s, [&] { return maintenance_entered >= count; }), "worker did not advance maintenance");
    }
    void release_maintenance(unsigned count) {
      { std::lock_guard lock(mutex); maintenance_released = count; }
      changed.notify_all();
    }
  };
  struct copy_control {
    signal entered, released;
    bool fail = false;
  };
  struct contribution {
    int id = 0;
    std::uint64_t work = 1, bytes = 1;
    std::shared_ptr<int> input;
    unsigned maintenance = 0;
    bool fail = false;
    std::shared_ptr<signal> admission;
    std::shared_ptr<copy_control> copying;
    contribution() = default;
    contribution(contribution const & other)
      : id(other.id), work(other.work), bytes(other.bytes), input(other.input), maintenance(other.maintenance),
        fail(other.fail), admission(other.admission), copying(other.copying) {
      if (copying) {
        copying->entered.set(); copying->released.wait();
        if (copying->fail) throw std::runtime_error("injected contribution copy failure");
      }
    }
    contribution(contribution &&) noexcept = default;
    contribution & operator=(contribution const &) = default;
    contribution & operator=(contribution &&) noexcept = default;
  };
  struct engine_failure : std::runtime_error {
    std::shared_ptr<int const> identity;
    explicit engine_failure(int id)
      : std::runtime_error("injected session engine failure"), identity(std::make_shared<int const>(id)) {}
  };
  struct contents {
    std::vector<int> values;
    unsigned layout = 0;
  };
  struct engine {
    using world_type = std::shared_ptr<contents const>;
    using contribution_type = contribution;
    std::shared_ptr<control> gate;
    contents state;
    unsigned remaining = 0;

    explicit engine(std::shared_ptr<control> value) : gate(std::move(value)) {}
    static session_reservation reservation(contribution const & value) {
      if (value.admission) value.admission->set();
      return {value.work, value.bytes};
    }
    world_type snapshot() const { return std::make_shared<contents const>(state); }
    world_type contribute(contribution value) {
      gate->enter(value.id);
      state.values.push_back(value.id);
      // Failure after private mutation must not replace the immutable snapshot.
      if (value.fail) throw engine_failure(value.id);
      remaining += value.maintenance;
      return snapshot();
    }
    bool pending() const {
      if (gate->fail_pending_id && !state.values.empty() && state.values.back() == gate->fail_pending_id)
        throw engine_failure(gate->fail_pending_id);
      return remaining != 0;
    }
    std::optional<world_type> advance(std::uint64_t budget) {
      check(remaining && budget, "worker advanced without maintenance or budget");
      gate->enter_maintenance(budget);
      if (gate->fail_maintenance) throw engine_failure(99);
      --remaining; ++state.layout;
      return snapshot();
    }
  };
  using pipe_type = session<engine>;
  struct ready_engine : engine {
    using engine::engine;
    bool admission_ready() const { return remaining == 0; }
    world_type contribute(contribution value) {
      check(admission_ready(), "claimed input before retiring prior debt");
      return engine::contribute(std::move(value));
    }
  };
  struct validating_engine : engine {
    using engine::engine;
    bool broken = false;
    bool failed() const noexcept { return broken; }
    world_type contribute(contribution value) {
      if (value.id < 0) throw std::invalid_argument("rejected before mutation");
      try { return engine::contribute(std::move(value)); }
      catch (...) { broken = true; throw; }
    }
  };

  contribution input(int id, std::uint64_t work = 1, std::uint64_t bytes = 1) {
    contribution value; value.id = id; value.work = work; value.bytes = bytes; return value;
  }
  void empty(pipe_type const & pipe) {
    auto reserved = pipe.outstanding();
    check(!reserved.work && !reserved.bytes && !pipe.pending_count(), "completed session retained admission charge");
  }

  void fifo() {
    auto gate = std::make_shared<control>(); gate->blocked_id = 1;
    pipe_type pipe(engine(gate), {100, 100, 8, 7});
    auto initial = pipe.snapshot();
    check(!pipe.failure(), "healthy session reported a worker failure");
    check(initial->generation == 0 && initial->revision == 0 && initial->world->values.empty(), "initial publication");
    auto first = pipe.submit(input(1)); gate->wait_active();
    auto second = pipe.submit(input(2));
    auto third = pipe.submit(input(3));
    check(first.valid() && second.valid() && third.valid(), "accepted ticket invalid");
    check(pipe.snapshot() == initial && pipe.pending_count() == 3, "pending contribution became visible");
    gate->release_active();
    auto one = first.get(); auto two = second.get(); auto three = third.get();
    check(one->world->values == std::vector<int>{1}, "first FIFO snapshot");
    check(two->world->values == std::vector<int>({1, 2}), "second FIFO snapshot");
    check(three->world->values == std::vector<int>({1, 2, 3}), "third FIFO snapshot");
    check(one->generation == 1 && one->revision == 1 && two->generation == 2 && two->revision == 2 &&
      three->generation == 3 && three->revision == 3, "contribution stamps");
    check(one->logical != initial->logical && two->logical != one->logical && three->logical != two->logical,
      "contribution reused logical identity");
    first.wait(); check(first.get() == one, "ticket get is not repeatable");
    pipe.shutdown(); pipe.shutdown(); empty(pipe);
    check(pipe.snapshot() == three && initial->world->values.empty(), "publication lifetime or FIFO head");
  }

  void maintenance() {
    auto gate = std::make_shared<control>(); gate->gate_maintenance = true;
    pipe_type pipe(engine(gate), {100, 100, 8, 7});
    auto value = input(4); value.maintenance = 2;
    auto accepted = pipe.submit(std::move(value)).get();
    gate->wait_maintenance(1);
    check(pipe.snapshot() == accepted, "unfinished maintenance became visible");
    gate->release_maintenance(1); gate->wait_maintenance(2);
    auto revised = pipe.snapshot();
    check(revised->generation == accepted->generation && revised->revision == accepted->revision + 1,
      "equivalent maintenance changed logical generation");
    check(revised->logical == accepted->logical && revised->world->values == accepted->world->values &&
      revised->world->layout == accepted->world->layout + 1, "equivalent maintenance publication");
    check(accepted->world->layout == 0, "maintenance mutated retained snapshot");
    pipe.close(); gate->release_maintenance(2); pipe.shutdown(); empty(pipe);
    check(gate->budgets == std::vector<std::uint64_t>({7, 7}), "maintenance budget or shutdown service");
  }

  void admission() {
    for (session_limits limits : {session_limits{3, 100, 2, 7}, session_limits{100, 7, 2, 7}, session_limits{100, 100, 1, 7}}) {
      auto gate = std::make_shared<control>(); gate->blocked_id = 1;
      pipe_type pipe(engine(gate), limits);
      auto first = pipe.submit(input(1, 3, 7)); gate->wait_active();
      auto held = pipe.outstanding();
      check(held.work == 3 && held.bytes == 7 && pipe.pending_count() == 1, "active reservation released early");
      auto refused = input(2); refused.input = std::make_shared<int>(2);
      check(!pipe.try_submit(std::move(refused)), "try_submit exceeded an admission limit");
      check(bool(refused.input), "saturated try_submit consumed caller input");
      rejects<std::length_error>([&] { (void)pipe.try_submit(input(3, limits.work + 1, 0)); }, "oversize work accepted");
      rejects<std::length_error>([&] { (void)pipe.submit(input(3, 0, limits.bytes + 1)); }, "oversize bytes blocked or accepted");
      gate->release_active(); first.get(); pipe.shutdown(); empty(pipe);
    }
    auto gate = std::make_shared<control>(); gate->blocked_id = 1;
    auto maximum = std::numeric_limits<std::uint64_t>::max();
    pipe_type pipe(engine(gate), {maximum, maximum, 2, 1});
    auto first = pipe.submit(input(1, maximum - 1, maximum - 1)); gate->wait_active();
    check(!pipe.try_submit(input(2, 2, 2)), "admission counters wrapped");
    gate->release_active(); first.get(); pipe.shutdown(); empty(pipe);
  }

  void cancellation() {
    auto gate = std::make_shared<control>(); gate->blocked_id = 1;
    pipe_type pipe(engine(gate), {10, 20, 3, 1});
    pipe_type other(engine(std::make_shared<control>()), {10, 20, 3, 1});
    pipe_type::ticket invalid;
    check(!invalid.valid() && !pipe.cancel(invalid), "invalid ticket appeared live");
    auto active_pin = std::make_shared<int>(1), queued_pin = std::make_shared<int>(2);
    std::weak_ptr<int> active = active_pin, queued = queued_pin;
    auto a = input(1, 3, 7); a.input = std::move(active_pin);
    auto first = pipe.submit(std::move(a)); gate->wait_active();
    auto b = input(2, 2, 5); b.input = std::move(queued_pin);
    auto second = pipe.submit(std::move(b));
    check(!active.expired() && !queued.expired(), "accepted input lost its owner");
    check(!pipe.cancel(first) && !other.cancel(second), "cancel accepted running or foreign ticket");
    check(pipe.cancel(second) && !pipe.cancel(second), "queued cancellation is not exactly once");
    rejects<session_cancelled>([&] { (void)second.get(); }, "cancelled ticket succeeded");
    rejects<session_cancelled>([&] { (void)second.get(); }, "cancelled ticket outcome changed");
    check(queued.expired() && !active.expired(), "cancellation released wrong input pins");
    auto held = pipe.outstanding();
    check(held.work == 3 && held.bytes == 7 && pipe.pending_count() == 1, "cancellation reservation refund");
    gate->release_active(); auto result = first.get();
    check(active.expired() && result->world->values == std::vector<int>{1}, "successful input retained or cancelled input applied");
    check(!pipe.cancel(first), "completed ticket cancelled");
    pipe.shutdown(); other.shutdown(); empty(pipe);
  }

  void failure(bool pending_error) {
    auto gate = std::make_shared<control>(); gate->blocked_id = 1;
    if (pending_error) gate->fail_pending_id = 1;
    pipe_type pipe(engine(gate), {100, 100, 4, 1});
    auto initial = pipe.snapshot();
    auto a = input(1); a.fail = !pending_error; a.input = std::make_shared<int>(1);
    std::weak_ptr<int> first_pin = a.input;
    auto first = pipe.submit(std::move(a)); gate->wait_active();
    auto b = input(2); b.input = std::make_shared<int>(2); std::weak_ptr<int> second_pin = b.input;
    auto second = pipe.submit(std::move(b)); auto third = pipe.submit(input(3));
    gate->release_active();
    std::shared_ptr<int const> identity;
    auto failed = [&](auto && operation) {
      bool caught = false;
      try { operation(); }
      catch (engine_failure const & error) {
        caught = true;
        if (!identity) identity = error.identity;
        check(error.identity == identity && *identity == 1, "worker failure identity changed");
      }
      catch (...) { check(false, "worker failure type changed"); }
      check(caught, "failed worker operation succeeded");
    };
    failed([&] { first.get(); }); failed([&] { second.get(); }); failed([&] { third.get(); });
    failed([&] { first.get(); }); failed([&] { (void)pipe.try_submit(input(4)); });
    failed([&] { (void)pipe.submit(input(5)); });
    check(first_pin.expired() && second_pin.expired(), "failed worker retained input pins");
    check(pipe.snapshot() == initial && initial->world->values.empty(), "failed private mutation published");
    pipe.shutdown(); empty(pipe);
  }

  void maintenance_failure() {
    auto gate = std::make_shared<control>(); gate->gate_maintenance = true; gate->fail_maintenance = true;
    pipe_type pipe(engine(gate), {10, 10, 3, 1});
    auto value = input(1); value.maintenance = 1;
    auto first = pipe.submit(std::move(value)); auto accepted = first.get();
    gate->wait_maintenance(1);
    auto second = pipe.submit(input(2)), third = pipe.submit(input(3));
    gate->release_maintenance(1);
    rejects<engine_failure>([&] { (void)second.get(); }, "maintenance failure did not reject queued work");
    rejects<engine_failure>([&] { (void)third.get(); }, "maintenance failure lost a queued completion");
    check(first.get() == accepted && pipe.snapshot() == accepted, "maintenance failure changed logical publication");
    auto error = pipe.failure();
    check(bool(error), "background failure is not observable");
    rejects<engine_failure>([&] { std::rethrow_exception(error); }, "background failure lost its exception");
    pipe.shutdown(); empty(pipe);
  }

  void concurrent_submission() {
    constexpr unsigned count = 8;
    auto gate = std::make_shared<control>(); gate->blocked_id = 100;
    pipe_type pipe(engine(gate), {100, 100, count + 1, 1});
    auto first = pipe.submit(input(100)); gate->wait_active();
    std::latch ready(count), start(1);
    std::vector<pipe_type::ticket> tickets(count);
    std::vector<std::thread> submitters;
    for (unsigned i = 0; i != count; ++i) submitters.emplace_back([&, i] {
      ready.count_down(); start.wait(); tickets[i] = pipe.submit(input(int(i + 1)));
    });
    ready.wait(); start.count_down(); for (auto & thread : submitters) thread.join();
    check(pipe.pending_count() == count + 1, "concurrent submissions lost queue entries");
    gate->release_active(); first.get();
    std::vector<std::uint64_t> generations;
    for (unsigned i = 0; i != count; ++i) {
      auto publication = tickets[i].get();
      check(publication->world->values.front() == 100 && publication->world->values.back() == int(i + 1) &&
        publication->world->values.size() == publication->generation, "ticket publication disagrees with serialized order");
      generations.push_back(publication->generation);
    }
    std::sort(generations.begin(), generations.end());
    for (unsigned i = 0; i != count; ++i) check(generations[i] == i + 2, "concurrent generation reused or skipped");
    pipe.shutdown(); auto values = pipe.snapshot()->world->values;
    std::sort(values.begin(), values.end());
    check(values == std::vector<int>({1, 2, 3, 4, 5, 6, 7, 8, 100}), "concurrent contributions duplicated or lost");
    empty(pipe);
  }

  void close_wakes_submitter() {
    auto gate = std::make_shared<control>(); gate->blocked_id = 1;
    pipe_type pipe(engine(gate), {1, 1, 1, 1});
    auto first = pipe.submit(input(1)); gate->wait_active();
    auto admitted = std::make_shared<signal>();
    auto waiting = std::async(std::launch::async, [&] {
      auto value = input(2); value.admission = admitted; return pipe.submit(std::move(value));
    });
    admitted->wait();
    check(waiting.wait_for(0s) == std::future_status::timeout, "saturated submit completed before close");
    pipe.close(); pipe.close();
    rejects<session_closed>([&] { (void)waiting.get(); }, "close did not wake blocked submitter");
    rejects<session_closed>([&] { (void)pipe.try_submit(input(3)); }, "closed session accepted contribution");
    gate->release_active(); check(first.get()->world->values == std::vector<int>{1}, "close abandoned accepted contribution");
    pipe.shutdown(); empty(pipe);
  }

  void capacity_wakes_submitter() {
    auto gate = std::make_shared<control>(); gate->blocked_id = 1;
    pipe_type pipe(engine(gate), {1, 1, 1, 1});
    auto first = pipe.submit(input(1)); gate->wait_active();
    auto admitted = std::make_shared<signal>();
    auto waiting = std::async(std::launch::async, [&] {
      auto value = input(2); value.admission = admitted; return pipe.submit(std::move(value));
    });
    admitted->wait();
    check(waiting.wait_for(0s) == std::future_status::timeout, "saturated submit did not wait for capacity");
    gate->release_active(); first.get();
    auto second = waiting.get();
    check(second.get()->world->values == std::vector<int>({1, 2}), "capacity release did not admit waiting input");
    check(pipe.apply(input(3))->world->values == std::vector<int>({1, 2, 3}), "synchronous apply did not publish");
    pipe.shutdown(); empty(pipe);
  }

  void contribution_construction() {
    for (bool fail : {false, true}) {
      auto gate = std::make_shared<control>();
      pipe_type pipe(engine(gate), {2, 3, 1, 1});
      auto value = input(7, 2, 3); value.copying = std::make_shared<copy_control>(); value.copying->fail = fail;
      auto submitting = std::async(std::launch::async, [&] { return pipe.submit(value); });
      value.copying->entered.wait();
      auto held = pipe.outstanding();
      check(held.work == 2 && held.bytes == 3 && pipe.pending_count() == 1, "input copy began before reservation");
      check(!pipe.try_submit(input(8)), "constructing input did not count against admission");
      if (!fail) pipe.close();
      value.copying->released.set();
      if (fail) {
        rejects<std::runtime_error>([&] { (void)submitting.get(); }, "input copy failure was swallowed");
        empty(pipe);
        check(pipe.apply(input(8))->world->values == std::vector<int>{8}, "copy failure poisoned healthy Engine");
      } else {
        auto ticket = submitting.get();
        check(ticket.get()->world->values == std::vector<int>{7}, "close discarded previously reserved construction");
      }
      pipe.shutdown(); empty(pipe);
    }
  }

  void destruction_drains() {
    auto gate = std::make_shared<control>(); gate->blocked_id = 1;
    auto pipe = std::make_unique<pipe_type>(engine(gate), session_limits{10, 10, 2, 1});
    auto retained = pipe->snapshot();
    auto first = pipe->submit(input(1)); gate->wait_active(); auto second = pipe->submit(input(2));
    std::latch destroying(1), destroyed(1);
    std::thread destructor([owned = std::move(pipe), &destroying, &destroyed]() mutable {
      destroying.count_down(); owned.reset(); destroyed.count_down();
    });
    destroying.wait(); gate->release_active(); destroyed.wait(); destructor.join();
    check(first.get()->world->values == std::vector<int>{1} &&
      second.get()->world->values == std::vector<int>({1, 2}), "destructor did not drain accepted work");
    check(retained->world->values.empty(), "snapshot did not survive session destruction");
  }

  void required_maintenance() {
    auto gate = std::make_shared<control>(); gate->blocked_id = 1; gate->gate_maintenance = true;
    session<ready_engine> pipe(ready_engine(gate), {8, 8, 2, 1});
    auto value = input(1); value.maintenance = 2;
    auto first = pipe.submit(std::move(value)); gate->wait_active();
    auto second = pipe.submit(input(2));
    gate->release_active(); auto before = first.get(); gate->wait_maintenance(1);
    check(!second.ready() && pipe.pending_count() == 1 && pipe.outstanding().work == 1,
      "required maintenance released queued admission reservation");
    pipe.close(); // Accepted work must still get its prerequisite service.
    gate->release_maintenance(1); gate->wait_maintenance(2);
    auto intermediate = pipe.snapshot();
    check(intermediate->logical == before->logical && intermediate->revision == before->revision + 1,
      "required maintenance changed logical state");
    check(!second.ready(), "input claimed while prerequisite debt remained");
    gate->release_maintenance(2);
    auto after = second.get();
    check(after->world->values == std::vector<int>({1, 2}) && after->world->layout == 2 &&
      after->generation == 2 && after->revision == 4, "required service/publication order");
    pipe.shutdown();
    check(pipe.pending_count() == 0, "close left accepted work behind prerequisite service");
  }
  void rejected_contribution() {
    auto gate = std::make_shared<control>(); gate->blocked_id = 1;
    session<validating_engine> pipe(validating_engine(gate), {8, 8, 4, 1});
    auto first = pipe.submit(input(1)); gate->wait_active();
    auto rejected = pipe.submit(input(-1));
    auto following = pipe.submit(input(2));
    gate->release_active(); first.get();
    rejects<std::invalid_argument>([&] { (void)rejected.get(); }, "preflight rejection was swallowed");
    auto result = following.get();
    check(result->world->values == std::vector<int>({1, 2}) && result->generation == 2 && !pipe.failure(),
      "preflight rejection poisoned healthy Engine or published a generation");
    auto failing = input(3); failing.fail = true;
    rejects<engine_failure>([&] { (void)pipe.apply(std::move(failing)); }, "execution error was swallowed");
    check(bool(pipe.failure()) && pipe.snapshot() == result, "execution error was treated as safe rejection");
    pipe.shutdown();
  }
}

int main() {
  watchdog deadline;
  fifo(); maintenance(); admission(); cancellation(); failure(false); failure(true); maintenance_failure();
  concurrent_submission(); close_wakes_submitter(); capacity_wakes_submitter();
  contribution_construction(); destruction_drains(); required_maintenance(); rejected_contribution();
  std::cout << "session: publication, admission, cancellation and concurrency passed\n";
}
