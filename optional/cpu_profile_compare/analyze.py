#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Verifies paired CPU raw evidence and generates complete-path comparison tables.
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

MODES = ('typed-byte', 'raw-byte', 'raw-bit', 'typed-bit')


def crc32c(data):
    value = 0xffffffff
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0x82f63b78 if value & 1 else 0)
    return value ^ 0xffffffff


def aggregate(rows, field):
    medians = [statistics.median(row[field] for row in rows if row['process'] == i) for i in range(3)]
    return dict(median=statistics.median(medians), process_min=min(medians), process_max=max(medians),
        process_medians=medians, trial_min=min(row[field] for row in rows), trial_max=max(row[field] for row in rows))


def space_components(record):
    """Read FC and EF section lengths from the retained physical header."""
    header = bytes.fromhex(record['first_256_bytes_hex'])
    typed_bit = header[96:100] == b'KV03'
    directory_bytes = 192 if typed_bit else 128
    descriptor = 96 + (64 if typed_bit else 48)
    sections = [struct.unpack_from('<QQ', header, descriptor + 16 * i) for i in range(5)]
    assert header[16] in (0, 1)
    fc_bits = struct.unpack_from('<Q', header, 104)[0] * (1 if header[16] else 8)
    fc_bytes = sections[0][1]
    assert fc_bytes == (fc_bits + 7) // 8
    previous = directory_bytes
    for offset, length in sections:
        assert offset % 8 == 0 and offset >= previous
        assert 96 + offset + length <= record['bytes']
        previous = offset + length
    ef_bytes = sum(length for _, length in sections[1:])
    other_bytes = record['bytes'] - fc_bytes - ef_bytes
    assert other_bytes >= 96 + directory_bytes
    return dict(total_bytes=record['bytes'], total_bits=8 * record['bytes'],
        fc_logical_bits=fc_bits, fc_storage_bytes=fc_bytes,
        ef_bytes=ef_bytes, other_bytes=other_bytes)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    root = args.directory
    rows = json.loads((root / 'results.json').read_text())
    checks = json.loads((root / 'checks.json').read_text())
    metadata = json.loads((root / 'metadata.json').read_text())
    manifest = json.loads((root / 'manifest.json').read_text())
    assert len(rows) == 1152 and len(checks) == 288
    assert metadata['all_sources_and_artifacts_unchanged']
    assert metadata['all_logical_identities_match_metal_fixtures']
    assert metadata['raw_and_typed_bit_wire_identical_between_revisions']
    logical, physical, spaces = {}, {}, {}
    headers = 0
    for check in checks:
        assert logical.setdefault(check['case'], check['logical_sha256']) == check['logical_sha256']
        assert {key: check[key] for key in ('case', 'mode', 'revision', 'process')} == metadata['schedule'][check['sequence']]
        for extension in ('stdout', 'stderr'):
            raw = (root / 'raw' / (check['tag'] + '.' + extension)).read_bytes()
            assert hashlib.sha256(raw).hexdigest() == check[extension + '_sha256']
        parsed = list(csv.DictReader(io.StringIO((root / 'raw' / (check['tag'] + '.stdout')).read_text())))
        saved = [r for r in rows if r['sequence'] == check['sequence']]
        assert [r['trial'] for r in saved] == [-1, 0, 1, 2]
        for text, actual in zip(parsed, saved):
            for key, value in text.items():
                value = value if key in ('mode', 'distribution') else float(value) if key.endswith('_ms') else int(value)
                assert value == actual[key], (check['tag'], key)
        for name, record in check['physical'].items():
            group = (check['case'], check['mode'], check['revision'] if check['mode'] == 'typed-byte' else '', name)
            assert physical.setdefault(group, (record['sha256'], record['bytes'])) == (record['sha256'], record['bytes'])
            header = bytearray.fromhex(record['first_256_bytes_hex'])
            assert header[96:100] == (b'KV03' if check['mode'] == 'typed-bit' else b'KV02')
            assert struct.unpack_from('<Q', header, 56)[0] == saved[0]['output_records' if name == 'output.kv' else 'older']
            assert struct.unpack_from('<Q', header, 80)[0] == record['bytes']
            size_key = 'output_bytes' if name == 'output.kv' else 'input_a_bytes' if name == 'input-a.kv' else 'input_b_bytes'
            assert saved[0][size_key] == record['bytes']
            if name == 'output.kv':
                key = (check['case'], check['mode'], check['revision'])
                value = space_components(record)
                assert spaces.setdefault(key, value) == value
            header = header[:96]
            checksum = struct.unpack_from('<I', header, 68)[0]
            header[68:72] = bytes(4)
            assert crc32c(header) == checksum
            headers += 1
    summary = []
    for case in range(12):
        entry = {'case': case, 'logical_sha256': logical[case], 'modes': {}}
        for mode in MODES:
            pair = {}
            for revision in ('baseline', 'candidate'):
                data = [r for r in rows if r['case'] == case and r['mode'] == mode and r['revision'] == revision and r['trial'] >= 0]
                assert len(data) == 9
                keys = ('distribution', 'older', 'newer', 'cancelled', 'input_a_bytes', 'input_b_bytes', 'output_bytes', 'output_records', 'value_bytes')
                result = {key: data[0][key] for key in keys}
                result['space'] = spaces[(case, mode, revision)]
                assert all(all(row[key] == result[key] for key in keys) for row in data)
                for field in ('complete_ms', 'merge_ms', 'output_ms'):
                    result[field] = aggregate(data, field)
                pair[revision] = result
            old, new = pair['baseline']['complete_ms'], pair['candidate']['complete_ms']
            pair['baseline_over_candidate'] = old['median'] / new['median']
            pair['candidate_percent_change'] = (new['median'] / old['median'] - 1) * 100
            pair['process_range'] = 'candidate-faster' if new['process_max'] < old['process_min'] else 'candidate-slower' if old['process_max'] < new['process_min'] else 'overlap'
            pair['trial_range'] = 'candidate-faster' if new['trial_max'] < old['trial_min'] else 'candidate-slower' if old['trial_max'] < new['trial_min'] else 'overlap'
            entry['modes'][mode] = pair
        entry['candidate_typed_bit_over_byte'] = entry['modes']['typed-bit']['candidate']['complete_ms']['median'] / entry['modes']['typed-byte']['candidate']['complete_ms']['median']
        entry['candidate_raw_bit_over_byte'] = entry['modes']['raw-bit']['candidate']['complete_ms']['median'] / entry['modes']['raw-byte']['candidate']['complete_ms']['median']
        entry['candidate_typed_byte_over_bit_bytes'] = entry['modes']['typed-byte']['candidate']['output_bytes'] / entry['modes']['typed-bit']['candidate']['output_bytes']
        entry['candidate_over_baseline_typed_byte_bytes'] = entry['modes']['typed-byte']['candidate']['output_bytes'] / entry['modes']['typed-byte']['baseline']['output_bytes']
        summary.append(entry)
    space_rows = []
    for entry in summary:
        for mode, revision in [('raw-bit', 'candidate'), ('raw-byte', 'candidate'), ('typed-bit', 'candidate'),
                ('typed-byte', 'baseline'), ('typed-byte', 'candidate')]:
            value = entry['modes'][mode][revision]
            space_rows.append(dict(case=entry['case'], distribution=value['distribution'],
                value_shape='FF16' if entry['case'] % 6 < 3 else 'FV0..512',
                records=value['output_records'], mode=mode, revision=revision,
                logical_key_bytes=value['output_records'] * 16, logical_value_bytes=value['value_bytes'],
                **value['space']))
    (root / 'space.json').write_text(json.dumps(space_rows, indent=2, sort_keys=True) + '\n')
    with (root / 'space.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(space_rows[0]))
        writer.writeheader()
        writer.writerows(space_rows)
    mode_summary = {}
    for mode in MODES:
        pairs = [entry['modes'][mode] for entry in summary]
        mode_summary[mode] = {'lower_candidate_medians': sum(pair['baseline_over_candidate'] > 1 for pair in pairs),
            'ratio_min': min(pair['baseline_over_candidate'] for pair in pairs), 'ratio_max': max(pair['baseline_over_candidate'] for pair in pairs),
            'process_ranges': {name: sum(pair['process_range'] == name for pair in pairs) for name in ('candidate-faster', 'candidate-slower', 'overlap')},
            'trial_ranges': {name: sum(pair['trial_range'] == name for pair in pairs) for name in ('candidate-faster', 'candidate-slower', 'overlap')}}
    result = {'aggregation': 'median of three fresh-process medians, each over three measured iterations',
        'timed_rows': sum(row['trial'] >= 0 for row in rows), 'excluded_warmups': sum(row['trial'] < 0 for row in rows),
        'physical_headers_checked': headers, 'mode_summary': mode_summary, 'cases': summary}
    (root / 'summary.json').write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    typed = mode_summary['typed-byte']
    def table_value(value):
        return f"{value['median']:.3f} [{value['process_min']:.3f}–{value['process_max']:.3f}]"
    def label(entry):
        value = entry['modes']['raw-byte']['baseline']
        shape = 'FF16' if entry['case'] % 6 < 3 else 'FV 0–512'
        return f"{value['distribution']} | {shape} | {value['older']:,}"
    lines = ['Matched CPU bit and byte native merges', '======================================', '',
        f"The candidate typed-byte path has lower complete-path medians in {typed['lower_candidate_medians']}/12 cases. "
        f"Baseline/candidate ratios span {typed['ratio_min']:.3f}–{typed['ratio_max']:.3f}×; "
        f"{typed['process_ranges']['candidate-faster']} process-median ranges are disjoint in its favor. "
        'The raw-bit and typed-bit controls are mixed, so these results do not establish a general EF-cursor speedup.', '',
        f"The frozen comparison is `{metadata['header_revisions']['baseline']}` → `{metadata['header_revisions']['candidate']}`. "
        'The candidate combines forward EF navigation, byte-writer changes, and the built-in byte-string transport. '
        'The later equal-key donor improvement is outside this measurement, whose inputs have no equal keys or cancellations.', '',
        'All modes represent the same sixteen-byte logical keys and present values. The typed paths are real, distinct '
        'transports: bit KV03 versus byte KV02. Raw KV02 controls remove typed framing and keep exactly the same raw '
        'key/value bytes. This is native merge throughput, not a complete transaction or indexed-world benchmark.', '',
        'Method and spread', '-----------------', '',
        f"The collection contains {len(checks)} fresh processes, {result['timed_rows']} timed iterations and "
        f"{result['excluded_warmups']} excluded warmups. Each case/mode/revision has three fresh processes with three "
        'measured iterations each. Tables show milliseconds: the median of those three process medians, followed '
        'by their minimum–maximum. Revision order alternates for each case/mode across rounds, with the unavoidable '
        '2:1 first-slot split; mode and case order rotate. All trials are retained.', '',
        'The complete timer includes merge setup on existing mapped inputs, native output allocation/encoding, '
        'EF construction, a fresh output mapping and direct section copy, body/header CRC32C, clipping and all '
        'return-time cleanup. Inputs are encoded, mapped and validated before timing. Fixture construction, '
        'sorted-record oracles and output validation are excluded. There is no `fsync`, `msync`, catalog publication '
        'or fractional-index construction. Inner phase diagnostics in the raw data omit some final destruction; '
        'the outer complete timer includes it.', '',
        '| Mode | Lower candidate medians | Baseline/candidate ratio range | Process ranges: faster / slower / overlap | All-trial ranges: faster / slower / overlap |',
        '|---|---:|---:|---:|---:|']
    for mode, stats in mode_summary.items():
        p, t = stats['process_ranges'], stats['trial_ranges']
        lines.append(f"| {mode} | {stats['lower_candidate_medians']}/12 | {stats['ratio_min']:.3f}–{stats['ratio_max']:.3f} | "
            f"{p['candidate-faster']} / {p['candidate-slower']} / {p['overlap']} | {t['candidate-faster']} / {t['candidate-slower']} / {t['overlap']} |")
    for mode in MODES:
        lines += ['', mode + ' complete latency', '-' * (len(mode) + 17), '',
            '| Keys | Values | Records/input | Baseline ms | Candidate ms | Baseline/candidate | Process ranges |',
            '|---|---|---:|---:|---:|---:|---|']
        for entry in summary:
            pair = entry['modes'][mode]
            lines.append(f"| {label(entry)} | {table_value(pair['baseline']['complete_ms'])} | {table_value(pair['candidate']['complete_ms'])} | "
                f"{pair['baseline_over_candidate']:.3f}× | {pair['process_range']} |")
    lines += ['', 'Control interpretation', '----------------------', '']
    for mode in ('raw-bit', 'typed-bit'):
        worse = [entry for entry in summary if entry['modes'][mode]['process_range'] == 'candidate-slower']
        for entry in worse:
            pair = entry['modes'][mode]
            value = pair['baseline']
            shape = 'FF16' if entry['case'] % 6 < 3 else 'FV 0–512'
            lines.append(f"The {mode} {value['distribution']} {shape} case at {value['older']:,}/input has a "
                f"{pair['candidate_percent_change']:.2f}% higher candidate median with disjoint process-median ranges. "
                f"Its all-trial range classification is `{pair['trial_range']}`.")
            lines.append('')
    lines += ['The raw-bit results are a useful control for the generic profile navigation change. '
        'The unchanged typed-bit path also varies. The small regressions have not been causally isolated. '
        'Source inspection shows the forward EF cursor copies its complete view/state only at codec boundaries '
        '(once per fifteen records here), then commits that copy after parsing succeeds; it does not copy the '
        'whole cursor on every record. That is a possible follow-up cost to investigate, not a measured explanation. '
        'No additional experiment or selective rerun was used to remove these results.', '',
        'Candidate bit versus byte', '-------------------------', '',
        'These cross-mode ratios use the same frozen candidate and logical records. They compare the actual '
        'complete formats, including their different framing and output sizes. They do not isolate one instruction '
        'or one wire-code feature.', '',
        '| Keys | Values | Records/input | Typed bit ms | Typed byte ms | Typed bit / byte | Raw bit / byte |',
        '|---|---|---:|---:|---:|---:|---:|']
    for entry in summary:
        bit = entry['modes']['typed-bit']['candidate']['complete_ms']['median']
        byte = entry['modes']['typed-byte']['candidate']['complete_ms']['median']
        lines.append(f"| {label(entry)} | {bit:.3f} | {byte:.3f} | {entry['candidate_typed_bit_over_byte']:.3f}× | {entry['candidate_raw_bit_over_byte']:.3f}× |")
    lines += ['', 'Exact encoded output sizes', '--------------------------', '',
        'Sizes are complete logical file bytes, including envelope, directory, EF, padding and checksums. '
        'Raw-bit, raw-byte and typed-bit files are byte-identical between revisions; only typed-byte wire changes. '
        'All input file sizes and hashes are also retained in the raw checks.', '',
        '| Keys | Values | Records/input | Raw bit | Raw byte | Typed bit | Typed byte baseline | Typed byte candidate |',
        '|---|---|---:|---:|---:|---:|---:|---:|']
    for entry in summary:
        values = [entry['modes'][mode][revision]['output_bytes'] for mode, revision in
            [('raw-bit', 'candidate'), ('raw-byte', 'candidate'), ('typed-bit', 'candidate'), ('typed-byte', 'baseline'), ('typed-byte', 'candidate')]]
        lines.append('| ' + label(entry) + ' | ' + ' | '.join(f'{value:,}' for value in values) + ' |')
    reduction = [100 * (1 - entry['candidate_over_baseline_typed_byte_bytes']) for entry in summary]
    lines += ['', f"The new typed-byte transport reduces complete file size by {min(reduction):.2f}–{max(reduction):.2f}% "
        'relative to the old typed-byte transport across these fixtures. Against typed-bit, the space tradeoff '
        'depends on the key distribution and value sizes:', '']
    for first, end in ((0, 3), (3, 6), (6, 9), (9, 12)):
        group = summary[first:end]
        changes = [100 * (entry['candidate_typed_byte_over_bit_bytes'] - 1) for entry in group]
        assert all(value > 0 for value in changes) or all(value < 0 for value in changes)
        value = group[0]['modes']['typed-byte']['candidate']
        shape = 'FF16' if first % 6 < 3 else 'FV 0–512'
        direction = 'more' if changes[0] > 0 else 'less'
        magnitudes = [abs(value) for value in changes]
        lines.append(f"- {value['distribution']} {shape}: candidate typed-byte uses {min(magnitudes):.3f}–{max(magnitudes):.3f}% {direction} complete file space than typed-bit.")
    lines += ['', 'The following ratios use complete output bytes for the same records; a ratio below one means '
        'the numerator occupies less space. Total file bits are exactly eight times the byte counts above. '
        '[The full space table](results/space.csv) also records those bit totals, logical key/value byte counts, '
        'and component sizes for all sixty distinct case/format/revision combinations.', '',
        '| Keys | Values | Records/input | Candidate typed byte / typed bit bytes | Candidate / baseline typed byte bytes |',
        '|---|---|---:|---:|---:|']
    for entry in summary:
        lines.append(f"| {label(entry)} | {entry['candidate_typed_byte_over_bit_bytes']:.6f} | {entry['candidate_over_baseline_typed_byte_bytes']:.6f} |")
    lines += ['', 'Space components', '----------------', '',
        'These are exact physical section lengths from the retained file headers, with no new merge runs. '
        'The front-coded record stream includes key controls, key suffixes and values; it is not solely user payload. '
        'EF includes low/high words, select samples, sparse entries and their word padding. Other bytes comprise '
        'the envelope, directory, sort metadata where present, and section alignment. The three physical columns '
        'sum to the complete file size. The separate FC bit count excludes its final partial-byte padding.', '',
        'The largest case for each key/value distribution is shown here: case 2 is structured FF16, '
        'case 5 structured FV, case 8 hash-like FF16, and case 11 hash-like FV. '
        'FF cases use 262,144/input and FV cases 131,072/input; the linked table contains every size.', '',
        '| Case | Format | Revision | FC logical bits | FC storage bytes | EF bytes | Other bytes | Total bytes |',
        '|---:|---|---|---:|---:|---:|---:|---:|']
    for value in space_rows:
        if value['case'] in (2, 5, 8, 11):
            lines.append(f"| {value['case']} | {value['mode']} | {value['revision']} | " +
                ' | '.join(f"{value[key]:,}" for key in ('fc_logical_bits', 'fc_storage_bytes', 'ef_bytes', 'other_bytes', 'total_bytes')) + ' |')
    lines += ['', 'Validation and provenance', '-------------------------', '',
        f"- All {len(checks)} processes passed exact whole-file equality with an independently ordered, full-record writer oracle after every iteration. "
        'Every output received a full CRC/semantic scan; warmup outputs and all inputs were independently decoded to the logical records.',
        '- All canonical logical input/output SHA-256 values match across four modes, both revisions, all processes, '
        'and the earlier matched Metal fixtures. Only the typed-byte wire change is allowed across revisions.',
        f'- Independently checked {headers} retained physical headers for native layout kind, record count, physical extent and header CRC32C. '
        'Full physical SHA-256 values are stable within each format/revision. Disposable full input/output files were removed after checks and header/hash capture.',
        '- Both release builds passed sixteen serial CTests. The candidate also passed all sixteen strict O2 ASan/UBSan CTests. '
        'Only the O3 release binaries supplied the retained timings.',
        f"- The collector verified both immutable header revisions ({len(manifest['builds']['baseline']['headers'])} files each), "
        f"{len(manifest['harness_sources'])} harness/fixture sources, and both binary hashes before and after collection. "
        f"Measured harness source: `{manifest['harness_revision']}`.",
        '- One preflight attempt stopped while reading sandbox-restricted CPU identity metadata, before any benchmark process. '
        'The identical frozen collector then ran with authorized read-only metadata access; no timed attempt was replaced.', '',
        f"Host: {metadata['host']['cpu']}, macOS {metadata['host']['os']} ({metadata['host']['os_build']}). "
        f"Collection date: {datetime.fromtimestamp(metadata['started_unix'], timezone.utc).date().isoformat()}. "
        'C++20 with O3, NDEBUG and strict warnings. The existing fixture limit of 131,072/input for FV '
        'keeps the grid identical to the Metal comparison; it is not a CPU format-size limit.', '',
        'See [raw rows](results/results.csv), [all summaries](results/summary.json), '
        '[source/artifact manifest](results/manifest.json), [logical/physical checks](results/checks.json), '
        '[schedule/host metadata](results/metadata.json), [correctness qualification](results/qualification.json), '
        'and [the method](README.md). '
        'Earlier Metal calibration and fixed-format measurements are unchanged.', '']
    (root / 'report.md').write_text('\n'.join(lines))
    print(json.dumps(mode_summary, indent=2))


if __name__ == '__main__':
    main()
