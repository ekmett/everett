/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/front.h>
#include <everett/rank15.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace everett {
  enum class borrowed_prefix_policy {
    ordinary,
    bidirectional,
    shared_boundaries
  };

  struct blob_record {
    std::string key;
    std::optional<std::uint64_t> value;
  };

  inline std::array<std::byte, 9> encode_blob_value(std::optional<std::uint64_t> value) noexcept {
    std::array<std::byte, 9> bytes{};
    bytes[0] = value ? std::byte{1} : std::byte{0};
    auto bits = value.value_or(0);
    for (unsigned i = 0; i != 8; ++i) bytes[i + 1] = static_cast<std::byte>(bits >> (8 * i));
    return bytes;
  }

  inline std::optional<std::uint64_t> decode_blob_value(std::span<std::byte const, 9> bytes) {
    if (bytes[0] == std::byte{0}) return std::nullopt;
    if (bytes[0] != std::byte{1}) throw std::invalid_argument("invalid blob value tag");
    std::uint64_t value = 0;
    for (unsigned i = 0; i != 8; ++i) {
      value |= std::uint64_t(std::to_integer<unsigned>(bytes[i + 1])) << (8 * i);
    }
    return value;
  }

  struct blob_window {
    std::uint64_t native_first = 0;
    std::uint64_t native_last = 0;
    std::uint64_t borrowed_first = 0;
    std::uint64_t borrowed_last = 0;
  };

  struct blob_native_match {
    std::uint64_t ordinal = 0;
    std::optional<std::uint64_t> value;
  };

  struct blob_borrowed_predecessor {
    std::uint64_t ordinal = 0;
    // Samples are taken at virtual target ordinals 0, 15, 30, ... .
    std::uint64_t target_ordinal = 0;
    // In ordinary mode an earlier predecessor has only a known ordinal;
    // its decoding context needs a bridge supplied by the caller. Conservative
    // policies also reconstruct that predecessor from the shared boundary.
    bool has_context = false;
    std::string prefix;
    std::uint64_t full_size = 0;
    bool false_borrow = false;
  };

  struct blob_window_result {
    std::optional<blob_native_match> native;
    std::optional<blob_borrowed_predecessor> borrowed_predecessor;
  };

  // A bounded immutable slice: separate native and borrowed front-coded byte
  // streams, two select15 indexes and one rank15 virtual interleave. Mapping,
  // target identity/pins and the outer redundant-level scheduler live above it.
  struct blob {
    static blob build(std::span<blob_record const> native,
                      std::span<std::string const> borrowed = {},
                      std::uint64_t native_restart_factor = 18,
                      std::span<std::uint64_t const> borrowed_prefix_ceilings = {},
                      borrowed_prefix_policy policy = borrowed_prefix_policy::shared_boundaries) {
      if (native.size() > std::numeric_limits<std::uint64_t>::max() - borrowed.size()) {
        throw std::length_error("blob virtual count overflows");
      }
      blob result;
      std::vector<front_record<9>> native_records;
      native_records.reserve(native.size());
      for (std::size_t i = 0; i != native.size(); ++i) {
        if (i && compare_keys(native[i - 1].key, native[i].key) >= 0) {
          throw std::invalid_argument("blob native keys must be strictly sorted");
        }
        native_records.push_back({native[i].key, encode_blob_value(native[i].value)});
      }
      result.native_ = std::make_shared<front_array<9> const>(
        front_array<9>::build(native_records, {}, native_restart_factor));
      build_index(result, native, borrowed, borrowed_prefix_ceilings, policy);
      return result;
    }

    // Repairing a downstream index never changes native front coding or its
    // select15 offsets. This initial builder materializes native keys as scratch;
    // a streaming builder can remove that peak without changing the representation.
    blob reindex(std::span<std::string const> borrowed,
                 std::span<std::uint64_t const> borrowed_prefix_ceilings = {},
                 borrowed_prefix_policy policy = borrowed_prefix_policy::shared_boundaries) const {
      std::vector<blob_record> native_records;
      native_records.reserve(static_cast<std::size_t>(native_->size()));
      native_->view().visit_all([&](front_item<9> item) {
        native_records.push_back({std::string(item.key.prefix), decode_blob_value(item.value)});
        return true;
      });
      if (native_records.size() > std::numeric_limits<std::uint64_t>::max() - borrowed.size()) {
        throw std::length_error("blob virtual count overflows");
      }
      blob result;
      result.native_ = native_;
      build_index(result, native_records, borrowed, borrowed_prefix_ceilings, policy);
      return result;
    }

  private:
    static void build_index(blob & result, std::span<blob_record const> native,
                            std::span<std::string const> borrowed,
                            std::span<std::uint64_t const> borrowed_prefix_ceilings,
                            borrowed_prefix_policy policy) {
      if (!borrowed_prefix_ceilings.empty() && borrowed_prefix_ceilings.size() != borrowed.size()) {
        throw std::invalid_argument("one borrowed prefix ceiling is required per record");
      }
      if (policy != borrowed_prefix_policy::ordinary &&
          policy != borrowed_prefix_policy::bidirectional &&
          policy != borrowed_prefix_policy::shared_boundaries) {
        throw std::invalid_argument("invalid borrowed prefix policy");
      }
      std::vector<std::uint64_t> ceilings;
      if (policy == borrowed_prefix_policy::shared_boundaries) {
        ceilings.assign(borrowed.size(), std::numeric_limits<std::uint64_t>::max());
        if (!borrowed_prefix_ceilings.empty()) {
          std::copy(borrowed_prefix_ceilings.begin(), borrowed_prefix_ceilings.end(), ceilings.begin());
        }
        borrowed_prefix_ceilings = ceilings;
      } else if (policy == borrowed_prefix_policy::bidirectional) {
        // shared[i] <= min(LCP(S[i-1], S[i]), LCP(S[i], S[i+1]))
        // lets S[i] decode from an anchor on either side of it. The final
        // sample is literal because its upper interval is unbounded.
        // Relative to ordinary FC, extra suffix bytes are the positive drops
        // in the adjacent-LCP sequence (zero at both ends). Their sum equals
        // its positive rises, each <= that record's ordinary suffix length.
        // Thus suffix bytes at most double. Framing is charged separately;
        // arbitrary caller ceilings can add further redundancy.
        ceilings.reserve(borrowed.size());
        for (std::size_t i = 0; i != borrowed.size(); ++i) {
          auto ceiling = i + 1 == borrowed.size() ? 0 : common_prefix(borrowed[i], borrowed[i + 1]);
          if (!borrowed_prefix_ceilings.empty()) {
            ceiling = std::min<std::uint64_t>(ceiling, borrowed_prefix_ceilings[i]);
          }
          ceilings.push_back(ceiling);
        }
        borrowed_prefix_ceilings = ceilings;
      }
      result.borrowed_policy_ = policy;
      std::vector<front_record<0>> borrowed_records;
      borrowed_records.reserve(borrowed.size());
      result.false_borrows_.resize(borrowed.size() / 8 + (borrowed.size() % 8 != 0));
      std::size_t native_at = 0;
      for (std::size_t i = 0; i != borrowed.size(); ++i) {
        if (i && compare_keys(borrowed[i - 1], borrowed[i]) > 0) {
          throw std::invalid_argument("blob borrowed keys must be sorted");
        }
        while (native_at != native.size() && compare_keys(native[native_at].key, borrowed[i]) < 0) {
          ++native_at;
        }
        if (native_at != native.size() && compare_keys(native[native_at].key, borrowed[i]) == 0) {
          result.false_borrows_[i / 8] |= static_cast<std::byte>(1u << (i % 8));
        }
        borrowed_records.push_back({borrowed[i], {}});
      }
      auto count = std::uint64_t(native.size()) + borrowed.size();
      std::vector<std::uint8_t> classes(static_cast<std::size_t>(count / 15 + (count % 15 != 0)));
      std::size_t a = 0;
      std::size_t s = 0;
      for (std::uint64_t i = 0; i != count; ++i) {
        // Native precedes borrowed on equality, including multiple borrowed
        // copies. false_borrow then identifies a possibly earlier native slot.
        auto take_borrowed = s != borrowed.size() &&
          (a == native.size() || compare_keys(borrowed[s], native[a].key) < 0);
        if (policy == borrowed_prefix_policy::shared_boundaries && i % 15 == 0 && s) {
          // rank15(i / 15) == s, so S[s-1] is the predecessor needed by
          // this window. It must decode from the known boundary key C[i].
          // The rightmost applicable boundary has the smallest LCP; taking
          // the minimum here imposes precisely that constraint. If no cut
          // uses a record, its ordinary copy count is left unchanged.
          auto const & boundary = take_borrowed ? borrowed[s] : native[a].key;
          ceilings[s - 1] = std::min<std::uint64_t>(
            ceilings[s - 1], common_prefix(borrowed[s - 1], boundary));
        }
        if (take_borrowed) {
          ++classes[static_cast<std::size_t>(i / 15)];
          ++s;
        } else {
          ++a;
        }
      }
      result.borrowed_ = front_array<0>::build(borrowed_records, borrowed_prefix_ceilings);
      result.interleave_ = rank15_index::build(classes, count);
      result.virtual_count_ = count;
    }

  public:
    front_array<9> const & native() const noexcept { return *native_; }
    front_array<0> const & borrowed() const noexcept { return borrowed_; }
    rank15_index const & interleave() const noexcept { return interleave_; }
    std::span<std::byte const> false_borrow_bits() const noexcept { return false_borrows_; }
    std::uint64_t virtual_size() const noexcept { return virtual_count_; }
    borrowed_prefix_policy borrowed_policy() const noexcept { return borrowed_policy_; }
    std::uint64_t group_count() const noexcept {
      return virtual_count_ / 15 + (virtual_count_ % 15 != 0);
    }

    bool false_borrow(std::uint64_t ordinal) const {
      if (ordinal >= borrowed_.size()) throw std::out_of_range("blob borrowed ordinal");
      return (std::to_integer<unsigned>(false_borrows_[ordinal / 8]) >> (ordinal % 8)) & 1;
    }

    blob_window project(std::uint64_t group) const {
      if (group >= group_count()) throw std::out_of_range("blob virtual group");
      auto first = group * 15;
      auto last = first + std::min<std::uint64_t>(15, virtual_count_ - first);
      auto ranks = interleave_.view();
      auto a = ranks.rank(group);
      // The stored population is exactly the difference of the two ranks.
      auto b = a + ranks.class_at(group);
      return {first - a, last - b, a, b};
    }

    // The caller provides a target window selected by the incoming sample and
    // the corresponding known lower key (or empty context at ordinal zero).
    // For full lookup this must be the window containing the last virtual key
    // <= query. This method does not search a large first catalog unaided.
    blob_window_result search_window(std::string_view query, std::uint64_t group,
                                     front_anchor lower) const {
      auto window = project(group);
      blob_window_result result;
      native_->view().visit_window(window.native_first, window.native_last, lower, query.size(),
        [&](front_item<9> item) {
          auto order = compare_prefix(item.key, query);
          if (!order) result.native = blob_native_match{item.ordinal, decode_blob_value(item.value)};
          return order < 0;
        });

      auto consider_sample = [&](front_item<0> item) {
        auto order = compare_prefix(item.key, query);
        if (order > 0) return false;
        auto is_false = false_borrow(item.ordinal);
        result.borrowed_predecessor = blob_borrowed_predecessor{
          item.ordinal, checked_target_ordinal(item.ordinal), true,
          std::string(item.key.prefix), item.key.full_size, is_false};
        if (!order && is_false && !result.native) {
          if (!window.native_first) {
            throw std::invalid_argument("false borrow has no native predecessor");
          }
          // Native-before-borrowed tie order and the builder's equality bit
          // prove this exact slot matches query, even before this window.
          auto ordinal = window.native_first - 1;
          result.native = blob_native_match{
            ordinal, decode_blob_value(native_->view().encoded_at(ordinal).value)};
        }
        return true;
      };
      if (window.borrowed_first) {
        auto ordinal = window.borrowed_first - 1;
        if (borrowed_policy_ != borrowed_prefix_policy::ordinary) {
          // Both conservative policies clamp S[j]'s copied prefix to one
          // shared with this upper boundary anchor. No backward walk is needed.
          borrowed_.view().visit_window(ordinal, ordinal + 1, lower, query.size(), consider_sample);
        } else {
          result.borrowed_predecessor = blob_borrowed_predecessor{
            ordinal, checked_target_ordinal(ordinal), false, {}, 0, false_borrow(ordinal)};
        }
      }
      borrowed_.view().visit_window(window.borrowed_first, window.borrowed_last, lower,
                                   query.size(), consider_sample);
      return result;
    }

  private:
    std::shared_ptr<front_array<9> const> native_ = std::make_shared<front_array<9> const>();
    front_array<0> borrowed_;
    rank15_index interleave_ = rank15_index::build({}, 0);
    std::vector<std::byte> false_borrows_;
    std::uint64_t virtual_count_ = 0;
    borrowed_prefix_policy borrowed_policy_ = borrowed_prefix_policy::shared_boundaries;

    static std::uint64_t checked_target_ordinal(std::uint64_t ordinal) {
      if (ordinal > std::numeric_limits<std::uint64_t>::max() / 15) {
        throw std::overflow_error("borrowed target ordinal overflows");
      }
      return ordinal * 15;
    }
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Everett's blob support.
 */
