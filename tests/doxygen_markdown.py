#!/usr/bin/env python3
##
# \file
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense

"""Adapt dollar math in Markdown to Doxygen commands without changing lines.

This is a narrow math adapter, not a Markdown renderer. Fenced/indented code,
backtick spans, escaped dollars, unmatched delimiters and existing Doxygen math
are preserved. Inline math requires non-whitespace inside both delimiters and
cannot end immediately before a digit (so ordinary currency stays literal).
"""

from pathlib import Path
import re
import sys


def escaped(source, at):
    first = at
    while first and source[first - 1] == "\\":
        first -= 1
    return (at - first) % 2 != 0


def next_dollar(source, at, delimiter):
    while True:
        at = source.find(delimiter, at)
        if at < 0 or not escaped(source, at):
            return at
        at += len(delimiter)


def prose_math(source, formulas):
    result = []
    at = 0
    while at < len(source):
        # Repository prose uses fenced code; retain ordinary indented code too.
        if (at == 0 or source[at - 1] == "\n") and source.startswith(("    ", "\t"), at):
            end = source.find("\n", at)
            end = len(source) if end < 0 else end + 1
            result.append(source[at:end])
            at = end
            continue
        if source[at] == "`" and not escaped(source, at):
            run = re.match(r"`+", source[at:]).group()
            close = re.search(r"(?<!`)" + re.escape(run) + r"(?!`)", source[at + len(run):])
            if close:
                end = at + len(run) + close.end()
                result.append(source[at:end])
                at = end
                continue
            result.append(run)
            at += len(run)
            continue
        existing = next((opening for opening in (r"\f$", r"\f[")
                         if source.startswith(opening, at)), None)
        if existing:
            closing = r"\f$" if existing == r"\f$" else r"\f]"
            end = source.find(closing, at + len(existing))
            if end >= 0:
                result.append(source[at:end + len(closing)])
                at = end + len(closing)
                continue
        if source[at] == "$" and not escaped(source, at):
            delimiter = "$$" if source.startswith("$$", at) else "$"
            first = at + len(delimiter)
            end = next_dollar(source, first, delimiter)
            valid = end >= 0
            # A lone currency dollar must not borrow a closer from a later
            # code span or an already-adapted Doxygen formula.
            if valid and re.search(r"`|\\f(?:\$|\[)", source[first:end + len(delimiter)]):
                valid = False
            if delimiter == "$" and valid:
                valid = (end > first and not source[first].isspace() and
                         not source[end - 1].isspace() and not source.startswith("$$", end) and
                         not (end + 1 < len(source) and source[end + 1].isdigit()) and
                         "\n\n" not in source[first:end])
            if valid:
                body = source[first:end]
                opening, closing = (r"\f[", r"\f]") if delimiter == "$$" else (r"\f$", r"\f$")
                result.append(opening + body + closing)
                formulas.append(("display" if delimiter == "$$" else "inline", body))
                at = end + len(delimiter)
                continue
            result.append(delimiter)
            at += len(delimiter)
            continue
        result.append(source[at])
        at += 1
    return "".join(result)


def adapt_markdown(source):
    result, pending, formulas = [], [], []
    fence = None
    for line in source.splitlines(keepends=True):
        if fence:
            result.append(line)
            if re.fullmatch(r" {0,3}" + re.escape(fence[0]) + "{" + str(len(fence)) + r",}[ \t]*(?:\r?\n)?", line):
                fence = None
            continue
        match = re.match(r" {0,3}(`{3,}|~{3,})(.*)", line)
        if match and (match.group(1)[0] != "`" or "`" not in match.group(2)):
            result.append(prose_math("".join(pending), formulas))
            pending.clear()
            result.append(line)
            fence = match.group(1)
        else:
            pending.append(line)
    result.append(prose_math("".join(pending), formulas))
    return "".join(result), formulas


def main():
    if len(sys.argv) != 2:
        raise ValueError("expected one Markdown input filename")
    converted, _ = adapt_markdown(Path(sys.argv[1]).read_text(encoding="utf-8"))
    sys.stdout.write(converted)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        print(f"Doxygen Markdown filter failed: {error}", file=sys.stderr)
        sys.exit(1)


##
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Adapts Markdown dollar math for Diet's Doxygen build.
