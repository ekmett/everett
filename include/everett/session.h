/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Serializes charged contributions and equivalent layout work behind immutable snapshots.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <list>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace everett {
  struct session_reservation {
    std::uint64_t work = 0;
    std::uint64_t bytes = 0;
    bool operator==(session_reservation const &) const = default;
  };

  struct session_limits {
    std::uint64_t work;
    std::uint64_t bytes;
    std::uint64_t contributions;
    std::uint64_t maintenance_budget = 128;
  };

  struct session_closed : std::exception {
    char const * what() const noexcept override { return "session is closed"; }
  };
  struct session_cancelled : std::exception {
    char const * what() const noexcept override { return "queued session contribution was cancelled"; }
  };

  // Engine supplies world_type and contribution_type, plus:
  //   static session_reservation reservation(contribution_type const &);
  //   world_type snapshot() const;
  //   world_type contribute(contribution_type);
  //   bool pending() const;
  //   std::optional<world_type> advance(std::uint64_t budget);
  // reservation is thread-safe and independent of mutable Engine state. Its
  // work is the Engine's conservative admission allowance. Existing merge
  // debt is separate; an Engine can expose admission_ready() to require its
  // service before claiming another queued input.
  // advance returns only completed, equivalent layouts of the current world.
  // All returned worlds own their transitive pins independently of Engine.
  //
  // One worker owns Engine. Admission limits cover accepted input reservations,
  // including the running contribution, until its charged path completes. They
  // do not bound saved snapshots, Engine's working set, total I/O or elapsed time.
  // shutdown drains accepted contributions but stops optional maintenance. No
  // user Engine operation or contribution destructor runs under the queue lock.
  template <class Engine> struct session {
    using world_type = typename Engine::world_type;
    using contribution_type = typename Engine::contribution_type;

    struct logical_state {};
    struct publication {
      std::shared_ptr<logical_state const> logical;
      std::uint64_t generation;
      std::uint64_t revision;
      world_type world;
    };
    using snapshot_type = std::shared_ptr<publication const>;

  private:
    struct request;
    struct identity {};

  public:
    struct ticket {
      ticket() = default;
      bool valid() const noexcept { return result_.valid(); }
      bool ready() const { return result_.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }
      void wait() const { result_.wait(); }
      snapshot_type get() const { return result_.get(); }
    private:
      friend struct session;
      std::shared_ptr<identity const> owner_;
      std::weak_ptr<request> request_;
      std::shared_future<snapshot_type> result_;
    };

    explicit session(Engine engine, session_limits limits)
      : engine_(std::make_unique<Engine>(std::move(engine))), limits_(limits) {
      if (!limits_.contributions || !limits_.maintenance_budget)
        throw std::invalid_argument("session needs a positive contribution limit and maintenance budget");
      auto first = std::make_shared<publication const>(publication{
        std::make_shared<logical_state const>(), 0, 0, engine_->snapshot()});
      std::atomic_store_explicit(&current_, std::move(first), std::memory_order_release);
      auto pending = engine_->pending();
      worker_ = std::thread([this, pending] { run(pending); });
    }
    session(session const &) = delete;
    session & operator=(session const &) = delete;
    session(session &&) = delete;
    session & operator=(session &&) = delete;
    ~session() { shutdown(); }

    snapshot_type snapshot() const noexcept { return std::atomic_load_explicit(&current_, std::memory_order_acquire); }

    // A saturated try_submit neither copies nor moves input. Blocking submit
    // retains no private input copy while waiting. Caller-owned waiting inputs
    // lie outside the accepted-input reservation bound.
    // An Engine may adapt a public command to its queue envelope without
    // taking ownership yet. The ordinary enqueue path still reserves first.
    template <class C> requires (!std::same_as<std::remove_cvref_t<C>, contribution_type>) &&
      requires(C && input) { { Engine::borrow_contribution(std::forward<C>(input)) } -> std::same_as<contribution_type>; }
    std::optional<ticket> try_submit(C && input) {
      return try_submit(Engine::borrow_contribution(std::forward<C>(input)));
    }
    template <class C> requires (!std::same_as<std::remove_cvref_t<C>, contribution_type>) &&
      requires(C && input) { { Engine::borrow_contribution(std::forward<C>(input)) } -> std::same_as<contribution_type>; }
    ticket submit(C && input) { return submit(Engine::borrow_contribution(std::forward<C>(input))); }
    template <class C> requires (!std::same_as<std::remove_cvref_t<C>, contribution_type>) &&
      requires(C && input) { { Engine::borrow_contribution(std::forward<C>(input)) } -> std::same_as<contribution_type>; }
    snapshot_type apply(C && input) { return submit(std::forward<C>(input)).get(); }

    template <class C> requires std::same_as<std::remove_cvref_t<C>, contribution_type>
    std::optional<ticket> try_submit(C && input) {
      return enqueue(std::forward<C>(input), false);
    }
    template <class C> requires std::same_as<std::remove_cvref_t<C>, contribution_type>
    ticket submit(C && input) {
      if (worker_session_ == this)
        throw std::logic_error("blocking submission from the session worker");
      return *enqueue(std::forward<C>(input), true);
    }
    template <class C> requires std::same_as<std::remove_cvref_t<C>, contribution_type>
    snapshot_type apply(C && input) { return submit(std::forward<C>(input)).get(); }

    // Only an unclaimed queue entry can be cancelled. The ticket remains valid
    // and resolves with session_cancelled. Waiting, dropping or timing out a ticket
    // does not cancel it. Other sessions' tickets cannot cancel this queue.
    bool cancel(ticket const & value) {
      if (value.owner_ != identity_) return false;
      auto item = value.request_.lock();
      if (!item) return false;
      {
        std::lock_guard lock(mutex_);
        auto found = queue_.begin();
        while (found != queue_.end() && *found != item) ++found;
        if (found == queue_.end()) return false;
        queue_.erase(found);
      }
      item->input.reset();
      {
        std::lock_guard lock(mutex_);
        item->result.set_exception(std::make_exception_ptr(session_cancelled{}));
        unreserve(item->charge);
      }
      changed_.notify_all();
      return true;
    }

    session_reservation outstanding() const {
      std::lock_guard lock(mutex_);
      return reserved_;
    }
    std::uint64_t pending_count() const {
      std::lock_guard lock(mutex_);
      return count_;
    }
    std::exception_ptr failure() const {
      std::lock_guard lock(mutex_);
      return failure_;
    }

    // close rejects new admissions, wakes blocked submitters and drains the
    // accepted queue asynchronously. shutdown additionally joins the worker;
    // it is idempotent and concurrent shutdown calls serialize their joins.
    void close() {
      {
        std::lock_guard lock(mutex_);
        closing_ = true;
      }
      changed_.notify_all();
    }
    void shutdown() {
      if (worker_session_ == this)
        throw std::logic_error("joining the session worker from itself");
      close();
      std::lock_guard lock(join_mutex_);
      if (worker_.joinable()) worker_.join();
    }

  private:
    struct request {
      std::optional<contribution_type> input;
      session_reservation charge;
      std::promise<snapshot_type> result;
      template <class C> request(C && value, session_reservation reservation)
        : input(std::in_place, std::forward<C>(value)), charge(reservation) {}
    };

    bool fits(session_reservation charge) const noexcept {
      return count_ < limits_.contributions && charge.work <= limits_.work - reserved_.work &&
        charge.bytes <= limits_.bytes - reserved_.bytes;
    }
    void available() const {
      if (failure_) std::rethrow_exception(failure_);
      if (closing_) throw session_closed{};
    }
    template <class C> std::optional<ticket> enqueue(C && input, bool wait) {
      auto charge = Engine::reservation(input);
      if (charge.work > limits_.work || charge.bytes > limits_.bytes)
        throw std::length_error("contribution exceeds a session admission limit");
      // Reserve before constructing the private input. Copy/move constructors
      // may run arbitrary code, so reserve under the lock and construct outside.
      {
        std::unique_lock lock(mutex_);
        available();
        if (wait) changed_.wait(lock, [&] { return closing_ || failure_ || fits(charge); });
        available();
        if (!fits(charge)) return std::nullopt;
        reserved_.work += charge.work;
        reserved_.bytes += charge.bytes;
        ++count_;
      }
      std::shared_ptr<request> item;
      ticket result;
      try {
        item = std::make_shared<request>(std::forward<C>(input), charge);
        result.owner_ = identity_;
        result.request_ = item;
        result.result_ = item->result.get_future().share();
        {
          std::lock_guard lock(mutex_);
          // close drains this reservation too; a failure cannot accept a new
          // request after the failed worker has already emptied its queue.
          if (failure_) std::rethrow_exception(failure_);
          queue_.push_back(item);
        }
      } catch (...) {
        item.reset();
        {
          std::lock_guard lock(mutex_);
          unreserve(charge);
        }
        changed_.notify_all();
        throw;
      }
      changed_.notify_all();
      return result;
    }
    void unreserve(session_reservation charge) noexcept {
      reserved_.work -= charge.work;
      reserved_.bytes -= charge.bytes;
      --count_;
    }
    snapshot_type next(world_type world, bool contribution) const {
      auto old = snapshot();
      if (old->revision == std::numeric_limits<std::uint64_t>::max() ||
          (contribution && old->generation == std::numeric_limits<std::uint64_t>::max()))
        throw std::length_error("session publication sequence exhausted");
      return std::make_shared<publication const>(publication{
        contribution ? std::make_shared<logical_state const>() : old->logical,
        old->generation + std::uint64_t(contribution), old->revision + 1, std::move(world)});
    }
    void failed(std::shared_ptr<request> const & active, std::exception_ptr error) noexcept {
      std::list<std::shared_ptr<request>> abandoned;
      {
        std::lock_guard lock(mutex_);
        failure_ = error;
        closing_ = true;
        abandoned.swap(queue_);
      }
      // Destroy failed continuations before refunding their input reservation.
      engine_.reset();
      auto reject = [&](std::shared_ptr<request> const & item) {
        item->input.reset();
        std::lock_guard lock(mutex_);
        item->result.set_exception(error);
        unreserve(item->charge);
      };
      if (active) reject(active);
      for (auto const & item : abandoned) reject(item);
      changed_.notify_all();
      // Input constructors and a simultaneous cancellation run outside the
      // lock. Their reservations must also settle before shutdown can join.
      std::unique_lock lock(mutex_);
      changed_.wait(lock, [&] { return count_ == 0; });
    }
    void run(bool pending) noexcept {
      struct worker_scope {
        session const * previous = worker_session_;
        explicit worker_scope(session const * self) { worker_session_ = self; }
        ~worker_scope() { worker_session_ = previous; }
      } scope(this);
      while (true) {
        std::shared_ptr<request> item;
        try {
          bool ready = true;
          if constexpr (requires (Engine const & value) { { value.admission_ready() } -> std::convertible_to<bool>; }) {
            ready = engine_->admission_ready();
            if (!ready && !pending) throw std::logic_error("Engine refuses admission without pending service");
          }
          {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [&] { return !queue_.empty() || (closing_ ? count_ == 0 : pending); });
            if (!queue_.empty() && ready) { item = std::move(queue_.front()); queue_.pop_front(); }
            else if (closing_ && count_ == 0) break;
          }
          if (item) {
            std::optional<world_type> produced;
            try { produced.emplace(engine_->contribute(std::move(*item->input))); }
            catch (...) {
              // This optional contract certifies that a rejected contribution
              // did not change logical state. Do not apply it to failures in
              // next(), durability publication, or other work after contribute.
              if constexpr (std::is_nothrow_move_constructible_v<world_type> &&
                  requires (Engine const & value) { { value.failed() } noexcept -> std::same_as<bool>; }) {
                if (!engine_->failed()) {
                  auto error = std::current_exception();
                  pending = engine_->pending();
                  item->input.reset();
                  {
                    std::lock_guard lock(mutex_);
                    item->result.set_exception(error); unreserve(item->charge);
                  }
                  changed_.notify_all();
                  continue;
                }
              }
              throw;
            }
            auto candidate = next(std::move(*produced), true);
            item->input.reset();
            pending = engine_->pending();
            std::atomic_store_explicit(&current_, candidate, std::memory_order_release);
            {
              std::lock_guard lock(mutex_);
              item->result.set_value(std::move(candidate));
              unreserve(item->charge);
            }
            changed_.notify_all();
          } else {
            auto candidate = engine_->advance(limits_.maintenance_budget);
            if (candidate) std::atomic_store_explicit(&current_, next(std::move(*candidate), false), std::memory_order_release);
            pending = engine_->pending();
          }
        } catch (...) {
          failed(item, std::current_exception());
          return;
        }
      }
      engine_.reset();
    }

    std::unique_ptr<Engine> engine_;
    session_limits limits_;
    std::shared_ptr<identity const> identity_ = std::make_shared<identity const>();
    snapshot_type current_;
    mutable std::mutex mutex_;
    std::mutex join_mutex_;
    std::condition_variable changed_;
    std::list<std::shared_ptr<request>> queue_;
    session_reservation reserved_;
    std::uint64_t count_ = 0;
    bool closing_ = false;
    std::exception_ptr failure_;
    std::thread worker_;
    inline static thread_local session const * worker_session_ = nullptr;
  };
}
