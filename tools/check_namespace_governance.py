#!/usr/bin/env python3
"""Govern detail namespaces: what one may contain, and what it must be called.

`namespace detail` makes a claim -- everything inside is implementation, and a
caller who reaches in has no grounds to complain when it changes. It says nothing
about *why* the implementation is where it is, and that is the half that matters
when reading a header:

  * A detail namespace that carries template code has to be in a header. There is
    no choice in it: a template is instantiated at its point of use, so its
    definition has to be visible to whoever uses it, and "implementation detail"
    and "public header" stop being opposites. That case is spelled
    `TemplatedDetail` here, so the reason is in the name and a reader knows
    before opening the file that the internals are on show.

  * A detail namespace with no template code in it had a choice. In an
    implementation file the anonymous namespace already does the job -- internal
    linkage, no name to collide with, nothing for an includer to see -- and in a
    module unit a declaration that is not exported is already internal --
    tools/check_reflection_boundary.py forbids `detail` in module interface units
    outright, though only the lowercase spelling, so a capital `Detail` there
    lands here instead. In a header, non-template functions cost every
    translation unit that includes them and are a decision worth writing down.

So a plain `detail`/`Detail` is allowed only where a reason survives that, and
the reason goes in tools/namespace_allowlist.json next to the namespace it
excuses. That file is the point of this script: a detail namespace is a decision,
and the allowlist is where the decision is recorded -- the same deal
tools/macro_allowlist.json makes for `#define`.

Two passes, and neither needs a parser:

1. DECLARATIONS. Every namespace named `detail` or `Detail` is classified by its
   body: template code means it must be spelled `TemplatedDetail`, and no
   template code means it must be allowlisted. The reverse is checked as well,
   because a `TemplatedDetail` with no templates in it is a name that lies, and
   an allowlist entry that matches nothing is a decision nobody made.

2. REFERENCES. Every qualified use -- `TemplatedDetail::Foo`, `Detail::Foo`,
   `detail::Foo` -- has to name something a namespace of that spelling declares
   somewhere in the tree. This is what catches a rename half-finished: a
   reference left behind, or one renamed too far. It resolves by name rather than
   by scope, which is coarse -- the same name declared by two unrelated detail
   namespaces satisfies both -- but a wrong spelling never satisfies it.

Comments and string/character literals are stripped before scanning, so prose
about a namespace -- this docstring, a reason in the allowlist, a section banner
naming what follows -- cannot fail the check.

Usage:
    check_namespace_governance.py               # both passes, whole tree
    check_namespace_governance.py --file PATH   # one file (repeatable)
    check_namespace_governance.py --list        # print the inventory and exit
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ALLOWLIST = ROOT / "tools" / "namespace_allowlist.json"

SOURCE_ROOTS = (
    ROOT / "src",
    ROOT / "include",
    ROOT / "app",
    ROOT / "tests",
    ROOT / "samples",
    ROOT / "extras",
    ROOT / "modules",
    ROOT / "tools",
)
CXX_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".cppm",
    ".ixx",
    ".inl",
    ".ipp",
}
TRANSLATION_UNIT_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}
MODULE_UNIT_SUFFIXES = {".cppm", ".ixx"}


# Vendored from tools/check_macro_governance.py: comments and string literals
# mention namespaces (this file does) and must not count as declarations or uses.
def strip_comments_and_strings(text: str) -> str:
    """Replace comments and string/char literals with spaces, newlines kept."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n:
            if text[i + 1] == "/":
                while i < n and text[i] != "\n":
                    out[i] = " "
                    i += 1
                continue
            if text[i + 1] == "*":
                out[i] = out[i + 1] = " "
                i += 2
                while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                    if text[i] != "\n":
                        out[i] = " "
                    i += 1
                if i + 1 < n:
                    out[i] = out[i + 1] = " "
                    i += 2
                continue
        if c in "\"'":
            quote, out[i] = c, " "
            i += 1
            while i < n:
                if text[i] == "\\":
                    out[i] = " "
                    i += 1
                    if i < n:
                        out[i] = " "
                        i += 1
                    continue
                if text[i] == quote:
                    out[i] = " "
                    i += 1
                    break
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            continue
        i += 1
    return "".join(out)


def iter_sources(paths=None):
    # Always absolute: every report formats with path.relative_to(ROOT), and a
    # --file passed on the command line is relative to the caller's cwd.
    if paths:
        for p in paths:
            yield Path(p).resolve()
        return
    for root in SOURCE_ROOTS:
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if path.suffix.lower() in CXX_SUFFIXES and path.is_file():
                yield path.resolve()


def is_vendored(path: Path) -> bool:
    """extern/ and third_party/ are not ours to govern."""
    return bool({"extern", "third_party"} & set(path.parts))


