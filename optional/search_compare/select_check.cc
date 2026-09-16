/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Verifies generated selection overlays against their integer source.
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#include <everett/elias_fano.h>
#include <random>
#include <iostream>

int main() {
  std::mt19937_64 random(917);
  std::uint64_t selections = 0;
  for (unsigned shape = 0; shape < 5; ++shape) for (unsigned count : {0, 1, 31, 32, 33, 255, 256, 257, 4096, 16385}) {
    std::vector<std::uint64_t> source;
    std::uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) {
      value += shape == 0 ? 0 : shape == 1 ? random() % 8 : shape == 2 ? random() % 8192 :
        shape == 3 ? (i == 128 ? 1u << 28 : 0) : (i % 257 == 0 ? 65536 : 1);
      source.push_back(value);
    }
    auto encoded = everett::elias_fano::build(source);
    auto view = encoded.view(); auto cursor = view.cursor();
    for (unsigned i = 0; i < count; ++i) {
      if (view.select(i) != source[i] || cursor.next() != source[i]) return 1;
      ++selections;
    }
    if (!cursor.done()) return 2;
    for (unsigned i = 0; i < count; ++i) {
      auto ordinal = random() % count;
      if (view.select(ordinal) != source[ordinal]) return 3;
      ++selections;
    }
  }
  std::cout << selections << " oracle selections passed\n";
}
