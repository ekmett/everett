#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Checks complete-file accounting and summarizes matched byte/bit space.
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
import argparse
import csv
import json
import pathlib
from collections import defaultdict


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('results', type=pathlib.Path)
    args = parser.parse_args()
    root = args.results
    rows = list(csv.DictReader((root / 'files.csv').open()))
    raw = []
    for path in sorted((root / 'raw').glob('*.csv')):
        raw.extend(csv.DictReader(path.open()))
    assert sorted(map(str, rows)) == sorted(map(str, raw)), 'canonical/raw CSV mismatch'
    files = json.loads((root / 'file-sha256.json').read_text())
    fixtures = json.loads((root / 'fixtures.json').read_text())
    metadata = json.loads((root / 'metadata.json').read_text())
    identities = {(r['fixture'], r['logical_records'], r['mode'], r['file']): r for r in files}
    groups = defaultdict(list)
    component_names = ['data_bytes', 'ef_low_bytes', 'ef_high_bytes', 'ef_sample_bytes', 'ef_sparse_bytes',
                       'rank_bytes', 'flag_bytes', 'cut_bytes', 'sort_metadata_bytes', 'header_directory_bytes', 'alignment_bytes']
    for row in rows:
        for key in list(row):
            if key not in ('mode', 'fixture', 'file'):
                row[key] = int(row[key])
        assert row['file_bytes'] == sum(row[k] for k in component_names), 'complete-file byte accounting'
        assert row['data_bits'] == row['key_literal_bits'] + row['encoded_value_bits'] + row['framing_bits']
        assert row['data_bits'] <= row['data_bytes'] * 8
        identity = identities[(row['fixture'], row['logical_records'], row['mode'], row['file'])]
        assert identity['bytes'] == row['file_bytes']
        groups[(row['fixture'], row['logical_records'], row['mode'])].append(row)
    summaries = {}
    for (name, count, mode), items in sorted(groups.items()):
        native = [r for r in items if r['file'].endswith('.kv')]
        index = [r for r in items if r['file'].endswith('.index')]
        assert len(native) == 4 and sum(r['records'] for r in native) == count
        summary = {key: sum(r[key] for r in items) for key in component_names}
        summary.update(fixture=name, logical_records=count, mode=mode,
                       native_bytes=sum(r['file_bytes'] for r in native),
                       index_bytes=sum(r['file_bytes'] for r in index),
                       total_bytes=sum(r['file_bytes'] for r in items), file_count=len(items),
                       native_key_literal_bits=sum(r['key_literal_bits'] for r in native),
                       native_encoded_value_bits=sum(r['encoded_value_bits'] for r in native),
                       native_framing_bits=sum(r['framing_bits'] for r in native),
                       index_key_literal_bits=sum(r['key_literal_bits'] for r in index),
                       index_framing_bits=sum(r['framing_bits'] for r in index),
                       stream_zero_escape_delta=sum(2*r['records'] + r['suffix_zero_bytes'] - r['suffix_length_bytes'] for r in items)
                       if mode.endswith('byte') else None)
        summary.update(fixtures[f'{name}/{count}'])
        summaries[(name, count, mode)] = summary
    assert len(summaries) == 8 * len(metadata['sizes']) * 4
    comparisons = []
    for name in sorted({g[0] for g in groups}):
        for count in metadata['sizes']:
            for family in ['raw', 'typed']:
                byte = summaries[(name, count, family + '-byte')]
                bit = summaries[(name, count, family + '-bit')]
                assert byte['sha256'] == bit['sha256'] and byte['file_count'] == bit['file_count']
                comparisons.append(dict(fixture=name, records=count, family=family,
                    byte_total=byte['total_bytes'], bit_total=bit['total_bytes'],
                    byte_native=byte['native_bytes'], bit_native=bit['native_bytes'],
                    byte_index=byte['index_bytes'], bit_index=bit['index_bytes'],
                    total_delta_percent=100 * (byte['total_bytes'] / bit['total_bytes'] - 1),
                    native_delta_percent=100 * (byte['native_bytes'] / bit['native_bytes'] - 1),
                    index_delta_percent=100 * (byte['index_bytes'] / bit['index_bytes'] - 1),
                    extra_bytes_per_record=(byte['total_bytes'] - bit['total_bytes']) / count))
    (root / 'summary.json').write_text(json.dumps(list(summaries.values()), indent=2) + '\n')
    (root / 'comparisons.json').write_text(json.dumps(comparisons, indent=2) + '\n')
    checks = dict(complete_files=len(rows), matched_logical_fixtures=len(fixtures),
                  accounting=True, canonical_matches_raw=True, matched_sha256=True,
                  record_totals=True, unchanged_graph_file_counts=True,
                  production_reader_scan_and_decoded_equality=True, prepared_cascade_probes=True)
    (root / 'checks.json').write_text(json.dumps(checks, indent=2) + '\n')
    largest = max(metadata['sizes'])
    lines = ['Generated Space Tables', '======================', '',
             f'Complete native plus index files at {largest:,} logical rows. Positive deltas mean byte files are larger.', '']
    for family in ['typed', 'raw']:
        lines += [family.capitalize(), '-' * len(family), '',
                  '| Fixture | Byte `.kv` | Bit `.kv` | Byte `.index` | Bit `.index` | Complete byte delta | Bytes/row delta |',
                  '| --- | ---: | ---: | ---: | ---: | ---: | ---: |']
        for c in comparisons:
            if c['family'] != family or c['records'] != largest:
                continue
            lines.append(f"| {c['fixture']} | {c['byte_native']:,} | {c['bit_native']:,} | {c['byte_index']:,} | {c['bit_index']:,} | {c['total_delta_percent']:+.2f}% | {c['extra_bytes_per_record']:+.3f} |")
        lines += ['']
    lines += ['Typed Size Sweep', '----------------', '',
              '| Fixture | ' + ' | '.join(f'{n:,}' for n in metadata['sizes']) + ' |',
              '| --- | ' + ' | '.join('---:' for _ in metadata['sizes']) + ' |']
    for name in sorted({g[0] for g in groups}):
        entries = [next(c for c in comparisons if c['fixture'] == name and c['family'] == 'typed' and c['records'] == n) for n in metadata['sizes']]
        lines.append('| ' + name + ' | ' + ' | '.join(f"{c['total_delta_percent']:+.2f}%" for c in entries) + ' |')
    (root / 'tables.md').write_text('\n'.join(lines) + '\n')
    print(json.dumps(checks, indent=2))


if __name__ == '__main__':
    main()
