#!/usr/bin/env python3
##
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Tests Everett's Doxygen metadata and declaration associations.
#
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense

"""Generate API/Markdown documentation and verify Doxygen associations and math."""

import argparse
from collections import Counter
import html as html_module
from html.parser import HTMLParser
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import unicodedata
from urllib.parse import quote as urlquote, unquote, urlsplit
import xml.etree.ElementTree as xml

from doxygen_markdown import adapt_markdown


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


def run_doxygen(executable, source, inputs, output, aliases, html=False, markdown_main=None):
    output.mkdir(parents=True, exist_ok=True)
    # Doxygen rewrites its index, but leaves pages for removed declarations in
    # place. A publication must contain only the current source's reference.
    for generated in (output / "html", output / "xml"):
        if generated.exists():
            shutil.rmtree(generated)
    warnings = output / "warnings.log"
    warnings.write_text("", encoding="utf-8")
    config = [
        "PROJECT_NAME = Everett",
        "OUTPUT_DIRECTORY = " + quote(output),
        "INPUT = " + " ".join(quote(path) for path in inputs),
        "STRIP_FROM_PATH = " + quote(source),
        "FILE_PATTERNS = *.h *.md",
        "MARKDOWN_SUPPORT = YES",
        "MARKDOWN_ID_STYLE = GITHUB",
        "RECURSIVE = YES",
        "EXTRACT_ALL = YES",
        "EXTRACT_PRIVATE = YES",
        "GENERATE_XML = YES",
        "XML_PROGRAMLISTING = NO",
        "GENERATE_HTML = " + ("YES" if html else "NO"),
        "GENERATE_LATEX = NO",
        "USE_MATHJAX = YES",
        "HAVE_DOT = NO",
        "QUIET = YES",
        "WARNINGS = YES",
        "WARN_IF_DOC_ERROR = YES",
        # Ordinary // implementation comments are not claimed as API docs.
        "WARN_IF_UNDOCUMENTED = NO",
        "WARN_AS_ERROR = NO",
        "WARN_LOGFILE = " + quote(warnings),
    ]
    if markdown_main is not None:
        helper = Path(__file__).with_name("doxygen_markdown.py").resolve()
        command = shlex.join([sys.executable, str(helper)])
        config += ["USE_MDFILE_AS_MAINPAGE = " + quote(markdown_main),
                   "FILTER_PATTERNS = " + quote("*.md=" + command)]
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


def metadata_fields(path):
    source = path.read_text(encoding="utf-8")
    blocks = [block for block in re.finditer(r"/\*\*(.*?)\*/", source, re.S)
              if re.search(r"\\file\s", block.group(1))]
    require(len(blocks) == 1, f"Expected one combined file metadata block: {path}")
    head = blocks[0]
    require(not source[:head.start()].strip(), f"File metadata must precede code: {path}")
    metadata = head.group(1)
    require(len(re.findall(r"\\file\b", metadata)) == 1, f"Nonunique file command: {path}")
    briefs = re.findall(r"\\brief ([^\n]+)", metadata)
    authors = re.findall(r"\\author ([^\n]+)", metadata)
    require(len(briefs) == 1 and len(authors) == 1, f"Nonunique brief or author: {path}")
    fields = ("FileType", "FileCopyrightText", "License-Identifier")
    notices = []
    for field in fields:
        matches = re.findall(r"SPDX-" + field + r":[^\n]+", source)
        require(len(matches) == 1, f"Expected exactly one SPDX-{field}: {path}")
        require(matches[0] in metadata, f"SPDX notice outside file metadata: {path}")
        notices.append(matches[0])
    license_blocks = re.findall(r"\\license\b(.*?)\\endlicense\b", metadata, re.S)
    require(len(license_blocks) == 1 and len(re.findall(r"\\(?:end)?license\b", metadata)) == 2,
            f"Expected one complete SPDX block: {path}")
    license_text = license_blocks[0]
    require(all(notice in license_text for notice in notices), f"SPDX notice outside license: {path}")
    require(not re.search(r"\\(?:author|brief)\b", license_text), f"File metadata nested in license: {path}")
    normalize = lambda value: " ".join(value.replace("<", "").replace(">", "").split())
    return normalize(briefs[0]), normalize(authors[0]), [" ".join(value.split()) for value in notices]


