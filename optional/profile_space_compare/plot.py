#!/usr/bin/env python3
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Plots complete matched byte/bit file space with codec controls visible.
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
import argparse
import csv
import hashlib
import json
import pathlib

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    here = pathlib.Path(__file__).resolve().parent
    parser.add_argument('--input', type=pathlib.Path, default=here / 'results/comparisons.csv')
    parser.add_argument('--output', type=pathlib.Path, default=here / 'results/space-tradeoff')
    args = parser.parse_args()
    rows = list(csv.DictReader(args.input.open()))
    sizes = [1024, 8192, 32768, 131072]
    shown = []

    def series(fixture, family):
        result = sorted((r for r in rows if r['fixture'] == fixture and r['family'] == family), key=lambda r: int(r['records']))
        assert [int(r['records']) for r in result] == sizes
        values = []
        for row in result:
            byte, bit = int(row['byte_total']), int(row['bit_total'])
            assert byte == int(row['byte_native']) + int(row['byte_index'])
            assert bit == int(row['bit_native']) + int(row['bit_index'])
            value = 100 * (byte / bit - 1)
            assert abs(value - float(row['total_delta_percent'])) < 1e-10
            values.append(value)
            shown.append(dict(fixture=fixture, family=family, records=int(row['records']),
                              byte_total=byte, bit_total=bit, delta_percent=value))
        return values

    plt.rcParams.update({
        'font.family': 'DejaVu Sans', 'font.size': 10,
        'axes.spines.top': False, 'axes.spines.right': False,
        'axes.edgecolor': '#a3acb7', 'axes.labelcolor': '#243247',
        'text.color': '#162235', 'xtick.color': '#435268', 'ytick.color': '#435268',
        'svg.hashsalt': 'everett-profile-space-20260916',
    })
    fig, axes = plt.subplots(1, 2, figsize=(12.8, 7.5), gridspec_kw={'width_ratios': [1.55, 1]})
    fig.subplots_adjust(left=0.075, right=0.965, top=0.785, bottom=0.29, wspace=0.28)
    fig.suptitle('Complete byte-file space relative to bit files', x=0.075, y=0.965,
                 ha='left', fontsize=19, fontweight='bold')
    fig.text(0.075, 0.913, 'Same logical records and searchable layout · native .kv + fractional .index files', fontsize=11)
    fig.text(0.075, 0.87, 'Positive means byte files are larger; negative means smaller. These are current codec choices, not alignment alone.',
             fontsize=10, color='#435268')

    for ax in axes:
        ax.set_xscale('log', base=2)
        ax.set_xlim(800, 180000)
        ax.set_xticks(sizes, ['1K', '8K', '32K', '128K'])
        ax.set_xlabel('Logical records (K = 1,024)', labelpad=9)
        ax.axhline(0, color='#526174', linewidth=1.2, zorder=1)
        ax.grid(axis='y', color='#e2e6eb', linewidth=0.7)
        ax.set_axisbelow(True)
        ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f'{v:+g}%' if v else '0%'))

    strings = [
        ('string-prefix-fixed', 'Decimal-ending keys', '#1764a6'),
        ('string-structured-fixed', 'Same keys + /state', '#c36510'),
        ('string-binary-fixed', 'Binary keys', '#7a5299'),
    ]
    for fixture, label, color in strings:
        for family, style, marker in [('typed', '-', 'o'), ('typed-known', '--', 's')]:
            values = series(fixture, family)
            axes[0].plot(sizes, values, color=color, linestyle=style, marker=marker,
                         linewidth=2, markersize=5, markerfacecolor='white' if family == 'typed-known' else color)
            if family == 'typed-known':
                axes[0].annotate(f'{values[-1]:+.2f}%', (sizes[-1], values[-1]), xytext=(-3, 9),
                                 textcoords='offset points', ha='right', color=color, fontsize=9.5, fontweight='bold')
    axes[0].set_title('String keys · 16-byte values', loc='left', fontweight='bold', pad=12)
    axes[0].set_ylim(-5.1, 12.5)
    axes[0].set_yticks([-4, 0, 4, 8, 12])
    axes[0].set_ylabel('Size difference relative to bit files', labelpad=8)
    colors = axes[0].legend(handles=[Line2D([0], [0], color=c, linewidth=2, label=label) for _, label, c in strings],
                            loc='lower left', bbox_to_anchor=(-0.01, -0.335), frameon=False, fontsize=9.5)
    axes[0].add_artist(colors)
    axes[0].legend(handles=[
        Line2D([0], [0], color='#354459', linewidth=2, marker='o', label='Default byte writer'),
        Line2D([0], [0], color='#354459', linewidth=2, linestyle='--', marker='s', markerfacecolor='white', label='Known value width'),
    ], loc='lower right', bbox_to_anchor=(1.015, -0.335), frameon=False, fontsize=9.5)

    for fixture, label, color in [
        ('integer-random-fixed', 'Random u64 keys', '#1764a6'),
        ('integer-ordered-fixed', 'Ordered u64 keys', '#c36510'),
    ]:
        values = series(fixture, 'typed')
        axes[1].plot(sizes, values, marker='o', color=color, linewidth=2, markersize=5, label=label)
        axes[1].annotate(f'{values[-1]:+.2f}%', (sizes[-1], values[-1]), xytext=(-3, 10),
                         textcoords='offset points', ha='right', color=color, fontsize=10, fontweight='bold')
    axes[1].set_title('u64 keys · fixed u64 values', loc='left', fontweight='bold', pad=12)
    axes[1].set_ylim(-34, 10)
    axes[1].set_yticks([-30, -20, -10, 0, 10])
    axes[1].legend(loc='lower left', bbox_to_anchor=(-0.01, -0.31), frameon=False, fontsize=9.5)

    fig.text(0.075, 0.105, 'Known width is an explicit low-level writer hint: all values are present, 16 bytes + a 1-byte tag; it is not the connection default.',
             fontsize=9, color='#435268')
    fig.text(0.075, 0.072, 'Integer caveat: current byte KV02 front-codes ordered integers; bit KV03 stores each integer in 64 bits. Both declare fixed value width.',
             fontsize=9, color='#435268')
    fig.text(0.075, 0.039, 'All rows are live and unique; 15:1 sampling and 15-record codec blocks. Complete files, including envelopes and navigation; no timing claims.',
             fontsize=9, color='#435268')

    args.output.parent.mkdir(parents=True, exist_ok=True)
    png = args.output.with_suffix('.png')
    svg = args.output.with_suffix('.svg')
    fig.savefig(png, dpi=180, facecolor='white', metadata={'Software': 'Matplotlib'})
    fig.savefig(svg, facecolor='white', metadata={'Date': None, 'Creator': 'Matplotlib'})
    plt.close(fig)
    provenance = dict(
        input=args.input.name, input_sha256=sha256(args.input), script=pathlib.Path(__file__).name,
        script_sha256=sha256(pathlib.Path(__file__)), matplotlib_version=matplotlib.__version__,
        y_metric='100 * ((byte_native + byte_index) / (bit_native + bit_index) - 1)',
        files='Complete serialized native and fractional-index files; metadata catalog and filesystem allocation excluded.',
        measured_source_revision='9fbf906111523b2ea7c040021855b58043d74fbe',
        data=shown, artifacts={png.name: sha256(png), svg.name: sha256(svg)},
        notes=['Known-width control is a low-level writer hint for all-present fixed-length string fixtures.',
               'Integer native grammars differ: byte FC versus bit fixed-width raw integers.',
               'This figure measures current typed codecs, not byte alignment in isolation.'])
    args.output.with_suffix('.json').write_text(json.dumps(provenance, indent=2) + '\n')


if __name__ == '__main__':
    main()
