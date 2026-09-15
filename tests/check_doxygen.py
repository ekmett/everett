#!/usr/bin/env python3
##
# \file
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense

"""Generate API/Markdown documentation and verify Doxygen associations and math."""

import argparse
from collections import Counter
import html as html_module
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
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


def metadata_fields(path, split=False):
    source = path.read_text(encoding="utf-8")
    blocks = [block for block in re.finditer(r"/\*\*(.*?)\*/", source, re.S)
              if re.search(r"\\file\s", block.group(1))]
    require(bool(blocks), f"Missing file metadata: {path}")
    metadata = "\n".join(block.group(1) for block in blocks)
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
    if split:
        require(len(blocks) == 2, f"Expected separate leading/trailing file blocks: {path}")
        head, tail = blocks
        require(not source[:head.start()].strip(), f"SPDX header must precede code: {path}")
        require(not source[tail.end():].strip(), f"File brief/author must follow code: {path}")
        require(all(notice in head.group(1) for notice in notices), f"SPDX notices must be in header: {path}")
        require("\\license" in head.group(1) and "\\endlicense" in head.group(1),
                f"Missing SPDX block delimiters: {path}")
        require("\\brief" not in head.group(1) and "\\author" not in head.group(1),
                f"Brief/author must remain in footer: {path}")
        require("SPDX-" not in tail.group(1) and "\\license" not in tail.group(1)
                and "\\endlicense" not in tail.group(1), f"License repeated in footer: {path}")
    normalize = lambda value: " ".join(value.replace("<", "").replace(">", "").split())
    return normalize(briefs[0]), normalize(authors[0]), [" ".join(value.split()) for value in notices]


def check_file_metadata(items, headers, aliases=True, split=False, markdown=()):
    files = {item.findtext("compoundname"): item for item in items
             if item.attrib["kind"] == "file"}
    require(set(files) == {path.name for path in [*headers, *markdown]}, "Unexpected documented file set")
    briefs = []
    for path in headers:
        brief, author, notices = metadata_fields(path, split=split)
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
                              ("everett::mapped_profile", ["P", "Role"]), ("everett::encoded_sections", ["P"])):
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
                  " * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>",
                  " * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0",
                  r" * \endlicense", r" * \author Edward Kmett <ekmett@gmail.com>",
                  f" * \\brief File {filename} marker.", " */"]
        if placement == "split":
            head = footer[:7] + [" */"]
            tail = ["/**", r" * \file"] + footer[7:]
            complete = head + lines + tail
        else:
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
                # Even GITHUB mode prefixes number-leading section IDs in
                # Doxygen 1.9.8. Resolve against the actual generated ID.
                anchors = [(element.get("id"), spelling)
                           for spelling in (fragment, "autotoc_md" + fragment)
                           for element in target.iter()
                           if element.get("id", "").endswith("_1" + spelling)]
                require(len(anchors) == 1, f"Missing or ambiguous Markdown heading: {name}: {url}")
                (refid, fragment), kind = anchors[0], "member"
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
                                     ("docs/keys.md", "docs/arrows.md"),
                                     ("docs/sampling.md", "docs/durability.md"),
                                     ("docs/network-admission.md", "docs/rebuild.md")):
        check_page_link(pages, source_name, target_name, output)
    for anchor in ("examples", "field-guide", "building"):
        check_page_anchor(pages, "README.md", anchor, output)
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

## Details

An ordinary paragraph.
""", encoding="utf-8")
    child = directory / "docs/child.md"
    child.write_text("# Child page\n\n[Home](../README.md#details). Formula $q^2$.\n\n## 7. Numbered section\n", encoding="utf-8")
    (directory / "AGENTS.md").write_text("# Guidance\n", encoding="utf-8")
    (directory / "THIRD_PARTY.md").write_text(
        "<!-- A leading attribution notice. -->\n\nThird-party notices\n===================\n\nFixture text.\n",
        encoding="utf-8")
    (directory / "proof/.lake/generated").mkdir(parents=True, exist_ok=True)
    (directory / "proof/README.md").write_text("# Proof notes\n\n[Home](../README.md).\n", encoding="utf-8")
    (directory / "proof/.lake/generated/README.md").write_text("# Not an input\n", encoding="utf-8")
    inputs = markdown_inputs(directory)
    require({path.relative_to(directory).as_posix() for path in inputs} ==
            {"README.md", "AGENTS.md", "docs/child.md", "proof/README.md", "THIRD_PARTY.md"}, "Wrong Markdown input discovery")
    generated = output / "markdown-fixture-docs"
    run_doxygen(executable, directory, inputs, generated, aliases=True, html=True, markdown_main=readme)
    items = compounds(generated)
    repair_markdown_links(items, directory, generated)
    items = compounds(generated)
    pages = markdown_pages(items, directory)
    require(set(pages) == {"README.md", "AGENTS.md", "docs/child.md", "proof/README.md", "THIRD_PARTY.md"}, "Fixture pages missing")
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
    check_file_metadata(baseline_items, headers, aliases=False, split=True)
    check_actual_members(baseline_items, source)

    check_markdown_adapter()
    markdown = markdown_inputs(source)
    require(all(path.is_file() for path in markdown), "Missing Markdown input")
    reference = output / "reference"
    run_doxygen(args.doxygen, source, [*headers, *markdown], reference, aliases=True, html=True,
                markdown_main=source / "README.md")
    items = compounds(reference)
    check_file_metadata(items, headers, split=True, markdown=markdown)
    member_count = check_actual_members(items, source)
    repaired_links = repair_markdown_links(items, source, reference)
    items = compounds(reference)
    source_files = bundle_source_links(items, source, reference)
    items = compounds(reference)
    formula_count = check_markdown_pages(items, source, markdown, reference)
    check_markdown_fixture(args.doxygen, output)
    fixture_results = []
    for placement in ("before", "after", "split"):
        inputs = output / ("fixture-" + placement)
        expected = make_fixtures(inputs, placement)
        generated = output / ("fixture-" + placement + "-docs")
        run_doxygen(args.doxygen, source, [inputs], generated, aliases=True)
        items = compounds(generated)
        check_file_metadata(items, sorted(inputs.glob("*.h")), split=placement == "split")
        fixture_results.append(check_fixtures(items, expected))
    require(all(result == fixture_results[0] for result in fixture_results[1:]),
            "Moving or splitting file metadata changed symbol documentation")
    print(f"Checked {len(headers)} headers, {member_count} real function/overload associations, "
          "and twelve fixture symbols with file metadata before/after/split around declarations.")
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


##
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Tests Everett's Doxygen metadata and declaration associations.
