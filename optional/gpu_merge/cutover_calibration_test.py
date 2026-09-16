#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Small algorithm checks; no GPU, subprocesses, or measurements are needed."""
import unittest
from calibrate_cutover import features, fit, predict


class CalibrationTest(unittest.TestCase):
    def test_features_use_headers_only(self):
        row = {"a_records": "512", "b_records": "512", "a_payload_bits": "8192", "b_payload_bits": "8192",
               "a_file_bytes": "1200", "b_file_bytes": "1200", "a_common_value_bits": "8", "b_common_value_bits": "8",
               "a_terminal_key_bits": "65", "b_terminal_key_bits": "65", "prefix": "999", "overlap": "100"}
        self.assertEqual(features(row), [1024, 2048, 2400, 2, 1, 1, 8, 1, 2])
        row["prefix"] = "0"
        row["overlap"] = "0"
        self.assertEqual(features(row), [1024, 2048, 2400, 2, 1, 1, 8, 1, 2])
        row["a_common_value_bits"] = "-1"
        row["b_common_value_bits"] = "-1"
        self.assertEqual(features(row)[-2:], [0, 0])

    def test_minimum_two_robust_cases(self):
        x = [100] * 9
        lone = [{"case": 0, "features": x, "robust_gpu": True}]
        self.assertFalse(predict(fit(lone), x))
        two = lone + [{"case": 1, "features": x, "robust_gpu": True}]
        self.assertTrue(predict(fit(two), x))
        two[1]["robust_gpu"] = False
        self.assertFalse(predict(fit(two), x))

    def test_frozen_tree_and_unobservable_misses(self):
        cases = [{"case": i, "features": [n] + [1] * 8, "robust_gpu": win}
                 for i, (n, win) in enumerate([(100, False), (200, False), (1000, True), (2000, True)])]
        tree = fit(cases)
        self.assertFalse(predict(tree, [300] + [1] * 8))
        self.assertTrue(predict(tree, [1500] + [1] * 8))
        frozen = repr(tree)
        # A bad held-out result is evaluated separately; it never modifies the
        # training tree or retries a more favorable threshold.
        self.assertTrue(predict(tree, [1200] + [1] * 8))
        self.assertEqual(repr(tree), frozen)
        identical = [{"case": i, "features": [1] * 9, "robust_gpu": i % 2 == 0} for i in range(6)]
        self.assertFalse(predict(fit(identical), [1] * 9))


if __name__ == "__main__":
    unittest.main()