# Template code: a template declaration of any kind, a concept, or a requires
# clause. All three mean the body cannot be moved into a translation unit.
TEMPLATE_CODE = re.compile(r"\btemplate[ \t]*<|\bconcept[ \t]+[A-Za-z_]\w*|\brequires\b")

ANY_NAMESPACE = re.compile(r"^[ \t]*(?:export[ \t]+)?(?:inline[ \t]+)?namespace[ \t]+([A-Za-z_][\w:]*)")

# What a namespace body declares at its own level, well enough to resolve a
# qualified reference. Deliberately loose: pass 2 asks whether some namespace of
# this spelling declares the name, so a name missed here reads as a dangling
# reference. Being loose costs a name accepted twice; being narrow costs a false
# alarm on code that is correct.
DECLARED_NAME = (
    re.compile(r"^[ \t]*(?:struct|class|union)[ \t]+([A-Za-z_]\w*)"),
    re.compile(r"^[ \t]*concept[ \t]+([A-Za-z_]\w*)"),
    re.compile(r"^[ \t]*(?:export[ \t]+)?using[ \t]+([A-Za-z_]\w*)"),
    re.compile(r"^[ \t]*(?:export[ \t]+)?(?:inline[ \t]+)?(?:constexpr|consteval|static)?[ \t]*"
               r"(?:[\w:]+(?:<[^;{=]*>)?(?:[ \t]*[*&])?[ \t]+)+?([A-Za-z_]\w*)[ \t]*(?:\(|\{|=|;)"),
    re.compile(r"^[ \t]*(?:\[\[[^\]]*\]\][ \t]*)+(?:inline[ \t]+)?(?:constexpr[ \t]+)?"
               r"(?:[\w:]+(?:<[^;{=]*>)?[ \t]+)*?([A-Za-z_]\w*)[ \t]*\("),
    # A name with no type in front of it: the return type went on the line above,
    # which clang-format does once a signature gets long. Only reachable at
    # namespace level, where a bare call cannot appear.
    re.compile(r"^[ \t]*([A-Za-z_]\w*)[ \t]*\("),
)
KEYWORDS = {
    "alignas", "alignof", "and", "asm", "auto", "bool", "case", "catch", "char", "class", "const",
    "constexpr", "consteval", "decltype", "default", "delete", "do", "double", "else", "enum",
    "explicit", "export", "extern", "false", "float", "for", "friend", "if", "inline", "int", "long",
    "namespace", "new", "noexcept", "not", "nullptr", "operator", "or", "private", "protected",
    "public", "register", "requires", "return", "short", "signed", "sizeof", "static", "static_assert",
    "struct", "switch", "template", "this", "throw", "true", "try", "typedef", "typename", "union",
    "unsigned", "using", "virtual", "void", "volatile", "while",
}


TEMPLATE_PREFIX = re.compile(r"^[ \t]*template[ \t]*<")


def strip_template_prefix(text: str) -> str:
    """Drop a leading `template <...>` so the declaration behind it can be read.

    Angle brackets are counted rather than searched for, because a template head
    can nest (`template <template <typename> class C>`) and a one-line
    `template <typename T> struct X {` is a declaration like any other.
    """
    match = TEMPLATE_PREFIX.match(text)
    if not match:
        return text
    depth, index = 0, match.end() - 1
    while index < len(text):
        if text[index] == "<":
            depth += 1
        elif text[index] == ">":
            depth -= 1
            if depth == 0:
                return text[index + 1:]
        index += 1
    return text


def declared_at_namespace_level(body: list) -> set:
    """Names this namespace declares itself, ignoring anything inside a function or type body.

    Brace kinds are tracked separately: a nested namespace's contents still sit at
    namespace level and count, while a local variable two braces down does not.
    A name missed here reads as a dangling reference in pass 2, so the patterns
    above stay loose about what a declaration looks like and this stays strict
    about where one has to be.
    """
    names, stack = set(), []
    for text in body:
        if stack.count("other") == 0:
            stripped = strip_template_prefix(text)
            for pattern in DECLARED_NAME:
                found = pattern.match(stripped)
                if found:
                    name = found.group(1)
                    if name not in KEYWORDS:
                        names.add(name)
                    break
            nested = ANY_NAMESPACE.match(stripped)
            if nested:
                names.update(nested.group(1).split("::"))
        for character in text:
            if character == "{":
                stack.append("ns" if ANY_NAMESPACE.match(text) else "other")
            elif character == "}" and stack:
                stack.pop()
    return names


