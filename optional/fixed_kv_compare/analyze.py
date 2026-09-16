#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Verifies matched raw records and reports process-level latency and size.
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
import argparse
import csv
from datetime import datetime, timezone
import hashlib
import io
import json
from pathlib import Path
import statistics
import struct


def crc32c(data):
    result = 0xffffffff
    for value in data:
        result ^= value
        for _ in range(8):
            result = (result >> 1) ^ (0x82f63b78 if result & 1 else 0)
    return result ^ 0xffffffff


def aggregate(rows, field):
    processes = [statistics.median(row[field] for row in rows if row['process'] == i) for i in range(3)]
    return {'median': statistics.median(processes), 'process_min': min(processes),
        'process_max': max(processes), 'process_medians': processes,
        'trial_min': min(row[field] for row in rows), 'trial_max': max(row[field] for row in rows)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    root = args.directory
    rows = json.loads((root / 'results.json').read_text())
    checks = json.loads((root / 'checks.json').read_text())
    metadata = json.loads((root / 'metadata.json').read_text())
    assert len(rows) == 288 and len(checks) == 72
    assert metadata['all_sources_and_artifacts_unchanged'] and metadata['all_logical_identities_equal']
    identities = {}
    physical_identities = {}
    headers_checked = 0
    for check in checks:
        key = check['case']
        assert identities.setdefault(key, check['logical_sha256']) == check['logical_sha256']
        tag = check['tag']
        for extension in ('stdout', 'stderr'):
            raw = (root / 'raw' / (tag + '.' + extension)).read_bytes()
            assert hashlib.sha256(raw).hexdigest() == check[extension + '_sha256']
        parsed = list(csv.DictReader(io.StringIO((root / 'raw' / (tag + '.stdout')).read_text())))
        actual = [r for r in rows if r['sequence'] == check['sequence']]
        assert len(actual) == 4
        for raw, saved in zip(parsed, actual):
            for key, value in raw.items():
                value = value if key in ('layout', 'distribution') else float(value) if key.endswith('_ms') else int(value)
                assert value == saved[key], (tag, key)
        for name, record in check['physical'].items():
            identity = (check['case'], check['mode'], name)
            assert physical_identities.setdefault(identity, (record['sha256'], record['bytes'])) == (record['sha256'], record['bytes'])
            if name == 'due.bin':
                continue
            header = bytearray.fromhex(record['first_256_bytes_hex'])
            output = name.startswith('output')
            count = actual[0]['output_records' if output else 'older']
            reported_bytes = actual[0]['output_bytes' if output else 'input_a_bytes' if name.startswith('input-a') else 'input_b_bytes']
            assert record['bytes'] == reported_bytes
            if check['mode'] == 'fixed':
                words = struct.unpack('<64I', header)
                assert words[3] == count and words[16] == record['bytes']
                header[80:84] = bytes(4)
                assert crc32c(header) == words[20]
            else:
                header = header[:96]
                assert struct.unpack_from('<Q', header, 56)[0] == count
                assert struct.unpack_from('<Q', header, 80)[0] == record['bytes']
                checksum = struct.unpack_from('<I', header, 68)[0]
                header[68:72] = bytes(4)
                assert crc32c(header) == checksum
            headers_checked += 1
    summary = []
    for case in range(12):
        entry = {'case': case, 'formats': {}, 'logical_sha256': identities[case]}
        for mode in ('fixed', 'kv'):
            data = [r for r in rows if r['case'] == case and r['mode'] == mode and r['trial'] >= 0]
            assert len(data) == 9
            constant = ('layout', 'distribution', 'older', 'newer', 'cancelled', 'input_a_bytes', 'input_b_bytes', 'output_bytes', 'output_records', 'value_bytes')
            stats = {key: data[0][key] for key in constant}
            assert all(all(row[key] == stats[key] for key in constant) for row in data)
            for field in ('complete_ms', 'internal_ms', 'device_ms', 'checksum_ms'):
                stats[field] = aggregate(data, field)
            stats['input_bytes'] = stats['input_a_bytes'] + stats['input_b_bytes']
            entry['formats'][mode] = stats
        fixed, kv = entry['formats']['fixed'], entry['formats']['kv']
        for field in ('older', 'newer', 'cancelled', 'output_records', 'value_bytes'):
            assert fixed[field] == kv[field]
        f, k = fixed['complete_ms'], kv['complete_ms']
        entry['kv_over_fixed_latency'] = k['median'] / f['median']
        entry['fixed_over_kv_output_bytes'] = fixed['output_bytes'] / kv['output_bytes']
        entry['process_ranges_disjoint_fixed_faster'] = f['process_max'] < k['process_min']
        entry['trial_ranges_disjoint_fixed_faster'] = f['trial_max'] < k['trial_min']
        summary.append(entry)
    result = {'aggregation': 'median of three fresh-process medians; each process median uses three measured iterations',
        'timed_rows': 216, 'excluded_warmups': 72, 'physical_headers_checked': headers_checked, 'cases': summary}
    (root / 'summary.json').write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    ratios = [entry['kv_over_fixed_latency'] for entry in summary]
    wins = sum(value > 1 for value in ratios)
    process_separated = sum(entry['process_ranges_disjoint_fixed_faster'] for entry in summary)
    trial_separated = sum(entry['trial_ranges_disjoint_fixed_faster'] for entry in summary)
    def label(entry):
        f = entry['formats']['fixed']
        return f"{f['distribution']} {'FF16' if f['layout'] == 'ff' else 'FV'} {f['older']:,}/input"
    process_overlap = ', '.join(label(e) for e in summary if not e['process_ranges_disjoint_fixed_faster'])
    trial_overlap = ', '.join(label(e) for e in summary if not e['trial_ranges_disjoint_fixed_faster'])
    def size_range(layout, distribution, smaller=False):
        values = [(1 - entry['fixed_over_kv_output_bytes'] if smaller else entry['fixed_over_kv_output_bytes'] - 1) * 100
            for entry in summary if entry['formats']['fixed']['layout'] == layout
            and entry['formats']['fixed']['distribution'] == distribution]
        return f'{min(values):.2f}–{max(values):.2f}%'
    # The fixture-specific size discussion below requires these observed directions;
    # fail rather than emit stale qualitative prose if the wire formats change.
    assert all(e['fixed_over_kv_output_bytes'] > 1 for e in summary if e['formats']['fixed']['distribution'] == 'structured')
    assert all(e['fixed_over_kv_output_bytes'] < 1 for e in summary if e['formats']['fixed']['distribution'] == 'hash-like')
    manifest = json.loads((root / 'manifest.json').read_text())
    measured_date = datetime.fromtimestamp(metadata['started_unix'], timezone.utc).date().isoformat()
    lines = ['Matched fixed-key and KV03 Metal results', '=======================================', '',
        f'The fixed formats have lower complete-path medians in {wins} of {len(summary)} matched no-cancellation cases. '
        f'The KV03/fixed median ratio ranges from {min(ratios):.2f} to {max(ratios):.2f}×. The result depends on key distribution and value size. '
        'The tables distinguish structured keys from hash-like keys and report encoded sizes separately.', '',
        f"This measures two standalone Metal implementations on an {metadata['host']['cpu']}. It does not establish "
        'a transaction, durability, fractional-index, cancellation, or general-purpose sort speedup. '
        'Both paths include fresh output mapping/import, CRC32C, clipping and return-time cleanup. '
        'Neither performs durable synchronization.', '',
        'Complete merge latency', '----------------------', '',
        'Times are milliseconds: median of three process medians, followed by their minimum–maximum. '
        'Each process has three measured iterations after one excluded warmup. The fixed schedule '
        'alternates format order across the three process pairs for every case. All 216 timed rows '
        'and 72 warmups are retained.', '',
        '| Keys | Values | Records/input | Fixed complete ms | KV03 complete ms | KV03 / fixed |',
        '|---|---|---:|---:|---:|---:|']
    def interval(x):
        return f"{x['median']:.3f} [{x['process_min']:.3f}–{x['process_max']:.3f}]"
    for entry in summary:
        f, k = entry['formats']['fixed'], entry['formats']['kv']
        lines.append(f"| {f['distribution']} | {'FF16' if f['layout']=='ff' else 'FV 0–512'} | {f['older']:,} | {interval(f['complete_ms'])} | {interval(k['complete_ms'])} | {entry['kv_over_fixed_latency']:.2f}× |")
    lines += ['', f'{process_separated} of {len(summary)} process-median ranges are disjoint in the fixed format’s favor. '
        f'The other case(s) are: {process_overlap or "none"}. Looking at all nine individual iterations '
        f'instead, {trial_separated} cases have disjoint ranges; the other case(s) are: '
        f'{trial_overlap or "none"}. These short runs show visible '
        'variability; no trial or process was discarded.', '',
        'Encoded size', '------------', '',
        'All sizes below are exact logical file bytes, including headers, checksums, values and '
        'navigation sections. Inputs means the sum of both input files. Physical page rounding '
        'and scratch capacity are excluded from these file sizes. The fixed due-list envelope '
        'is another 32 input bytes in every case; it contains no cancellation ordinals.', '',
        '| Keys | Values | Records/input | Fixed inputs | KV03 inputs | Fixed output | KV03 output | Fixed / KV03 output |',
        '|---|---|---:|---:|---:|---:|---:|---:|']
    for entry in summary:
        f,k=entry['formats']['fixed'],entry['formats']['kv']
        lines.append(f"| {f['distribution']} | {'FF16' if f['layout']=='ff' else 'FV 0–512'} | {f['older']:,} | {f['input_bytes']:,} | {k['input_bytes']:,} | {f['output_bytes']:,} | {k['output_bytes']:,} | {entry['fixed_over_kv_output_bytes']:.3f} |")
    lines += ['', f"With structured FF16 keys, fixed output is {size_range('ff', 'structured')} larger. "
        f"With hash-like FF16 keys it is {size_range('ff', 'hash-like', True)} smaller. "
        'The much larger FV payload reduces the size difference: '
        f"fixed output is {size_range('fv', 'structured')} larger for structured keys and "
        f"{size_range('fv', 'hash-like', True)} smaller for hash-like keys.", '',
        'The same logical keys and present values are encoded by both formats. KV03 includes '
        'its ordinary single-sort selector, optional-string value framing and front coding. '
        'The experimental fixed formats use fixed-width keys with implicit or EF-addressed value lengths. '
        'This is a comparison of those actual formats, not identical wire grammars.', '',
        'Internal diagnostics', '--------------------', '',
        'These are also medians of process medians, in milliseconds. They are **not matching timing '
        'boundaries**: fixed internal starts with imported inputs/output and excludes CRC; KV03 '
        'internal includes fresh output mapping and CRC but stops before final return cleanup. '
        'Only the complete table above compares matching boundaries. Fixed CRC is separately timed; '
        'KV03 CRC remains part of its total.', '',
        '| Case | Fixed internal | KV03 internal | Fixed CRC | Fixed device | KV03 device |',
        '|---:|---:|---:|---:|---:|---:|']
    for entry in summary:
        f,k=entry['formats']['fixed'],entry['formats']['kv']
        lines.append(f"| {entry['case']} | {f['internal_ms']['median']:.3f} | {k['internal_ms']['median']:.3f} | {f['checksum_ms']['median']:.3f} | {f['device_ms']['median']:.3f} | {k['device_ms']['median']:.3f} |")
    lines += ['', 'Validation and scope', '--------------------', '',
        '- All 72 processes passed complete canonical CPU-output equality after every warmup and timed iteration. '
        'The corresponding CPU encoders were also checked against the independently merged logical fixture records.',
        '- Canonical logical input and output SHA-256 values agree across both formats and all processes for every case. '
        'All physical output hashes are stable across processes of a format; 216 retained physical headers have independently checked counts, file extents and header CRCs.',
        '- Eight separate serial Metal correctness CTests passed before collection. '
        f"All {len(manifest['sources'])} frozen source hashes and {len(manifest['artifacts'])} artifact hashes remained unchanged through the collection.",
        '- KV03 uses the calibrated optimized path: compressed mapped inputs, GPU EF/frame parsing, prefix-owner tree, '
        'eight-byte prefix cache, packed-word output and GPU output EF. Levelwise tree and separate output plan remain '
        'the defaults after earlier mixed experiments. Fixed uses its existing no-cancellation path and Merge Path 32. '
        'No claim is made that either implementation is an optimized limit.',
        '- Fixture generation, sorting hash-like keys, input encoding/mapping, pipeline warmup and output verification '
        'are outside timing. There is no CPU input-key reconstruction or per-record setup inside the merge timer.',
        '- No cancellation or tombstone case is included. Such a comparison requires matching logical updates while '
        'disclosing KV tombstone versus fixed due-ordinal physical counts.',
        '- FV stops at 131,072 records/input because the existing KV maximum-record-bound guard rejects the next '
        'larger proposed case. The guard was preserved; this is not an observed actual-file-size limit.', '',
        'Provenance', '----------', '',
        f"Measured host source: `{metadata['source_revision']}`. Collection date: {measured_date}. "
        f"Device: {metadata['host']['cpu']}; backend: Metal; macOS {metadata['host']['os']} ({metadata['host']['os_build']}). "
        'Host wrappers use C++20 Objective-C++ with `-O3 -DNDEBUG`, ARC and strict warnings. '
        'The existing, independently qualified Metal libraries were reused after exact current HLSL-source hash checks; '
        'no shader was modified for this comparison.', '',
        'See [the frozen manifest](results/manifest.json), [shader provenance](results/shader-provenance.json), '
        '[raw rows](results/results.csv), [all summaries](results/summary.json), '
        '[physical/logical checks](results/checks.json), and [schedule/host metadata](results/metadata.json). '
        '[The method and reproduction guide](README.md) defines the complete boundary and fixture mapping. '
        'Original resident-path measurements and earlier KV calibration remain unchanged.', '']
    (root / 'report.md').write_text('\n'.join(lines))
    print(f'checked {len(rows)} raw rows, {headers_checked} headers, {len(summary)} cases')


if __name__ == '__main__':
    main()