def check_file_metadata(items, headers, aliases=True, markdown=()):
    files = {item.findtext("compoundname"): item for item in items
             if item.attrib["kind"] == "file"}
    require(set(files) == {path.name for path in [*headers, *markdown]}, "Unexpected documented file set")
    briefs = []
    for path in headers:
        brief, author, notices = metadata_fields(path)
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
        if item.attrib["kind"] not in ("file", "page", "dir"):
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
        ("namespace", "everett", "crc32c", "crc32c.h", None, "no"),
        ("struct", "everett::query_root", "build", "query.h", None, "yes"),
        ("struct", "everett::query_root_builder", "finish", "query.h", None, "no"),
        ("struct", "everett::query_cursor", "step", "query.h", None, "no"),
        ("struct", "everett::query_cursor", "take_match", "query.h", None, "no"),
        ("struct", "everett::query_root", "adopt_prepared", "query.h", None, "yes"),
        ("struct", "everett::profile_view", "from_sections", "profile.h", None, "yes"),
        ("struct", "everett::mapped_blob", "bind", "mapped_blob.h", None, "yes"),
        ("struct", "everett::mapped_profile", "scan", "sections.h", None, "no"),
        ("struct", "everett::encoded_sections", "materialize", "sections.h", None, "no"),
        ("namespace", "everett", "encode_file_header", "file.h", None, "no"),
        ("struct", "everett::profile_native_writer", "finish", "native_writer.h", None, "no"),
        ("struct", "everett::native_merge_builder", "step", "native_merge.h", None, "no"),
        ("struct", "everett::object_stream", "append", "object_stream.h", None, "no"),
        ("struct", "everett::native_file_writer", "finish", "native_file_writer.h", None, "no"),
        ("struct", "everett::native_file_merge", "step", "native_file_merge.h", None, "no"),
        ("struct", "everett::profile_index", "native_only", "profile_index.h", None, "yes"),
        ("struct", "everett::index_builder", "finish_index", "index_builder.h", None, "no"),
        ("struct", "everett::file_index_builder", "finish", "file_index_builder.h", None, "no"),
        ("struct", "everett::file_index_pipeline", "seal_next", "file_index_pipeline.h", None, "no"),
        ("struct", "everett::rank_groups_builder", "append", "rank_groups.h", None, "no"),
        ("struct", "everett::cola_index_builder", "step", "cola_index.h", None, "no"),
        ("struct", "everett::cola_local_merge_job", "finish_stage", "cola_local_merge.h", None, "no"),
        ("struct", "everett::cola_query_cursor", "step", "cola_query.h", None, "no"),
        ("struct", "everett::mapped_cola_index", "scan", "cola_sections.h", None, "no"),
        ("struct", "everett::mapped_cola_blob", "bind", "mapped_cola.h", None, "yes"),
        ("struct", "everett::index_detail::pipeline_driver", "step", "index_pipeline_detail.h", None, "no"),
        ("struct", "everett::sample_cursor", "advance", "sampling.h", None, "no"),
        ("struct", "everett::elias_fano_view", "select", "elias_fano.h", None, "no"),
        ("struct", "everett::profile_view", "block_offset", "profile.h", None, "no"),
        ("struct", "everett::profile_cursor", "advance_comparison", "profile.h", None, "no"),
        ("struct", "everett::profile_blob", "adopt_native", "profile_blob.h", None, "yes"),
        ("struct", "everett::sqlite_catalog", "reserve", "sqlite_catalog.h", None, "no"),
        ("struct", "everett::sqlite_catalog", "save", "sqlite_catalog.h", None, "no"),
        ("struct", "everett::sqlite_catalog", "acquire_save", "sqlite_catalog.h", None, "no"),
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
    for owner, parameters in (("everett::multiverse", ["P"]), ("everett::profile_view", ["P", "Role"]),
                              ("everett::query_root", ["P", "Blob"]), ("everett::query_root_builder", ["P"]),
                              ("everett::query_cursor", ["P", "Blob"]), ("everett::mapped_blob", ["P"]),
                              ("everett::mapped_profile", ["P", "Role"]), ("everett::encoded_sections", ["P"]),
                              ("everett::profile_native_writer", ["P"]),
                              ("everett::native_merge_builder", ["P", "Native", "Compose", "Output"]),
                              ("everett::object_stream", ["P", "Ops"]),
                              ("everett::native_file_writer", ["P", "Ops"]),
                              ("everett::native_file_merge", ["P", "Native", "Compose", "Ops"]),
                              ("everett::profile_index", ["P"]),
                              ("everett::index_builder", ["P", "Native", "Output"]),
                              ("everett::file_index_builder", ["P", "Native", "Ops"]),
                              ("everett::file_index_pipeline", ["P", "Ops"]),
                              ("everett::sample_cursor", ["P", "Target"]),
                              ("everett::sqlite_catalog", ["P", "Ops"])):
        item = named_compound(items, "struct", owner)
        names = []
        for param in item.findall("./templateparamlist/param"):
            name = param.findtext("declname")
            if name is None:
                declaration = re.fullmatch(r"(?:class|typename)\s+(\w+)", text(param.find("type")))
                name = declaration.group(1) if declaration else None
            names.append(name)
        require(names == parameters, f"Wrong template association: {owner}: {names}")
    return len(cases)


def make_fixtures(directory):
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
        header = ["/**", r" * \file", r" * \author Edward Kmett <ekmett@gmail.com>",
                  f" * \\brief File {filename} marker.", " *", r" * \license", " * SPDX-FileType: SOURCE",
                  " * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>",
                  " * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0",
                  r" * \endlicense", " */"]
        complete = header + lines
        (directory / filename).write_text("\n".join(complete) + "\n", encoding="utf-8")
        for owner, name, marker, kind in specs:
            line = next(i + 2 for i, value in enumerate(complete) if value.strip() == "/// \\brief " + marker)
            expected[marker] = (owner, name, kind, filename, line)
    return expected


