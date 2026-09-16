/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Shares an allowance for encoded outputs retained in memory.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace everett {
  // This accounts for caller-declared allocation capacities. It does not
  // measure allocations or bound cursor workspace and user callbacks. Builders
  // acquire before retaining output; finished owners keep the lease until the
  // last reference is gone, possibly on a different thread from the builder.
  struct output_budget {
  private:
    struct state {
      std::size_t const limit;
      std::atomic<std::size_t> used{0};
      explicit state(std::size_t bytes) noexcept : limit(bytes) {}
    };
  public:
    struct lease {
      lease() = default;
      lease(lease const &) = delete;
      lease & operator=(lease const &) = delete;
      lease(lease && other) noexcept
        : state_(std::move(other.state_)), bytes_(std::exchange(other.bytes_, 0)) {}
      lease & operator=(lease && other) noexcept {
        if (this != &other) {
          reset(); state_ = std::move(other.state_); bytes_ = std::exchange(other.bytes_, 0);
        }
        return *this;
      }
      ~lease() { reset(); }
      std::size_t bytes() const noexcept { return bytes_; }
      void reset() noexcept {
        if (state_) state_->used.fetch_sub(bytes_, std::memory_order_relaxed);
        bytes_ = 0; state_.reset();
      }
      // Finalization can return conservative staging slack without briefly
      // releasing the allowance covering the finished object's allocations.
      void shrink(std::size_t bytes) {
        if (bytes > bytes_) throw std::invalid_argument("output lease cannot grow");
        if (state_) state_->used.fetch_sub(bytes_ - bytes, std::memory_order_relaxed);
        bytes_ = bytes;
      }
    private:
      friend struct output_budget;
      std::shared_ptr<state> state_;
      std::size_t bytes_ = 0;
      lease(std::shared_ptr<state> value, std::size_t bytes) noexcept
        : state_(std::move(value)), bytes_(bytes) {}
    };

    explicit output_budget(std::size_t limit) : state_(std::make_shared<state>(limit)) {}
    // Copies deliberately share one allowance, including across context owners.
    std::size_t limit() const noexcept { return state_->limit; }
    std::size_t used() const noexcept { return state_->used.load(std::memory_order_relaxed); }
    std::optional<lease> try_acquire(std::size_t bytes) const noexcept {
      auto used = state_->used.load(std::memory_order_relaxed);
      for (;;) {
        if (bytes > state_->limit - used) return std::nullopt;
        if (state_->used.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed))
          return lease(state_, bytes);
      }
    }

    // The alias owns both value and allowance without changing the array's
    // representation. Destroy the value before returning its charge. If the
    // holder allocation or value construction throws, the lease is returned.
    template <class T> static std::shared_ptr<T const> attach(T value, lease allocation) {
      auto owner = std::make_shared<retained<T>>(std::move(allocation), std::move(value));
      auto address = &owner->value;
      return std::shared_ptr<T const>(std::move(owner), address);
    }
  private:
    template <class T> struct retained {
      lease allocation;
      T value;
      retained(lease && charge, T && item) : allocation(std::move(charge)), value(std::move(item)) {}
    };
    std::shared_ptr<state> state_;
  };
}
