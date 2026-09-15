#!/usr/bin/env python3
"""Generate reference documentation and verify Doxygen's XML associations."""

import argparse
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as xml


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def text(element):
    def content(node):
        value = node.text or ""
        for child in node:
            value += " " if child.tag == "sp" else content(child)
            value += child.tail or ""
        return value + ("\n" if node.tag == "codeline" else "")
    return " ".join(content(element).split()) if element is not None else ""


def quote(value):
    return '"' + str(value).replace("\\", "/").replace('"', '\\"') + '"'


def run_doxygen(executable, source, inputs, output, aliases, html=False):
    output.mkdir(parents=True, exist_ok=True)
    warnings = output / "warnings.log"
    warnings.write_text("", encoding="utf-8")
    config = [
        "PROJECT_NAME = Everett",
        "OUTPUT_DIRECTORY = " + quote(output),
        "INPUT = " + " ".join(quote(path) for path in inputs),
        "STRIP_FROM_PATH = " + quote(source),
        "FILE_PATTERNS = *.h",
        "RECURSIVE = YES",
        "EXTRACT_ALL = YES",
        "EXTRACT_PRIVATE = YES",
        "GENERATE_XML = YES",
        "XML_PROGRAMLISTING = NO",
        "GENERATE_HTML = " + ("YES" if html else "NO"),
        "GENERATE_LATEX = NO",
        "HAVE_DOT = NO",
        "QUIET = YES",
        "WARNINGS = YES",
        "WARN_IF_DOC_ERROR = YES",
        # Ordinary // implementation comments are not claimed as API docs.
        "WARN_IF_UNDOCUMENTED = NO",
        "WARN_AS_ERROR = NO",
        "WARN_LOGFILE = " + quote(warnings),
    ]
    if aliases:
        # Match ekmett/ein doc/Doxyfile.in: preserve the SPDX text as a code
        # block, ending it explicitly before author and brief metadata.
        config += ['ALIASES = "license=@code{.spdx}"',
                   'ALIASES += "endlicense=@endcode"']
    doxyfile = output / "Doxyfile"
    doxyfile.write_text("\n".join(config) + "\n", encoding="utf-8")
    result = subprocess.run([executable, str(doxyfile)], cwd=source,
                            capture_output=True, text=True, check=False)
    (output / "stdout.log").write_text(result.stdout, encoding="utf-8")
    (output / "stderr.log").write_text(result.stderr, encoding="utf-8")
    require(result.returncode == 0, f"Doxygen failed: {output}\n{result.stderr}")
    diagnostics = warnings.read_text(encoding="utf-8") + result.stderr
    if aliases:
        require(not diagnostics.strip(), f"Doxygen diagnostics: {diagnostics}")
    return diagnostics


def compounds(output):
    # Follow index.xml rather than globbing stale XML from a previous run.
    index = xml.parse(output / "xml/index.xml").getroot()
    result = []
    for entry in index.findall("compound"):
        path = output / "xml" / (entry.attrib["refid"] + ".xml")
        compound = xml.parse(path).getroot().find("compounddef")
        require(compound is not None, f"Missing compound in {path}")
        result.append(compound)
    return result


def description(element):
    return " ".join(text(element.find(tag)) for tag in
                    ("briefdescription", "detaileddescription", "inbodydescription"))


def footer_fields(path):
    blocks = re.findall(r"/\*\*(.*?)\*/", path.read_text(encoding="utf-8"), re.S)
    require(bool(blocks), f"Missing footer: {path}")
    footer = blocks[-1]
    require(re.search(r"\\file\s", footer), f"Missing file command: {path}")
    brief = re.search(r"\\brief ([^\n]+)", footer)
    author = re.search(r"\\author ([^\n]+)", footer)
    require(brief is not None and author is not None, f"Incomplete footer: {path}")
    notices = re.findall(r"SPDX-[^\n]+", footer)
    require(len(notices) == 3, f"Incomplete SPDX notices: {path}")
    normalize = lambda value: " ".join(value.replace("<", "").replace(">", "").split())
    return normalize(brief.group(1)), normalize(author.group(1)), [" ".join(value.split()) for value in notices]


