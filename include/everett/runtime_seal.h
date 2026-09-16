/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Describes completed native and index objects retained by runtime owners.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <everett/catalog_bindings.h>
#include <everett/object_writer.h>
#include <everett/sections.h>

namespace everett {
  struct native_seal {
    object_id catalog;
    object_seal_receipt receipt;
  };

  struct pair_seal {
    object_id catalog;
    blob_identity identity;
    object_seal_receipt receipt; // The index; identity.native names its exact native owner.
  };

  template <class Mapped> struct native_binding : native_seal {
    std::shared_ptr<Mapped const> mapped;
    native_binding(native_seal seal, std::shared_ptr<Mapped const> value)
      : native_seal(std::move(seal)), mapped(std::move(value)) {}
  };

  template <class Mapped> struct pair_binding : pair_seal {
    std::shared_ptr<Mapped const> mapped;
    pair_binding(pair_seal seal, std::shared_ptr<Mapped const> value)
      : pair_seal(std::move(seal)), mapped(std::move(value)) {}
  };

  template <class P, class Ids, class Ops, class Family> struct runtime_store;
  template <class P, class Selector, class Ids, class CatalogOps, class FileOps> struct sort_runtime_context;
  namespace runtime_store_detail {
    template <class P, class Ids, class Ops, class Family> struct graph_sealer;
  }
}
