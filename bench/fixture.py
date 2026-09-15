#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Stages self-contained benchmark fixtures with policy API compatibility.
#
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense
"""Inline the benchmark-only adapter so the staged source hash covers it.

This changes fixture compilation, never the library headers extracted by
Snapshot. Historical fixtures without the include remain byte-for-byte intact.
"""
from pathlib import Path


def self_contained(source, adapter=None):
    include = b'#include "policy_compat.h"'
    if include not in source:
        return source
    if adapter is None:
        adapter = Path(__file__).with_name('policy_compat.h').read_bytes()
    return source.replace(include, adapter.rstrip(b'\n'))


def write_fixture(source, destination):
    Path(destination).write_bytes(self_contained(Path(source).read_bytes()))