def check_file_metadata(items, headers, aliases=True):
    files = {item.findtext("compoundname"): item for item in items
             if item.attrib["kind"] == "file"}
    require(set(files) == {path.name for path in headers}, "Unexpected documented file set")
    briefs = []
    for path in headers:
        brief, author, notices = footer_fields(path)
        briefs.append(brief)
        item = files[path.name]
        require(text(item.find("briefdescription")) == brief, f"Wrong file brief: {path}")
        details = item.find("detaileddescription")
        authors = details.findall(".//simplesect[@kind='author']")
        require(len(authors) == 1 and text(authors[0]) == author, f"Wrong file author: {path}")
        for notice in notices:
            rendered_notice = notice if aliases else notice.replace("<", "").replace(">", "")
            require(rendered_notice in text(details), f"Lost file notice: {path}: {notice}")
        if aliases:
            licenses = details.findall(".//programlisting")
            require(len(licenses) == 1, f"Missing separate SPDX code block: {path}")
            license_section = licenses[0]
            require(not license_section.findall(".//simplesect[@kind='author']"),
                    f"Author nested in license: {path}")
            for notice in notices:
                require(notice in text(license_section), f"Notice outside license: {path}")
            require(brief not in text(license_section), f"Brief nested in license: {path}")
            require("\\author" not in text(license_section) and "\\brief" not in text(license_section),
                    f"Unterminated SPDX code block: {path}")
    for item in items:
        checked = list(item.findall("./sectiondef/memberdef"))
        if item.attrib["kind"] != "file":
            checked.append(item)
        for entity in checked:
            docs = description(entity)
            require("SPDX-" not in docs, f"File notice leaked into {entity.findtext('name')}")
            require(not any(brief in docs for brief in briefs), "File brief leaked into symbol")


def named_compound(items, kind, name):
    matches = [item for item in items if item.attrib["kind"] == kind
               and item.findtext("compoundname") == name]
    require(len(matches) == 1, f"Expected one {kind} {name}; found {len(matches)}")
    return matches[0]


def check_actual_members(items, source):
    cases = [
        ("struct", "everett::multiverse", "open_object", "multiverse.h", None, "no"),
        ("struct", "everett::mapped_file", "open", "mapped_file.h", None, "yes"),
        ("struct", "everett::mapped_slice", "bytes", "mapped_file.h", "lvalue", "no"),
        ("struct", "everett::mapped_slice", "bytes", "mapped_file.h", "rvalue", "no"),
        ("struct", "everett::profile_view", "reconstruct_at", "profile.h", None, "no"),
        ("namespace", "everett::file_detail", "get", "file.h", None, "no"),
        ("namespace", "everett", "crc32c", "file.h", None, "no"),
    ]
    for kind, owner, name, filename, qualifier, static in cases:
        compound = named_compound(items, kind, owner)
        members = [member for member in compound.findall("./sectiondef/memberdef")
                   if member.attrib["kind"] == "function" and member.findtext("name") == name
                   and (qualifier is None or member.get("refqual") == qualifier)]
        require(len(members) == 1, f"Wrong overload count for {owner}::{name}/{qualifier}")
        member = members[0]
        require(member.findtext("qualifiedname") == owner + "::" + name,
                f"Wrong qualified owner for {owner}::{name}")
        require(member.get("static") == static, f"Wrong static association: {owner}::{name}")
        location = member.find("location")
        require(location is not None, f"Missing source location for {owner}::{name}")
        require(location.get("file", "").endswith("include/everett/" + filename),
                f"Wrong source file for {owner}::{name}")
        lines = (source / "include/everett" / filename).read_text(encoding="utf-8").splitlines()
        line = int(location.attrib["line"])
        require(0 < line <= len(lines) and re.search(r"\b" + name + r"\s*\(", lines[line - 1]),
                f"Wrong source line for {owner}::{name}: {line}")
        if qualifier == "rvalue":
            require("=delete" in member.findtext("argsstring", "").replace(" ", ""),
                    "Deleted rvalue overload was merged with lvalue overload")
    for owner, parameters in (("everett::multiverse", ["P"]), ("everett::profile_view", ["P", "Role"])):
        item = named_compound(items, "struct", owner)
        names = []
        for param in item.findall("./templateparamlist/param"):
            name = param.findtext("declname")
            if name is None:
                declaration = re.fullmatch(r"(?:class|typename)\s+(\w+)", text(param.find("type")))
                name = declaration.group(1) if declaration else None
            names.append(name)
        require(names == parameters, f"Wrong template association: {owner}: {names}")


