/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <filesystem>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace diet {
  // The immutable file layer owns data and fractional indexes. Cola roots,
  // pins and merge progress belong to the separate metadata backend.
  enum struct file_kind { native_blob, fractional_index };

  inline std::string_view file_extension(file_kind kind) {
    switch (kind) {
      case file_kind::native_blob: return ".kv";
      case file_kind::fractional_index: return ".index";
    }
    throw std::invalid_argument("unsupported Diet file kind");
  }

  // Canonical representation of a physical 128-bit object identity. The
  // allocator supplies well-distributed IDs; this type validates spelling,
  // not entropy or uniqueness. Logical keys never enter the path builder.
  struct object_id {
    explicit object_id(std::string hex) : hex_(std::move(hex)) {
      if (hex_.size() != 32) throw std::invalid_argument("object ID must contain 32 lowercase hex digits");
      for (char c : hex_)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
          throw std::invalid_argument("noncanonical object ID");
    }
    static object_id from_hex(std::string_view hex) { return object_id(std::string(hex)); }
    std::string const & hex() const noexcept { return hex_; }
    bool operator==(object_id const &) const = default;
  private:
    std::string hex_;
  };

  inline std::filesystem::path object_path(object_id const & id, file_kind kind) {
    auto const & hex = id.hex();
    return std::filesystem::path(hex.substr(0, 2)) / hex.substr(2, 2) /
           (hex.substr(4) + std::string(file_extension(kind)));
  }

  struct parsed_object_path {
    object_id id;
    file_kind kind;
  };

  // Parse the canonical generic relative spelling before filesystem lexical
  // normalization: ab/cd/<remaining 28 hex digits>.kv (or .index).
  inline parsed_object_path parse_object_path(std::string_view path) {
    if (path.size() < 37 || path.size() > 40 || path[2] != '/' || path[5] != '/' || path[34] != '.')
      throw std::invalid_argument("noncanonical sharded object path");
    auto id = object_id(std::string(path.substr(0, 2)) + std::string(path.substr(3, 2)) +
                        std::string(path.substr(6, 28)));
    auto extension = path.substr(34);
    for (auto kind : {file_kind::native_blob, file_kind::fractional_index})
      if (extension == file_extension(kind)) return {std::move(id), kind};
    throw std::invalid_argument("unsupported Diet object extension");
  }
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Diet's object path support.
 */