class Namespace:
    """One detail-ish namespace: where it is, what it is spelled, what it declares."""

    def __init__(self, path: Path, line: int, spelling: str, enclosing: str, body: list):
        self.path = path
        self.line = line
        self.spelling = spelling                    # as written: detail, Detail, TemplatedDetail::JSON
        self.enclosing = enclosing                  # best effort, for reports and the allowlist
        self.body = "\n".join(body)
        self.names = declared_at_namespace_level(body)

    @property
    def root(self) -> str:
        return self.spelling.split("::")[0]

    @property
    def templated(self) -> bool:
        return bool(TEMPLATE_CODE.search(self.body))

    @property
    def qualified(self) -> str:
        return (self.enclosing + "::" if self.enclosing else "") + self.spelling

    def is_detail(self, spellings) -> bool:
        return self.root in spellings

    def why_it_is_wrong(self) -> str:
        """What this file offered instead of a named detail namespace."""
        suffix = self.path.suffix.lower()
        if suffix in TRANSLATION_UNIT_SUFFIXES:
            return (
                "a translation unit has the anonymous namespace right there, which gives these "
                "internal linkage with no name to collide with"
            )
        if suffix in MODULE_UNIT_SUFFIXES:
            return (
                "a module unit already keeps anything unexported inside the module, and its "
                "implementation units have the anonymous namespace on top of that"
            )
        return "a header makes every translation unit that includes it pay for these"


def enclosing_namespace(lines: list, index: int) -> str:
    """The namespace this line sits in, by walking up to the one still open."""
    balance = 0
    for text in reversed(lines[:index]):
        balance += text.count("}") - text.count("{")
        match = ANY_NAMESPACE.match(text)
        if match and balance < 0:
            return match.group(1)
    return ""


def collect(paths, spellings, templated_spelling):
    """Every detail-ish namespace in the tree, and every qualified reference to one."""
    names = "|".join(re.escape(s) for s in list(spellings) + [templated_spelling])
    declare = re.compile(
        r"^[ \t]*(?:export[ \t]+)?(?:inline[ \t]+)?namespace[ \t]+(?:[A-Za-z_]\w*::)*"
        r"((?:%s)(?:::[A-Za-z_]\w*)*)[ \t]*\{?[ \t]*$" % names
    )
    reference = re.compile(r"\b((?:%s))::([A-Za-z_]\w*)" % names)

    found, references, scanned = [], [], 0
    for path in iter_sources(paths):
        if is_vendored(path):
            continue
        scanned += 1
        relative = path.relative_to(ROOT)
        lines = strip_comments_and_strings(path.read_text(errors="ignore")).split("\n")
        for index, line in enumerate(lines):
            match = declare.match(line)
            if not match:
                continue
            cursor = index
            while cursor < len(lines) and "{" not in lines[cursor] and cursor - index < 3:
                cursor += 1
            depth, end, seen = 0, None, False
            for i in range(cursor, len(lines)):
                for character in lines[i]:
                    if character == "{":
                        depth += 1
                        seen = True
                    elif character == "}":
                        depth -= 1
                        if seen and depth == 0:
                            end = i
                            break
                if end is not None:
                    break
            # Skip the opening brace's own line: it is the declaration being
            # recorded, and reading it as a member would make every namespace
            # appear to declare its own name.
            body = lines[cursor + 1:end + 1] if end is not None else lines[cursor + 1:]
            found.append(Namespace(relative, index + 1, match.group(1), enclosing_namespace(lines, index), body))
        for index, line in enumerate(lines):
            for match in reference.finditer(line):
                references.append((relative, index + 1, match.group(1), match.group(2)))
    return found, references, scanned


def excuse(namespace, entries):
    """The allowlist entry that excuses this namespace, if there is one."""
    for position, entry in enumerate(entries):
        if entry.get("path") != str(namespace.path):
            continue
        wanted = entry.get("namespace", "")
        if wanted and not wanted.endswith(namespace.spelling):
            continue
        member = entry.get("member")
        if member and member not in namespace.names:
            continue
        return position, entry
    return None, None