def check_metadata_rejections(directory, valid_header):
    directory.mkdir(parents=True, exist_ok=True)
    source = valid_header.read_text(encoding="utf-8")
    end = source.index("*/") + 2
    header, code = source[:end], source[end:]
    metadata = "\n".join(line for line in header.splitlines()
                         if "\\author" in line or "\\brief" in line)
    without_metadata = "\n".join(line for line in header.splitlines()
                                  if "\\author" not in line and "\\brief" not in line)
    notice = next(line for line in header.splitlines() if "SPDX-License-Identifier:" in line)
    invalid = {
        "footer": code + header,
        "split": without_metadata + code + "/**\n * \\file\n" + metadata + "\n */\n",
        "duplicate": source + header,
        "nested": without_metadata.replace(r" * \endlicense", metadata + "\n" + r" * \endlicense") + code,
        "outside-license": header.replace(notice, "").replace(r" * \license", notice + "\n" + r" * \license") + code,
        "unterminated-license": source.replace(r"\endlicense", ""),
    }
    for name, content in invalid.items():
        path = directory / (name + ".h")
        path.write_text(content, encoding="utf-8")
        try:
            metadata_fields(path)
        except RuntimeError:
            continue
        raise RuntimeError("Invalid file metadata accepted: " + name)
    return len(invalid)


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



def check_markdown_adapter():
    cases = [
        ("Inline $x+1$ and $y$.", r"Inline \f$x+1\f$ and \f$y\f$.", 2),
        ("$$\nx^2+y^2\n$$\n", "\\f[\nx^2+y^2\n\\f]\n", 1),
        ("$$x+y$$", r"\f[x+y\f]", 1),
        ("$a+\nb$", "\\f$a+\nb\\f$", 1),
        (r"Literal \$5 and unmatched $ or $5 and $10; math $x$.",
         r"Literal \$5 and unmatched $ or $5 and $10; math \f$x\f$.", 1),
        (r"$a+\$b$", r"\f$a+\$b\f$", 1),
        ("`$code$` and ``$code with ` ticks$``", "`$code$` and ``$code with ` ticks$``", 0),
        ("``multi\n$code$``\n", "``multi\n$code$``\n", 0),
        ("```text\n$fenced$\n$$display$$\n```\n", "```text\n$fenced$\n$$display$$\n```\n", 0),
        ("````text\n```\n$fenced$\n````\n", "````text\n```\n$fenced$\n````\n", 0),
        ("~~~text\n$fenced$\n~~~\n", "~~~text\n$fenced$\n~~~\n", 0),
        ("    $indented$\n\t$tabbed$\n", "    $indented$\n\t$tabbed$\n", 0),
        (r"Existing \f$x\f$ and \f[y\f]", r"Existing \f$x\f$ and \f[y\f]", 0),
        ("Unmatched $$ and $x", "Unmatched $$ and $x", 0),
    ]
    for original, expected, count in cases:
        converted, formulas = adapt_markdown(original)
        require(converted == expected, f"Markdown filter changed literals: {original!r}: {converted!r}")
        require(len(formulas) == count, f"Wrong converted formula count: {original!r}")
        require(converted.count("\n") == original.count("\n"), "Markdown filter changed source line count")
        require(adapt_markdown(converted)[0] == converted, f"Markdown filter is not idempotent: {original!r}")


def markdown_inputs(source):
    paths = [source / "README.md", source / "AGENTS.md", *sorted((source / "docs").glob("*.md"))]
    paths.extend(sorted((source / "bench").glob("*.md")))
    paths.extend(sorted((source / "optional/gpu_merge").glob("*.md")))
    paths.extend(sorted((source / "optional/fixed_gpu_merge").glob("*.md")))
    for name in ("proof/README.md", "THIRD_PARTY.md"):
        if (source / name).is_file():
            paths.append(source / name)
    paths.extend(sorted((source / "third_party/fast-crc32").glob("*.md")))
    return paths


def markdown_pages(items, source):
    pages = {}
    for item in items:
        if item.get("kind") != "page":
            continue
        location = item.find("location")
        require(location is not None, "Markdown page has no source location")
        path = Path(location.attrib["file"])
        if path.is_absolute():
            path = path.relative_to(source)
        pages[path.as_posix()] = item
    return pages


def page_html(page):
    return "index.html" if page.attrib["id"] == "indexpage" else page.attrib["id"] + ".html"


