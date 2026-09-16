#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Independent scalar checks for collision-rank merge ordinals and layouts.

The 512-bit layout is specified by rank512.hlsl. This model is not a shader
execution test. It also models Everett's separate 2048/512 layout.
Both are temporary construction scratch for collision -> survivor positions;
neither changes the final rank15 format or its CPU query implementation.
"""

from bisect import bisect_left
from itertools import accumulate
import random


def bitmap(flags):
    words = [0] * ((len(flags) + 31) // 32)
    for i, flag in enumerate(flags):
        words[i // 32] |= int(flag) << (i % 32)
    return words


def rank512_index(flags):
    words = bitmap(flags)
    directory = []
    total = 0
    for first in range(0, len(words), 16):
        relative, local = 0, 0
        for pair in range(8):
            if pair:
                relative |= local << ((pair - 1) * 9)
            local += sum(word.bit_count() for word in words[first + pair * 2:first + pair * 2 + 2])
        directory.append((total, relative))
        total += local

    def rank(i):
        if i == len(flags):
            return total
        base, relative = directory[i // 512]
        pair = i // 64 % 8
        result = base + ((relative >> ((pair - 1) * 9)) & 511 if pair else 0)
        if i & 32:
            result += words[i // 64 * 2].bit_count()
        return result + (words[i // 32] & ((1 << (i % 32)) - 1)).bit_count()

    return rank


def rank2048_index(flags):
    words = bitmap(flags)
    directory = []
    total = 0
    for first in range(0, len(words), 64):
        counts = [sum(w.bit_count() for w in words[first + j * 16:first + j * 16 + 16]) for j in range(4)]
        directory.append((total, sum(counts[j] << (j * 11) for j in range(3))))
        total += sum(counts)

    def rank(i):
        if i == len(flags):
            return total
        base, packed = directory[i // 2048]
        result = base + sum((packed >> (j * 11)) & 1023 for j in range(i // 512 % 4))
        result += sum(w.bit_count() for w in words[i // 512 * 16:i // 32])
        return result + (words[i // 32] & ((1 << (i % 32)) - 1)).bit_count()

    return rank


def tiled_lower_bound(source, queries, tile=32):
    result = []
    for first in range(0, len(queries), tile):
        at = bisect_left(source, queries[first])
        for key in queries[first:first + tile]:
            while at < len(source) and source[at] < key:
                at += 1
            result.append(at)
    return result


def check_merge(a, b):
    ka, kb = [x[0] for x in a], [x[0] for x in b]
    assert ka == sorted(set(ka)) and kb == sorted(set(kb))
    lb_a = [bisect_left(ka, key) for key in kb]
    lb_b = [bisect_left(kb, key) for key in ka]
    assert tiled_lower_bound(ka, kb) == lb_a
    assert tiled_lower_bound(kb, ka) == lb_b
    flags = [False] * len(a)
    for j, i in enumerate(lb_a):
        if i < len(a) and ka[i] == kb[j]:
            assert not flags[i]  # Unique B hits each A at most once.
            flags[i] = True
    prefix = [0] + list(accumulate(flags))
    expected = dict(a)
    expected.update(b)  # None is a retained newer tombstone, not deletion.
    expected = sorted(expected.items())
    for build in (rank512_index, rank2048_index):
        rank = build(flags)
        assert [rank(i) for i in range(len(a) + 1)] == prefix
        output = [None] * (len(a) + len(b) - prefix[-1])
        for j, i in enumerate(lb_a):
            at = i + j - rank(i)
            assert output[at] is None
            output[at] = len(a) + j
        for i, j in enumerate(lb_b):
            if not flags[i]:
                at = i + j - rank(i)
                assert output[at] is None
                output[at] = i
        assert all(i is not None for i in output)
        assert [(a + b)[i] for i in output] == expected
        # The bitmap/compact-A/Merge-Path alternative has disjoint key sets.
        survivors = [row for i, row in enumerate(a) if not flags[i]]
        assert sorted(survivors + b) == expected


def main():
    rng = random.Random(0xC0111510)
    rank_cases = 0
    for n in (0, 1, 31, 32, 33, 63, 64, 65, 511, 512, 513, 2047, 2048, 2049, 4097):
        for pattern in range(4):
            flags = [pattern == 1 or (pattern == 2 and i % 31 == 0) or
                     (pattern == 3 and rng.randrange(7) == 0) for i in range(n)]
            expected = [0] + list(accumulate(flags))
            for build in (rank512_index, rank2048_index):
                rank = build(flags)
                assert [rank(i) for i in range(n + 1)] == expected
            rank_cases += 1

    cases = []
    for n in (0, 1, 7, 8, 15, 16, 31, 32, 33, 511, 512, 513):
        keys = [i.to_bytes(4, 'big') for i in range(n)]
        a = [(key, b'old') for key in keys]
        for step in (1, 2, 17):
            b = [(key, None if i % 3 == 0 else b'new\0') for i, key in enumerate(keys[::step])]
            cases.extend(((a, b), (b, a)))
        cases.extend(((a, []), ([], a)))
    binary = [b'', b'\0', b'\0\0', b'\0\x80', b'a', b'a\0', b'a\xff', b'\xff']
    cases.append(([(k, b'old') for k in binary], [(k, None) for k in binary[::2]]))
    for n in (31, 512, 4097):
        even = [(int(i * 2).to_bytes(4, 'big'), b'old') for i in range(n)]
        odd = [(int(i * 2 + 1).to_bytes(4, 'big'), None) for i in range(n)]
        cases.extend(((even, odd), (odd, even)))
        sparse = [(b'', None), (even[n // 2][0], None), (b'\xff' * 5, b'last')]
        cases.extend(((even, sparse), (sparse, even)))
    for _ in range(40):
        stem = bytes(rng.randrange(256) for _ in range(rng.randrange(20)))
        keys = sorted({stem + bytes(rng.randrange(256) for _ in range(rng.randrange(13))) for _ in range(90)})
        a = [(k, b'old') for k in keys if rng.randrange(4)]
        b = [(k, None if rng.randrange(3) == 0 else b'new') for k in keys if rng.randrange(4)]
        cases.append((a, b))
    for a, b in cases:
        check_merge(a, b)
    print(f'collision-rank scalar: {rank_cases} rank layouts and {len(cases)} replacement merges passed')


if __name__ == '__main__':
    main()