def make_fixtures(directory, placement):
    directory.mkdir(parents=True, exist_ok=True)
    expected = {}
    for filename, namespace, parameter in (("alpha.h", "left", "int"), ("beta.h", "right", "double")):
        lines = [f"namespace fixture::{namespace} {{"]
        specs = []
        for type_name in ("int", "double"):
            marker = f"{namespace}_free_{type_name}."
            lines += [f"/// \\brief {marker}", f"inline {type_name} choose({type_name} value) {{ return value; }}"]
            specs.append((f"fixture::{namespace}", "choose", marker, "function"))
        marker = f"{namespace}_box."
        lines += [f"/// \\brief {marker}", "struct box {"]
        specs.append((f"fixture::{namespace}::box", "", marker, "struct"))
        for type_name in ("int", "double"):
            marker = f"{namespace}_member_{type_name}."
            lines += [f"  /// \\brief {marker}", f"  {type_name} choose({type_name} value) const {{ return value; }}"]
            specs.append((f"fixture::{namespace}::box", "choose", marker, "function"))
        marker = f"{namespace}_global."
        lines += ["};", "}", f"/// \\brief {marker}",
                  f"inline {parameter} standalone({parameter} value) {{ return value; }}"]
        specs.append((filename, "standalone", marker, "function"))
        footer = ["/**", r" * \file", r" * \license", " * SPDX-FileType: SOURCE",
                  " * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>. All rights reserved.",
                  " * SPDX-License-Identifier: LicenseRef-Everett-All-Rights-Reserved",
                  r" * \endlicense", r" * \author Edward Kmett <ekmett@gmail.com>",
                  f" * \\brief File {filename} marker.", " */"]
        complete = footer + lines if placement == "before" else lines + footer
        (directory / filename).write_text("\n".join(complete) + "\n", encoding="utf-8")
        for owner, name, marker, kind in specs:
            line = next(i + 2 for i, value in enumerate(complete) if value.strip() == "/// \\brief " + marker)
            expected[marker] = (owner, name, kind, filename, line)
    return expected


def check_fixtures(items, expected):
    found = {}
    for item in items:
        owner = item.findtext("compoundname")
        entities = [(item, item.attrib["kind"], "")]
        entities += [(member, member.attrib["kind"], member.findtext("name"))
                     for member in item.findall("./sectiondef/memberdef")]
        for entity, kind, name in entities:
            marker = text(entity.find("briefdescription"))
            if marker not in expected:
                continue
            require(marker not in found, f"Fixture documentation duplicated: {marker}")
            want_owner, want_name, want_kind, filename, line = expected[marker]
            require((owner, name, kind) == (want_owner, want_name, want_kind),
                    f"Fixture owner mismatch: {marker}: {(owner, name, kind)}")
            location = entity.find("location")
            require(location is not None and Path(location.attrib["file"]).name == filename
                    and int(location.attrib["line"]) == line, f"Fixture location mismatch: {marker}")
            qualified = entity.findtext("qualifiedname", "")
            if kind == "function":
                want_qualified = name if owner.endswith(".h") else owner + "::" + name
                if owner.endswith(".h") and not qualified:
                    # Doxygen 1.9.8 omits qualifiedname for global functions.
                    definition = entity.findtext("definition", "")
                    require("::" not in definition and definition.endswith(" " + name),
                            f"Fixture global scope mismatch: {marker}")
                    qualified = name
                require(qualified == want_qualified, f"Fixture qualification mismatch: {marker}")
            found[marker] = (owner, kind, qualified, entity.findtext("argsstring", ""),
                             description(entity), filename)
    require(set(found) == set(expected), f"Missing fixture markers: {set(expected) - set(found)}")
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--doxygen", required=True)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    headers = sorted((source / "include/everett").glob("*.h"))
    require(bool(headers), "No Everett headers found")
    baseline = output / "baseline"
    diagnostics = run_doxygen(args.doxygen, source, headers, baseline, aliases=False)
    lines = diagnostics.splitlines()
    require(len(lines) == 2 * len(headers), "Default configuration did not warn for both custom commands per header")
    require(all(re.search(r"warning: Found unknown command ['`]\\(?:end)?license['`]", line) for line in lines),
            f"Unexpected baseline diagnostics: {diagnostics}")
    baseline_items = compounds(baseline)
    check_file_metadata(baseline_items, headers, aliases=False)
    check_actual_members(baseline_items, source)

    reference = output / "reference"
    run_doxygen(args.doxygen, source, headers, reference, aliases=True, html=True)
    items = compounds(reference)
    check_file_metadata(items, headers)
    check_actual_members(items, source)
    fixture_results = []
    for placement in ("before", "after"):
        inputs = output / ("fixture-" + placement)
        expected = make_fixtures(inputs, placement)
        generated = output / ("fixture-" + placement + "-docs")
        run_doxygen(args.doxygen, source, [inputs], generated, aliases=True)
        items = compounds(generated)
        check_file_metadata(items, sorted(inputs.glob("*.h")))
        fixture_results.append(check_fixtures(items, expected))
    require(fixture_results[0] == fixture_results[1], "Moving the file block changed symbol documentation")
    print(f"Checked {len(headers)} headers, seven real function/overload associations, "
          "and twelve fixture symbols with file blocks before/after declarations.")
    print(f"Reference documentation: {reference / 'html/index.html'}")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, xml.ParseError) as error:
        print(f"Doxygen check failed: {error}", file=sys.stderr)
        sys.exit(1)


##
# \file
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>. All rights reserved.
# SPDX-License-Identifier: LicenseRef-Everett-All-Rights-Reserved
# \endlicense
# \author Edward Kmett <ekmett@gmail.com>
# \brief Tests Everett's Doxygen metadata and declaration associations.