def write_page_links(page, replacements, output, name):
    if not replacements:
        return
    html_path = output / "html" / page_html(page)
    rendered = html_path.read_text(encoding="utf-8")
    found = set()
    def replace(match):
        url = html_module.unescape(match.group(1))
        if url not in replacements:
            return match.group(0)
        found.add(url)
        return 'href="' + html_module.escape(replacements[url], quote=True) + '"'
    rendered = re.sub(r'href="([^"]+)"', replace, rendered)
    require(found == set(replacements), f"Markdown link absent from HTML: {name}: {set(replacements) - found}")
    html_path.write_text(rendered, encoding="utf-8")
    xml_path = output / "xml" / (page.attrib["id"] + ".xml")
    tree = xml.parse(xml_path)
    tree.getroot().remove(tree.getroot().find("compounddef"))
    tree.getroot().append(page)
    tree.write(xml_path, encoding="utf-8", xml_declaration=True)


def heading_slug(title):
    # Doxygen can put escaped HTML (<tt>, <em>, ...) inside an XML title.
    # Decode that formatting before considering an automatic GitHub anchor.
    class TitleText(HTMLParser):
        def __init__(self):
            super().__init__(convert_charrefs=True)
            self.parts = []
        def handle_data(self, data):
            self.parts.append(data)
    parser = TitleText()
    parser.feed("".join(title.itertext()))
    parser.close()
    value = "".join(parser.parts).strip().lower()
    return "".join("-" if char == " " else char for char in value
                   if char in " _-" or unicodedata.category(char)[0] in "LNM")


def explicit_heading_ids(source):
    # Only recognize the small explicit-ID extension, not Markdown links or a
    # second heading renderer. Ignore examples inside fenced/indented code.
    result, fence = set(), None
    lines = source.splitlines()
    for at, line in enumerate(lines):
        if fence:
            if re.fullmatch(r" {0,3}" + re.escape(fence[0]) + "{" + str(len(fence)) + r",}[ \t]*", line):
                fence = None
            continue
        opening = re.match(r" {0,3}(`{3,}|~{3,})(.*)", line)
        if opening and (opening.group(1)[0] != "`" or "`" not in opening.group(2)):
            fence = opening.group(1)
            continue
        if line.startswith(("    ", "\t")):
            continue
        heading = re.match(r" {0,3}#{1,6}(?:[ \t]|$)", line)
        underline = at + 1 < len(lines) and re.fullmatch(r" {0,3}(?:=+|-+)[ \t]*", lines[at + 1])
        if heading or underline:
            match = re.search(r"\{#([^{}\s]+)\}[ \t]*(?:#+[ \t]*)?$", line)
            if match:
                result.add(match.group(1))
    return result


def markdown_heading_targets(page, explicit=()):
    # Doxygen's GITHUB IDs are globally unique, whereas GitHub numbers heading
    # collisions within one document. Reconstruct that local sequence only
    # where the generated ID confirms the parsed title's automatic slug family.
    # Keep custom IDs and unrecognized title spellings exact instead of guessing.
    targets, used = {}, set()
    if page.attrib["id"] in explicit:
        # Doxygen promotes an explicitly named leading H1 to the page itself.
        targets[page.attrib["id"]] = [(page.attrib["id"], "")]
    def unique(base):
        slug, suffix = base, 0
        while slug in used:
            suffix += 1
            slug = f"{base}-{suffix}"
        used.add(slug)
        return slug
    prefixes = [page.attrib["id"] + "_1"]
    if page.attrib["id"] == "indexpage":
        prefixes.append("index_1")
    entries = []
    for element in page.iter():
        if not re.fullmatch(r"sect[1-6]|anchor", element.tag):
            continue
        refid = element.get("id", "")
        spellings = [refid[len(prefix):] for prefix in prefixes if refid.startswith(prefix)]
        require(len(spellings) == 1, f"Unexpected Markdown heading ID: {refid}")
        entries.append((element, refid, spellings[0]))
    title, leading_id, leading = page.find("title"), None, None
    if title is not None:
        leading = heading_slug(title)
        # A real leading H1 has an anchor, or is the configured main page.
        # A fallback page title (e.g. a filename after a notice) does not count.
        if entries:
            element, refid, actual = entries[0]
            if element.tag == "anchor" and actual not in explicit and (
                    re.fullmatch(r"(?:autotoc_md)?" + re.escape(leading) + r"(?:-\d+)?", actual) or
                    (page.attrib["id"] == "indexpage" and actual.startswith("md_"))):
                leading_id = refid
        if page.attrib["id"] == "indexpage" or leading_id is not None:
            unique(leading)
    for element, refid, actual in entries:
        title = element.find("title")
        slug, automatic = (leading, True) if refid == leading_id else (actual, False)
        if title is not None and actual not in explicit:
            base = heading_slug(title)
            spelling = r"(?:autotoc_md)?" + re.escape(base) + r"(?:-\d+)?"
            if re.fullmatch(spelling, actual):
                slug, automatic = unique(base), True
        targets.setdefault(slug, []).append((refid, actual))
        if refid == leading_id and page.attrib["id"] == "indexpage" and actual != slug:
            targets.setdefault(actual, []).append((refid, actual))
        # Keep the earlier exact-ID fallback for formulas or title spellings
        # we cannot prove automatic, including Doxygen's numeric-ID prefix.
        if not automatic and actual not in explicit and actual.startswith("autotoc_md"):
            targets.setdefault(actual[len("autotoc_md"):], []).append((refid, actual))
    return targets


