/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <diet/error_detail.h>

#include <diet/index_pipeline_detail.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace diet {
  // Builds owning indexes through the same bounded sample queues as file output.
  // Native stages are ordered nearest the target first, new head last. Finalizing
  // builds compact navigation and binds each completed pair to its exact target.
  template <class P>
  struct index_pipeline : index_detail::pipeline_driver<P, profile_blob<P>, index_builder<P>> {
    using policy_type = P;
    using blob_type = profile_blob<P>;
    using pair_type = std::shared_ptr<blob_type const>;

    index_pipeline(pair_type target, std::vector<pair_type> native_stages)
      : base(std::move(target), make_stages(native_stages)), results_(this->stages_.size()) {}

    pair_type finish() {
      if (this->failed_) error_detail::raise<std::logic_error>("index pipeline has failed");
      if (!this->done()) error_detail::raise<std::logic_error>("index pipeline still has input");
      if (!this->finished_) {
        try {
          auto target = this->target_;
          for (std::size_t i = 0; i != this->stages_.size(); ++i) {
            results_[i] = std::make_shared<blob_type const>(this->stages_[i]->finish(target));
            target = results_[i];
          }
          this->finished_ = true;
        } catch (...) {
          this->failed_ = true;
          throw;
        }
      }
      return results_.empty() ? this->target_ : results_.back();
    }

  private:
    using base = index_detail::pipeline_driver<P, blob_type, index_builder<P>>;
    std::vector<pair_type> results_;

    static std::vector<std::unique_ptr<index_builder<P>>> make_stages(std::span<pair_type const> sources) {
      std::vector<std::unique_ptr<index_builder<P>>> result;
      result.reserve(sources.size());
      for (auto const & source : sources) {
        if (!source) error_detail::raise<std::invalid_argument>("null pipeline native source");
        result.push_back(std::make_unique<index_builder<P>>(*source));
      }
      return result;
    }
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Builds fractional-index chains through bounded streaming queues.
 */
