/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Persists sort-owned native files behind the redundant runtime's exact checkpoints.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <diet/sort_runtime.h>
#include <diet/redundant_checkpoint.h>
#include <diet/runtime_store.h>

namespace diet {
  template <class P, class Selector> struct runtime_storage_codec<sort_runtime_family<P, Selector>>
    : runtime_storage_codec<redundant_runtime_family<P, sort_runtime_storage<P, Selector>>> {};

  template <class P = string_policy, class Selector = registry_selector<typename P::registry_type>,
    class Ids = random_object_ids, class Ops = sqlite_catalog_ops>
  using sort_runtime_store = runtime_store<P, Ids, Ops, sort_runtime_family<P, Selector>>;
}
