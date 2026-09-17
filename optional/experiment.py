# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Read the actual CMake-selected CPU profile without retaining local paths."""
import hashlib
import json
from pathlib import Path


def build_metadata(directory):
    root = Path(__file__).resolve().parent.parent
    result = json.loads((Path(directory) / 'everett-experiment.json').read_text())
    result['shared_source_sha256'] = {
        name: hashlib.sha256((root / name).read_bytes()).hexdigest()
        for name in ['optional/host_backend.h', 'optional/experiment.py',
                     'cmake/everett-experiment.cmake']
    }
    return result