def check_declarations(namespaces, entries, spellings, templated_spelling, violations, whole_tree):
    """Pass 1: the spelling has to match the body, and a plain one needs a reason."""
    templated = plain = excused = 0
    matched = set()

    for namespace in namespaces:
        if not namespace.is_detail(spellings):
            if namespace.templated:
                templated += 1
                continue
            violations.append(
                f"{namespace.path}:{namespace.line}: namespace {namespace.spelling} carries no template "
                f"code, so the name promises something the body does not have -- rename it to a plain "
                f"detail spelling and record the reason in tools/namespace_allowlist.json, or move it "
                f"into a translation unit"
            )
            continue

        if namespace.templated:
            violations.append(
                f"{namespace.path}:{namespace.line}: namespace {namespace.spelling} carries template code "
                f"-- rename it {templated_spelling}. A template is instantiated at its point of use, so "
                f"its definition has to be visible in a header; the name should say that instead of "
                f"claiming the internals are hidden"
            )
            continue

        plain += 1
        position, entry = excuse(namespace, entries)
        if entry is None:
            because = namespace.why_it_is_wrong()
            violations.append(
                f"{namespace.path}:{namespace.line}: namespace {namespace.spelling} wraps no template "
                f"code and has no entry in tools/namespace_allowlist.json -- {because}, so either move "
                f"them or write down why they stay"
            )
        else:
            excused += 1
            matched.add(position)

    # An entry matching nothing is only meaningful over the whole tree; with
    # --file the scan sees a slice, and every entry outside it would look stale.
    if not whole_tree:
        return templated, plain, excused

    for position, entry in enumerate(entries):
        if position in matched:
            continue
        missing = [key for key in ("path", "namespace", "reason") if not entry.get(key)]
        if missing:
            violations.append(
                f"tools/namespace_allowlist.json: entry {position} is missing {', '.join(missing)}"
            )
        else:
            violations.append(
                f"tools/namespace_allowlist.json: the entry for {entry['path']} ({entry['namespace']}) "
                f"matches no namespace in the tree -- remove it, or the allowlist is recording a "
                f"decision nobody made"
            )
    return templated, plain, excused


def check_references(references, namespaces, violations) -> int:
    """Pass 2: every qualified use names something that spelling declares."""
    declared: dict = {}
    for namespace in namespaces:
        names = declared.setdefault(namespace.root, set())
        names.update(namespace.names)
        # `TemplatedDetail::JSON::X` arrives as the pair (TemplatedDetail, JSON),
        # so a nested spelling's own segments are names of its first one.
        names.update(namespace.spelling.split("::")[1:])

    checked = 0
    for path, line, spelling, name in references:
        if spelling not in declared:
            continue
        checked += 1
        if name not in declared[spelling]:
            violations.append(
                f"{path}:{line}: {spelling}::{name} names nothing that any namespace spelled "
                f"'{spelling}' declares -- a rename half-finished, or a reach into internals that "
                f"moved somewhere else"
            )
    return checked


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--file",
        action="append",
        default=None,
        help="check only this file (repeatable); default scans the whole tree",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="print the inventory of detail namespaces and exit",
    )
    args = parser.parse_args()

    if not ALLOWLIST.is_file():
        print(f"Missing allowlist: {ALLOWLIST}", file=sys.stderr)
        return 1

    data = json.loads(ALLOWLIST.read_text(encoding="utf-8"))
    spellings = list(data.get("detail_spellings", ["detail", "Detail"]))
    templated_spelling = data.get("templated_spelling", "TemplatedDetail")
    entries = list(data.get("allowed_plain_detail", []))

    namespaces, references, scanned = collect(args.file, spellings, templated_spelling)

    if args.list:
        for namespace in sorted(namespaces, key=lambda n: (str(n.path), n.line)):
            kind = "templated" if namespace.templated else "plain    "
            note = ""
            if namespace.is_detail(spellings) and not namespace.templated:
                _, entry = excuse(namespace, entries)
                note = "  allowlisted" if entry else "  NOT ALLOWLISTED"
            print(f"{namespace.path}:{namespace.line}  {namespace.qualified}  [{kind}]{note}")
            print(f"    declares: {', '.join(sorted(namespace.names)) or '(nothing this script could read)'}")
        print(f"\n{len(namespaces)} namespaces, {len(references)} qualified references, {scanned} sources.")
        return 0

    violations: list = []
    templated, plain, excused = check_declarations(
        namespaces, entries, spellings, templated_spelling, violations, whole_tree=args.file is None
    )
    checked = check_references(references, namespaces, violations)

    if violations:
        print("Namespace governance violated:", file=sys.stderr)
        for violation in sorted(set(violations)):
            print(f"  - {violation}", file=sys.stderr)
        print(
            f"\nA detail namespace that carries template code is spelled {templated_spelling}: a template\n"
            "has to be visible where it is instantiated, and that is the only reason implementation belongs\n"
            "in a header at all. One that carries none had a choice -- a translation unit, an anonymous\n"
            "namespace, an unexported declaration in a module unit -- and if it still has to be where it is,\n"
            "the reason goes in allowed_plain_detail in tools/namespace_allowlist.json.",
            file=sys.stderr,
        )
        return 1

    scope = "1 file" if args.file and len(args.file) == 1 else f"{scanned} sources"
    print(
        f"Namespace governance OK ({scope}, {templated} {templated_spelling}, "
        f"{excused} of {plain} plain detail allowlisted, {checked} qualified references resolved)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
