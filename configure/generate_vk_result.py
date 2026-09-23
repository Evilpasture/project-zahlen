#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""Emit Vk::Result from the vendored headers' vk.xml.

Vk::Result (src/vulkan/core/RenderCore.hpp) mirrors VkResult one for one, minus
VK_SUCCESS: it is the category driver results travel under in the error channel,
because ErrorCode rejects any enum with a 0 enumerator and VK_SUCCESS is 0.
Hand-maintaining that mirror against header updates is exactly the kind of drift
this script exists to prevent -- it runs at CMake configure time (see
cmake/VkResultMirror.cmake) and emits the complete definition into the build tree
as vk/VkResult.hpp, which RenderCore.hpp includes at file scope. The include is
at file scope and not inside the enum on purpose: an in-namespace include would
trip check_reflection_boundary.py's no-include-in-namespace rule, whose rationale
(system headers declaring their names in the wrong namespace) does not apply to
a fragment but whose letter does.

Collection, in document order, three sections:

  * core: the <enum> children of <enums name="VkResult">, valued by `value`;
  * promoted: <enum extends="VkResult"> in <feature> blocks (core promotions
    carry their own `extnumber`, which is why they are found by scan rather
    than by extension number);
  * extensions: <enum extends="VkResult"> in <extension> blocks, valued by the
    standard offset formula -- -(1e9 + (ext-1)*1000 + offset) for dir="-",
    positive otherwise -- with the enclosing extension's number.

Skipped, loudly if violated: aliases (`alias` carries no value of its own),
anything with `bitpos` (a flag bit is not a result), and VK_SUCCESS itself,
whose value must still be 0 (the assert is the tripwire for a Khronos
renumbering that will never happen). Every other emitted value must be nonzero
-- the generation-time half of the channel's no-zero rule -- and both the
values and the derived enumerator names must be unique.

Names strip the VK_ERROR_/VK_ prefix and one trailing vendor tag (from vk.xml's
own <tags>, longest first), then PascalCase what remains: VK_ERROR_OUT_OF_DATE_KHR
becomes OutOfDate, VK_TIMEOUT becomes Timeout. Values are emitted as the Vulkan
constant itself (`= VK_ERROR_OUT_OF_DATE_KHR`), not the computed number, so an
enumerator can never disagree with the header it compiles against; the computed
numbers exist only for the uniqueness/nonzero asserts above.

Prose is configure/vk_result_prose.json (VK_* name -> sentence), with two
fallbacks: the vk.xml `comment` when one exists (core enums have them,
extension enums do not), else the bare VK spelling, which is still true if
not pretty. Either fallback prints a configure-time warning naming the code,
so a header update surfaces as prose to write rather than a build to fix; a
sidecar entry with no VkResult in vk.xml warns as stale. The generator appends
" (VK_NAME)" to sidecar and comment prose itself, so the log line always
carries the exact code (the error channel never prints raw ordinals).

Output is deterministic -- document order, no timestamps -- and every line is
asserted to fit the repository's 160-column limit, wrapping long entries onto
three lines the way a hand-written table would.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

COLUMN_LIMIT = 160
SUCCESS = "VK_SUCCESS"


def fail(message: str) -> None:
    print(f"generate_vk_result: error: {message}", file=sys.stderr)
    raise SystemExit(1)


def warn(message: str) -> None:
    print(f"generate_vk_result: warning: {message}", file=sys.stderr)


def extension_value(extnumber: int, offset: int, direction: str | None) -> int:
    base = 1_000_000_000 + (extnumber - 1) * 1000 + offset
    return -base if direction == "-" else base


def enumerator_name(vk: str, tags: set[str]) -> str:
    core = vk
    for prefix in ("VK_ERROR_", "VK_"):
        if core.startswith(prefix):
            core = core[len(prefix):]
            break
    else:
        fail(f"{vk}: no VK_ prefix to strip")
    for tag in sorted(tags, key=len, reverse=True):
        suffix = "_" + tag
        if core.endswith(suffix) and len(core) > len(suffix):
            core = core[: -len(suffix)]
            break
    parts = [word for word in core.split("_") if word]
    if not parts:
        fail(f"{vk}: nothing left after stripping")
    name = "".join(word[:1].upper() + word[1:].lower() for word in parts)
    if not name.isidentifier() or name[0].isdigit():
        fail(f"{vk}: derived {name!r} is not a C++ identifier")
    return name


