/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Outlines exceptional check failures while preserving their types and messages.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <stdexcept>

namespace diet::error_detail {
  // Keep construction and unwinding for a failed check out of its hot caller.
  // E and the supplied message retain the call site's exception contract.
  template <class E> [[noreturn]]
#if defined(_MSC_VER)
  __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
  [[gnu::cold, gnu::noinline]]
#endif
  inline void raise(char const * message) { throw E(message); }
}