def markdown_heading_target(page, fragment, diagnostic, explicit=()):
    anchors = markdown_heading_targets(page, explicit).get(fragment, [])
    require(len(anchors) == 1, f"Missing or ambiguous Markdown heading: {diagnostic}")
    return anchors[0]


def repair_markdown_links(items, source, output):
    # Doxygen 1.9.8 resolves .md pages and local #headings, but leaves
    # cross-page .md#heading URLs literal. Resolve those from generated page
    # locations and section IDs, without guessing Doxygen's filename escaping
    # or parsing Markdown links ourselves. Keep labels/children untouched.
    pages = markdown_pages(items, source)
    file_pages = {}
    for item in items:
        if item.get("kind") != "file" or item.find("location") is None:
            continue
        path = Path(item.find("location").attrib["file"])
        if path.is_absolute():
            path = path.relative_to(source)
        if path.as_posix() in pages:
            file_pages[item.attrib["id"]] = pages[path.as_posix()]
    count = 0
    for name, page in pages.items():
        replacements = {}
        # A leading notice/comment can make Doxygen resolve a Markdown link
        # to its empty file compound instead of the page containing its text.
        for link in page.findall(".//ref"):
            file_id = link.get("refid")
            if link.get("kindref") == "compound" and file_id in file_pages:
                target = file_pages[file_id]
                replacements[file_id + ".html"] = page_html(target)
                link.set("refid", target.attrib["id"])
                count += 1
        for link in page.findall(".//ulink"):
            url = link.attrib["url"]
            parts = urlsplit(url)
            if parts.scheme or parts.netloc or not parts.path.endswith(".md"):
                continue
            require(not parts.query, f"Local Markdown links cannot have a query: {name}: {url}")
            target_path = (source / name).parent.joinpath(unquote(parts.path)).resolve()
            require(target_path.is_relative_to(source), f"Markdown link leaves the input tree: {name}: {url}")
            target_name = target_path.relative_to(source).as_posix()
            require(target_name in pages, f"Markdown target is not an input page: {name}: {url}")
            target = pages[target_name]
            fragment = unquote(parts.fragment)
            refid, kind = target.attrib["id"], "compound"
            if fragment:
                # This also handles Doxygen's number-leading autotoc prefix
                # and suffixes caused by matching titles on other pages.
                refid, fragment = markdown_heading_target(target, fragment, f"{name}: {url}",
                    explicit_heading_ids(target_path.read_text(encoding="utf-8")))
                kind = "compound" if refid == target.attrib["id"] else "member"
            destination = page_html(target) + (("#" + fragment) if fragment else "")
            target_html = (output / "html" / page_html(target)).read_text(encoding="utf-8")
            require(not fragment or f'id="{fragment}"' in target_html or f'name="{fragment}"' in target_html,
                    f"Generated Markdown heading is missing: {name}: {url}")
            replacements[url] = destination
            link.tag = "ref"
            link.attrib.clear()
            link.attrib.update(refid=refid, kindref=kind)
            count += 1
        write_page_links(page, replacements, output, name)
    return count


def bundle_source_links(items, source, output):
    bundled = set()
    for name, page in markdown_pages(items, source).items():
        replacements = {}
        for link in page.findall(".//ulink"):
            url = link.attrib["url"]
            parts = urlsplit(url)
            if parts.scheme or parts.netloc or not parts.path:
                continue
            target = (source / name).parent.joinpath(unquote(parts.path)).resolve()
            require(target.is_relative_to(source), f"Source link leaves the input tree: {name}: {url}")
            require(target.is_file(), f"Missing linked source file: {name}: {url}")
            relative = Path("source") / target.relative_to(source)
            destination = output / "html" / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(target, destination)
            require(destination.read_bytes() == target.read_bytes(), f"Source copy differs: {target}")
            rewritten = urlquote(relative.as_posix())
            if parts.query:
                rewritten += "?" + parts.query
            if parts.fragment:
                rewritten += "#" + parts.fragment
            replacements[url] = rewritten
            link.set("url", rewritten)
            bundled.add(relative)
        write_page_links(page, replacements, output, name)
    return len(bundled)


def check_page_link(pages, source_name, target_name, output):
    source, target = pages[source_name], pages[target_name]
    target_ids = {target.attrib["id"]}
    if target.attrib["id"] == "indexpage":
        target_ids.add("index")
    require(any(ref.get("refid") in target_ids or any(ref.get("refid", "").startswith(key + "_1")
                for key in target_ids) for ref in source.findall(".//ref")),
            f"Missing XML page reference: {source_name} -> {target_name}")
    rendered = (output / "html" / page_html(source)).read_text(encoding="utf-8")
    links = [html_module.unescape(value) for value in re.findall(r'href="([^"]+)"', rendered)]
    matching = [value for value in links if value.split("#", 1)[0] == page_html(target)]
    require(bool(matching), f"Missing rendered page link: {source_name} -> {target_name}")
    target_html = (output / "html" / page_html(target)).read_text(encoding="utf-8")
    for link in matching:
        if "#" in link:
            fragment = link.split("#", 1)[1]
            require(f'id="{fragment}"' in target_html or f'name="{fragment}"' in target_html,
                    f"Missing rendered target anchor: {source_name} -> {link}")