def clean_comment(comment: str) -> str:
    text = re.sub(r"<<[^>]*>>", "", comment)  # asciidoc cross-references
    return re.sub(r"\s+", " ", text).strip()


def entry_lines(name: str, vk: str, prose: str) -> list[str]:
    full = f"{prose} ({vk})" if prose != vk else vk
    if '"' in full or "\\" in full or "\n" in full:
        fail(f"{vk}: prose is not a plain string literal: {full!r}")
    one = f'    {name} ZHLN_ANNOTATION(ZHLN::Description<"{full}"> {{}}) = {vk},'
    if len(one) <= COLUMN_LIMIT:
        return [one]
    if " (VK_" in full:
        head, tail = full.split(" (VK_", 1)
        tail = "(VK_" + tail
        column = 4 + len(name) + len(' ZHLN_ANNOTATION(ZHLN::Description<"')
        lines = [
            f'    {name} ZHLN_ANNOTATION(ZHLN::Description<"{head} "',
            " " * column + f'"{tail}"> {{}}) =',
            f"        {vk},",
        ]
        if all(len(line) <= COLUMN_LIMIT for line in lines):
            return lines
    fail(f"{vk}: entry does not fit {COLUMN_LIMIT} columns even wrapped; shorten its configure/vk_result_prose.json sentence")


def collect_entries(vk_xml: str) -> tuple[set[str], list[tuple[str, int, str | None]], list[tuple[str, int, str | None]], list[tuple[str, int, str | None]]]:
    """Collect (name, value, comment) triples for VkResult: core block first (document
    order), then <feature> promotions, then <extension> entries. Aliases carry no value
    of their own and are skipped. Raises ValueError naming the problem."""
    try:
        root = ET.parse(vk_xml).getroot()
    except ET.ParseError as exc:
        raise ValueError(f"cannot parse {vk_xml}: {exc}")
    except OSError as exc:
        raise ValueError(f"cannot read {vk_xml}: {exc}")

    tags = {tag.get("name") for tag in root.iter("tag") if tag.get("name")}

    enums_el = None
    for enums in root.iter("enums"):
        if enums.get("name") == "VkResult":
            enums_el = enums
    if enums_el is None:
        raise ValueError(f"{vk_xml}: no <enums name='VkResult'> block")
    core: list[tuple[str, int, str | None]] = []
    for enum in enums_el:
        if enum.tag != "enum" or "name" not in enum.attrib or "value" not in enum.attrib:
            continue
        core.append((enum.get("name"), int(enum.get("value"), 0), enum.get("comment")))
    if not core:
        raise ValueError(f"{vk_xml}: VkResult block holds no valued enums")

    parent = {child: elem for elem in root.iter() for child in elem}
    promoted: list[tuple[str, int, str | None]] = []
    extension: list[tuple[str, int, str | None]] = []
    for container_tag, bucket in (("feature", promoted), ("extension", extension)):
        for container in root.iter(container_tag):
            # Vulkan SC is a different API with its own headers (which this build does
            # not use): entries from vulkansc-only features and extensions -- and from
            # disabled extensions, which generate no headers at all -- name constants
            # that do not exist in vulkan_core.h, so they are not collected.
            gate = container.get("api") if container_tag == "feature" else container.get("supported")
            if gate is not None and "vulkan" not in gate.split(","):
                continue
            for require in container.iter("require"):
                if parent.get(require) is not container:
                    continue
                for enum in require.findall("enum"):
                    if enum.get("extends") != "VkResult":
                        continue
                    if "alias" in enum.attrib:
                        continue
                    if "bitpos" in enum.attrib:
                        raise ValueError(f"{enum.get('name')}: bitpos on a VkResult enum")
                    name = enum.get("name")
                    if "value" in enum.attrib:
                        value = int(enum.get("value"), 0)
                    else:
                        extnumber = enum.get("extnumber") or container.get("number")
                        if extnumber is None or "offset" not in enum.attrib:
                            raise ValueError(f"{name}: neither value nor offset/extnumber")
                        value = extension_value(int(extnumber), int(enum.get("offset")), enum.get("dir"))
                    bucket.append((name, value, enum.get("comment")))
    return tags, core, promoted, extension


