#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Offline boundary tests for the generated Unicode property range encoding."""

import unittest

from genUnicodeTable import pack_unicode_ranges


class UnicodeRangePackingTest(unittest.TestCase):
    def test_empty(self):
        self.assertEqual(pack_unicode_ranges([]), ([], []))

    def test_short_ranges(self):
        self.assertEqual(
            pack_unicode_ranges([(0, 0), (0x41, 0x41), (0x1000, 0x17FE)]),
            ([0, 0x20800, 0x8007FE], []),
        )

    def test_overflow_boundary_and_indices(self):
        self.assertEqual(
            pack_unicode_ranges([(0x1000, 0x17FF), (0, 0), (0, 0x10FFFF)]),
            ([0x7FF, 0, 0xFFF], [(0x1000, 0x17FF), (0, 0x10FFFF)]),
        )

    def test_highest_code_point(self):
        self.assertEqual(
            pack_unicode_ranges([(0x10FFFF, 0x10FFFF)]), ([0x87FFF800], [])
        )

    def test_invalid_endpoints(self):
        for pair in [(-1, 0), (1, 0), (0, 0x110000)]:
            with self.subTest(pair=pair), self.assertRaises(ValueError):
                pack_unicode_ranges([pair])

    def test_iterator_input(self):
        self.assertEqual(pack_unicode_ranges(iter([(0, 0)])), ([0], []))


if __name__ == "__main__":
    unittest.main()