def check_page_anchor(pages, name, anchor, output):
    page = pages[name]
    ids = {element.get("id") for element in page.iter() if element.get("id", "").endswith("_1" + anchor)}
    require(bool(ids) and any(ref.get("refid") in ids for ref in page.findall(".//ref")),
            f"Missing XML heading reference: {name}#{anchor}")
    rendered = (output / "html" / page_html(page)).read_text(encoding="utf-8")
    require(f'id="{anchor}"' in rendered and f'href="{page_html(page)}#{anchor}"' in rendered,
            f"Missing rendered heading reference: {name}#{anchor}")


def check_markdown_pages(items, source, markdown, output):
    pages = markdown_pages(items, source)
    expected_paths = {path.relative_to(source).as_posix() for path in markdown}
    require(set(pages) == expected_paths, f"Unexpected Markdown page set: {set(pages) ^ expected_paths}")
    require(pages["README.md"].attrib["id"] == "indexpage", "README is not the main page")
    count = 0
    for path in markdown:
        original = path.read_text(encoding="utf-8")
        converted, formulas = adapt_markdown(original)
        require(original.count("\n") == converted.count("\n"), f"Filter changed source lines: {path}")
        page = pages[path.relative_to(source).as_posix()]
        expected = Counter(" ".join((("$" + body + "$") if kind == "inline" else
                                     (r"\[" + body + r"\]")).split()) for kind, body in formulas)
        actual = Counter(text(formula) for formula in page.findall(".//formula"))
        require(actual == expected, f"Formula contents/count changed: {path}: {actual - expected}; missing {expected - actual}")
        require(not page.findall(".//programlisting//formula") and not page.findall(".//computeroutput//formula"),
                f"Code literal became a formula: {path}")
        rendered = (output / "html" / page_html(page)).read_text(encoding="utf-8")
        require("MathJax" in rendered, f"MathJax script missing: {path}")
        decoded_html = html_module.unescape(rendered)
        for kind, pattern in (("inline", r"\\\((.*?)\\\)"), ("display", r"\\\[(.*?)\\\]")):
            wanted = Counter(" ".join(body.split()) for mode, body in formulas if mode == kind)
            present = Counter(" ".join(body.split()) for body in re.findall(pattern, decoded_html, re.S))
            require(not (wanted - present), f"Rendered {kind} MathJax formula payloads missing: {path}")
        require(rendered.count('class="formulaDsp"') >= sum(kind == "display" for kind, _ in formulas),
                f"Rendered display formula elements missing: {path}")
        count += len(formulas)
    require(count > 0, "No Markdown formulas were checked")
    for source_name, target_name in (("README.md", "docs/design.md"),
                                     ("README.md", "docs/usage.md"),
                                     ("docs/usage.md", "README.md"),
                                     ("docs/keys.md", "docs/arrows.md"),
                                     ("docs/sampling.md", "docs/durability.md"),
                                     ("docs/network-admission.md", "docs/rebuild.md")):
        check_page_link(pages, source_name, target_name, output)
    for anchor in ("quick-start", "saved-tables", "building"):
        check_page_anchor(pages, "README.md", anchor, output)
    for anchor in ("persistent-tables-and-saves", "examples", "field-guide", "building-and-testing"):
        check_page_anchor(pages, "docs/usage.md", anchor, output)
    if "THIRD_PARTY.md" in pages:
        check_page_link(pages, "README.md", "THIRD_PARTY.md", output)
        for name in ("LICENSE.md", "LICENSE.MIT.md", "LICENSE.zlib.md", "UPSTREAM.md"):
            check_page_link(pages, "THIRD_PARTY.md", "third_party/fast-crc32/" + name, output)
    return count