def main() -> int:
    parser = argparse.ArgumentParser(description="Emit Vk::Result's enumerator table from vk.xml.")
    parser.add_argument("--vk-xml", required=True, help="path to the registry vk.xml")
    parser.add_argument("--prose", required=True, help="path to vk_result_prose.json")
    parser.add_argument("--output", required=True, help="path to write the enumerator table to")
    args = parser.parse_args()

    try:
        tags, core, promoted, extension = collect_entries(args.vk_xml)
    except ValueError as exc:
        fail(str(exc))

    collected = core + promoted + extension
    success = [value for name, value, _ in collected if name == SUCCESS]
    if not success:
        fail(f"{SUCCESS} vanished from vk.xml; the skip below would be a lie")
    if success != [0]:
        fail(f"{SUCCESS} is no longer 0; the channel's no-zero rule needs rethinking, not a tweak")
    entries = [(name, value, comment) for name, value, comment in collected if name != SUCCESS]
    for name, value, _ in entries:
        if value == 0:
            fail(f"{name} has value 0 and is not {SUCCESS}")
    seen_values: dict[int, str] = {}
    for name, value, _ in entries:
        if value in seen_values:
            fail(f"{name} and {seen_values[value]} share value {value}")
        seen_values[value] = name

    try:
        sidecar = {key: val for key, val in json.loads(Path(args.prose).read_text()).items() if not key.startswith("_")}
    except OSError as exc:
        fail(f"cannot read {args.prose}: {exc}")
    except json.JSONDecodeError as exc:
        fail(f"cannot parse {args.prose}: {exc}")
    for key, val in sidecar.items():
        if not isinstance(val, str) or not val:
            fail(f"{args.prose}: prose for {key} is not a nonempty string")
    collected_names = {name for name, _, _ in entries}
    for stale in sorted(set(sidecar) - collected_names):
        warn(f"{stale}: prose override names no VkResult in vk.xml (stale entry)")

    sections = (
        ("Core results", core),
        ("Promoted to core", promoted),
        ("Extension results", extension),
    )
    derived_sections: list[tuple[str, list[tuple[str, str, int, str | None]]]] = []
    seen_names: dict[str, str] = {}
    for title, section in sections:
        derived_section = []
        for name, value, comment in section:
            if name == SUCCESS:
                continue
            enum_name = enumerator_name(name, tags)
            if enum_name in seen_names:
                fail(f"{name} and {seen_names[enum_name]} derive the same enumerator {enum_name}")
            seen_names[enum_name] = name
            derived_section.append((enum_name, name, value, comment))
        derived_sections.append((title, derived_section))
    total = sum(len(section) for _, section in derived_sections)

    core_count, promoted_count, extension_count = (len(section) for _, section in derived_sections)
    lines = [
        "// DO NOT EDIT -- generated at configure time by configure/generate_vk_result.py",
        f"// from {args.vk_xml}: {total} entries ({core_count} core, {promoted_count} promoted, {extension_count} extension).",
        "",
        "#pragma once",
        "",
        "// The RHI's Vulkan-result vocabulary: every VkResult but VK_SUCCESS. This is the",
        "// category driver results travel under: VkResult itself cannot enter the error",
        "// channel because ErrorCode rejects any enum with a 0 enumerator, and VK_SUCCESS",
        "// is 0. ToFrameError static_casts into this enum, so the value word keeps the",
        "// driver's exact code (a result from a newer header than this table still",
        '// round-trips exactly and only loses its name, printing "Unknown") while Message()',
        "// gains the annotated prose. See src/vulkan/core/RenderCore.hpp.",
        "namespace ZHLN::Vk {",
        "",
        "enum class Result : int32_t {",
    ]
    fallback_count = 0
    for title, section in derived_sections:
        lines.append(f"    // --- {title}")
        for enum_name, vk, _, comment in section:
            if vk in sidecar:
                prose = sidecar[vk]
            elif comment:
                prose = clean_comment(comment)
                warn(f"{vk}: no prose override, using the vk.xml comment")
                fallback_count += 1
            else:
                prose = vk
                warn(f"{vk}: no prose override and no vk.xml comment, using the bare spelling")
                fallback_count += 1
            lines.extend(entry_lines(enum_name, vk, prose))

    lines += ["};", "", "} // namespace ZHLN::Vk"]

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines) + "\n")
    print(f"{len(entries)} VkResult entries -> {args.output} ({len(entries) - fallback_count} sidecar prose, {fallback_count} fallback)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