def check_markdown_fixture(executable, output):
    directory = output / "markdown fixture"
    (directory / "docs").mkdir(parents=True, exist_ok=True)
    readme = directory / "README.md"
    readme.write_text("""# Markdown fixture

Inline $x+1$ and $y_2$.

$$
z=x+y
$$

[Child](docs/child.md), [numbered section](docs/child.md#7-numbered-section),
[proof](proof/README.md), [notices](THIRD_PARTY.md), and [details](#details), and literal \\$5, $10, or a lone $.

`$inline_code$` and ``$code_with_`_tick$``.

```text
$fenced_code$
$$fenced_display$$
```

~~~text
$tilde_code$
~~~

    $indented_code$

## Shared heading

[First shared](docs/child.md#shared-heading),
[Second shared](docs/child.md#shared-heading-1), and
[Repeated page title](docs/child.md#child-page-1),
[Explicit heading](docs/child.md#shared-heading-9),
[Math heading](docs/child.md#cost-anchor), and
[Punctuation heading](docs/child.md#a-value_type--a-choice),
[Child title](docs/child.md#child-page),
[Peer title](docs/duplicate-title.md#child-page), and
[Explicit page title](docs/explicit-title.md#custom-page), and
[Main page title](README.md#markdown-fixture),
[Numbered page](docs/numeric-title.md#7-title), and
[Numbered repeat](docs/numeric-title.md#7-title-1).

## Details

An ordinary paragraph.
""", encoding="utf-8")
    child = directory / "docs/child.md"
    child.write_text("""# Child page

[Home](../README.md#details). Formula $q^2$.

## 7. Numbered section

## `Shared` heading

First occurrence in this page, after the same title on the main page.

## Shared heading

Second occurrence in this page.

## Child page

The leading page title participates in GitHub's local heading numbering.

## Shared heading {#shared-heading-9}

An explicit suffix is not an automatically numbered duplicate.

Cost $x$ {#cost-anchor}
----------------------

## A `value_type` & a *choice*!

```markdown
## Example {#shared-heading-1}
```
""", encoding="utf-8")
    (directory / "docs/duplicate-title.md").write_text("# Child page\n", encoding="utf-8")
    (directory / "docs/explicit-title.md").write_text("# Child page {#custom-page}\n", encoding="utf-8")
    (directory / "docs/numeric-title.md").write_text("# 7. Title\n\n## 7. Title\n", encoding="utf-8")
    (directory / "AGENTS.md").write_text("# Guidance\n", encoding="utf-8")
    (directory / "THIRD_PARTY.md").write_text(
        "<!-- A leading attribution notice. -->\n\nThird-party notices\n===================\n\nFixture text.\n",
        encoding="utf-8")
    (directory / "proof/.lake/generated").mkdir(parents=True, exist_ok=True)
    (directory / "proof/README.md").write_text("# Proof notes\n\n[Home](../README.md).\n", encoding="utf-8")
    (directory / "proof/.lake/generated/README.md").write_text("# Not an input\n", encoding="utf-8")
    inputs = markdown_inputs(directory)
    require({path.relative_to(directory).as_posix() for path in inputs} ==
            {"README.md", "AGENTS.md", "docs/child.md", "docs/duplicate-title.md",
             "docs/explicit-title.md", "docs/numeric-title.md", "proof/README.md", "THIRD_PARTY.md"}, "Wrong Markdown input discovery")
    generated = output / "markdown-fixture-docs"
    run_doxygen(executable, directory, inputs, generated, aliases=True, html=True, markdown_main=readme)
    items = compounds(generated)
    repair_markdown_links(items, directory, generated)
    items = compounds(generated)
    pages = markdown_pages(items, directory)
    require(set(pages) == {"README.md", "AGENTS.md", "docs/child.md", "docs/duplicate-title.md",
             "docs/explicit-title.md", "docs/numeric-title.md", "proof/README.md", "THIRD_PARTY.md"}, "Fixture pages missing")
    require(len(pages["README.md"].findall(".//formula")) == 3 and
            len(pages["docs/child.md"].findall(".//formula")) == 1, "Fixture formula nodes missing")
    details = pages["README.md"].find("detaileddescription")
    code = " ".join(text(value) for value in details.findall(".//programlisting") + details.findall(".//computeroutput") + details.findall(".//verbatim"))
    for literal in ("$inline_code$", "$code_with_`_tick$", "$fenced_code$", "$$fenced_display$$", "$tilde_code$", "$indented_code$"):
        require(literal in code, f"Fixture code literal changed: {literal}")
    require("literal $5, $10, or a lone $." in text(details), "Literal currency dollars changed")
    check_page_link(pages, "README.md", "docs/child.md", generated)
    check_page_link(pages, "README.md", "proof/README.md", generated)
    check_page_link(pages, "README.md", "THIRD_PARTY.md", generated)
    check_page_link(pages, "proof/README.md", "README.md", generated)
    check_page_link(pages, "docs/child.md", "README.md", generated)
    check_page_anchor(pages, "README.md", "details", generated)
    child = pages["docs/child.md"]
    headings = [node for node in child.iter() if re.fullmatch(r"sect[1-6]", node.tag)]
    expected = dict(zip(("First shared", "Second shared", "Repeated page title",
                         "Explicit heading", "Math heading", "Punctuation heading"),
                        [node.attrib["id"] for node in headings[1:]]))
    require(len(expected) == 6, "Fixture duplicate headings missing")
    references = {text(ref): ref.get("refid") for ref in pages["README.md"].findall(".//ref")}
    require(all(references.get(label) == refid for label, refid in expected.items()),
            f"Page-local heading sequence was replaced by global numbering: {references}")
    # Check that this actually exercised global Doxygen disambiguation, rather
    # than merely accepting a fixture with identical local/global IDs.
    require(not expected["First shared"].endswith("_1shared-heading"),
            "Fixture did not create a cross-page heading collision")
    for label, name, slug in (("Main page title", "README.md", "markdown-fixture"),
                              ("Child title", "docs/child.md", "child-page"),
                              ("Peer title", "docs/duplicate-title.md", "child-page"),
                              ("Explicit page title", "docs/explicit-title.md", "custom-page"),
                              ("Numbered page", "docs/numeric-title.md", "7-title"),
                              ("Numbered repeat", "docs/numeric-title.md", "7-title-1")):
        target = pages[name]
        refid, _ = markdown_heading_target(target, slug, "page-title fixture",
            explicit_heading_ids((directory / name).read_text(encoding="utf-8")))
        require(references.get(label) == refid, f"Duplicate or explicit page title changed: {label}")
        check_page_link(pages, "README.md", name, generated)
    require(not references["Peer title"].endswith("_1child-page"),
            "Fixture did not create a duplicated leading H1")
    numeric = pages["docs/numeric-title.md"]
    require(references["Numbered repeat"] == numeric.find(".//sect1").attrib["id"] and
            references["Numbered page"] != references["Numbered repeat"],
            "Numeric leading H1 did not reserve its page-local heading ID")
    ambiguous = xml.fromstring(xml.tostring(child))
    xml.SubElement(ambiguous.find("detaileddescription"), "anchor",
                   id=child.attrib["id"] + "_1shared-heading")
    for candidate, fragment in ((ambiguous, "shared-heading"), (child, "missing-heading")):
        try:
            markdown_heading_target(candidate, fragment, "negative heading fixture")
        except RuntimeError:
            continue
        raise RuntimeError("Accepted an ambiguous or missing Markdown heading")
    unknown = xml.fromstring('<compounddef id="unknown"><title>Unknown</title>'
        '<sect1 id="unknown_1autotoc_md7-formula"><title>7. \\f$x\\f$</title></sect1></compounddef>')
    for spelling in ("7-formula", "autotoc_md7-formula"):
        require(markdown_heading_target(unknown, spelling, "formula fallback")[0] ==
                "unknown_1autotoc_md7-formula", "Exact formula-title anchor fallback changed")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--doxygen", required=True)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    headers = sorted((source / "include/everett").glob("*.h"))
    require(bool(headers), "No Everett headers found")
    for header in headers:
        metadata_fields(header)
    baseline = output / "baseline"
    diagnostics = run_doxygen(args.doxygen, source, headers, baseline, aliases=False)
    lines = diagnostics.splitlines()
    require(len(lines) == 2 * len(headers), "Default configuration did not warn for both custom commands per header")
    require(all(re.search(r"warning: Found unknown command ['`]\\(?:end)?license['`]", line) for line in lines),
            f"Unexpected baseline diagnostics: {diagnostics}")
    baseline_items = compounds(baseline)
    check_file_metadata(baseline_items, headers, aliases=False)
    check_actual_members(baseline_items, source)

    check_markdown_adapter()
    markdown = markdown_inputs(source)
    require(all(path.is_file() for path in markdown), "Missing Markdown input")
    reference = output / "reference"
    run_doxygen(args.doxygen, source, [*headers, *markdown], reference, aliases=True, html=True,
                markdown_main=source / "README.md")
    items = compounds(reference)
    check_file_metadata(items, headers, markdown=markdown)
    member_count = check_actual_members(items, source)
    repaired_links = repair_markdown_links(items, source, reference)
    items = compounds(reference)
    source_files = bundle_source_links(items, source, reference)
    items = compounds(reference)
    formula_count = check_markdown_pages(items, source, markdown, reference)
    check_markdown_fixture(args.doxygen, output)
    inputs = output / "fixture-top"
    expected = make_fixtures(inputs)
    rejection_count = check_metadata_rejections(output / "metadata-rejections", inputs / "alpha.h")
    generated = output / "fixture-top-docs"
    run_doxygen(args.doxygen, source, [inputs], generated, aliases=True)
    items = compounds(generated)
    check_file_metadata(items, sorted(inputs.glob("*.h")))
    check_fixtures(items, expected)
    print(f"Checked {len(headers)} headers, {member_count} real function/overload associations, "
          "and twelve fixture symbols with combined top-of-file metadata.")
    print(f"Rejected {rejection_count} misplaced, duplicated or malformed file-metadata fixtures.")
    print(f"Checked {len(markdown)} Markdown pages and {formula_count} dollar formulas with MathJax HTML, "
          f"cross-page links ({repaired_links} repaired links), and protected-code/currency fixtures.")
    print(f"Bundled {source_files} linked source files with byte-for-byte checks.")
    print(f"Reference documentation: {reference / 'html/index.html'}")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, xml.ParseError) as error:
        print(f"Doxygen check failed: {error}", file=sys.stderr)
        sys.exit(1)
