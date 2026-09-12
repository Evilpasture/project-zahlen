#!/usr/bin/env python3
"""Govern `inline` in module units: where it is noise, and where it is load-bearing.

`inline` is a header word. It tells the linker that a definition may appear in
every translation unit that included the header, and that all of those copies are
one entity. A module unit is compiled once and imported by everything else, so
the question `inline` answers does not arise -- except in one place, where the
keyword stops being about headers and becomes the only thing giving a name
linkage at all. Three rules follow, and each was reproduced with
g++ 12.2 -fmodules-ts before it was written down here:

1. A function in a module unit takes no `inline`. Refused, in interface units and
   implementation units alike, at namespace scope and in a class body. The
   definition lives in the module's own object file and callers call it: an
   exported function links without the keyword, and so does an unexported one
   called from an exported template that importers instantiate -- the symbol is
   emitted under the module's mangling and the instantiation resolves against it.

2. A namespace-scope constant in an interface unit keeps its `inline`, and this
   check insists on it. `constexpr` makes a variable const, and a const-qualified
   variable at namespace scope has internal linkage unless something says
   otherwise, so without `inline` the name never reaches the interface: g++ does
   not diagnose it, it drops the declaration and every other translation unit --
   importer or the module's own implementation unit -- is told the name is not a
   member of its namespace. `export` on the declaration does not rescue it. This
   is the one spelling where the keyword is load-bearing, and losing it is
   silent, which is the worst way to lose it. An implementation unit is the
   mirror image: nothing it declares is visible elsewhere anyway, so `inline`
   there only widens a file-local constant to module linkage, where a second
   declaration of the same name in another unit of the module would be the same
   entity. Refused in both directions.

3. A static data member of a class in an interface unit is declared in the class
   and defined in the module purview. `inline static` -- a header's way to define
   one in-class -- emits no symbol for the definition, so the first importer that
   inlines a member body has an undefined reference to answer for; the same
   member defined out of line links. Refused in interface units. A class in an
   implementation unit is not exported and stays as it is.

Two things this check leaves alone, stated rather than hidden. A `static
constexpr` data member is implicitly inline and carries rule 3's hazard if an
importer odr-uses it, but it cannot be defined out of line and stay constexpr --
so use those as constants, which is what they are for, and not as objects. And
`static` at namespace scope in a module unit is the same header habit `inline`
is, internal linkage to keep definitions from colliding across includes, where
not exporting already hides the name; it is a separate question and this check
does not answer it.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

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


# Vendored from tools/check_macro_governance.py: a comment that says `inline` is
# prose about the keyword, not a use of it, and this file is full of both.
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
            quote, i = c, i + 1
            out[i - 1] = " "
            while i < n and text[i] != quote:
                if text[i] == "\\":
                    out[i] = " "
                    i += 1
                    if i < n and text[i] != "\n":
                        out[i] = " "
                        i += 1
                    continue
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            if i < n:
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


EXPORT_MODULE = re.compile(r"^[ \t]*export[ \t]+module[ \t]+[A-Za-z_][\w.:]*[ \t]*;", re.M)
MODULE_DECL = re.compile(r"^[ \t]*module[ \t]+[A-Za-z_][\w.:]*[ \t]*;", re.M)
GLOBAL_FRAGMENT = re.compile(r"^[ \t]*module[ \t]*;", re.M)

# What a line opens, in the order it is worth asking: a namespace (named or
# anonymous -- the anonymous one is where an implementation unit's helpers live,
# and it is namespace scope exactly like the named kind), then a class, then an
# enum, then a function. Anything else that opens a brace is a block, which is
# not namespace scope either but is not a declaration context.
NAMESPACE_OPEN = re.compile(r"^[ \t]*(?:export[ \t]+)?(?:inline[ \t]+)?namespace[ \t]+([A-Za-z_][\w:]*)")
ANON_NAMESPACE = re.compile(r"^[ \t]*(?:export[ \t]+)?(?:inline[ \t]+)?namespace[ \t]*\{")
CLASS_OPEN = re.compile(r"^[ \t]*(?:export[ \t]+)?(?:template[ \t]*<[^>;]*>[ \t]*)?(?:class|struct|union)[ \t]+([A-Za-z_]\w*)")
ENUM_OPEN = re.compile(r"^[ \t]*(?:export[ \t]+)?enum(?:[ \t]+(?:class|struct))?[ \t]*([A-Za-z_]\w*)?")
FUNCTIONISH = re.compile(r"^[ \t]*(?:export[ \t]+)?(?:template[ \t]*<[^>;]*>[ \t]*)?(?:\[\[[^\]]*\]\][ \t]*)*(?:inline[ \t]+)?(?:static[ \t]+)?(?:constexpr[ \t]+)?(?:consteval[ \t]+)?(?:noexcept[ \t]+)?[A-Za-z_~][\w:<>, *&]*[ \t]*[~A-Za-z_]\w*\(")

INLINE = re.compile(r"\binline\b")
# `inline` as a namespace specifier (`inline namespace detail`) is a different
# keyword use entirely and none of these rules is about it.
INLINE_NAMESPACE = re.compile(r"\binline[ \t]+namespace\b")
CONST_QUALIFIED = re.compile(r"\b(?:constexpr|const)\b")
EXTERN = re.compile(r"\bextern\b")


def module_kind(path: Path, text: str) -> str:
    """'interface', 'implementation' or '' for a file that is not a module unit.

    A .cppm/.ixx is a module unit by extension; a .cpp is one only if it declares
    a module, and `export module` is what makes a unit an interface.
    """
    suffix = path.suffix.lower()
    if suffix not in MODULE_UNIT_SUFFIXES and suffix not in TRANSLATION_UNIT_SUFFIXES:
        return ""
    if EXPORT_MODULE.search(text):
        return "interface"
    if MODULE_DECL.search(text):
        return "implementation"
    return "module" if suffix in MODULE_UNIT_SUFFIXES else ""


def purview_start(text: str) -> int:
    """Line index where the module purview begins.

    Everything in a global module fragment is ordinary header material -- the
    fragment is textually included, so `inline` there is doing its header job.
    """
    match = EXPORT_MODULE.search(text) or MODULE_DECL.search(text)
    if not match:
        return 0
    return text[: match.start()].count("\n")


class Declaration:
    """One declaration that carries, or ought to carry, `inline`."""

    def __init__(self, path, line, text, shape, has_inline, scope, exported):
        self.path = path
        self.line = line
        self.text = text.strip()
        self.shape = shape            # "function", "member", "constant" or "variable"
        self.has_inline = has_inline
        self.scope = scope            # "namespace", "class" or "function"
        self.exported = exported

    @property
    def kind(self) -> str:
        """How --list labels it, padded so the columns line up."""
        return {"function": "function ", "member": "member   ", "constant": "constant ", "variable": "variable "}[self.shape]


CLOSERS_ONLY = re.compile(r"^[\}\)\];,]+[ \t]*$")


def statement_complete(text: str) -> bool:
    """Whether a folded logical line has reached the end of its declaration.

    Parenthesis depth alone is not enough: a signature may put its name on the
    line below `[[nodiscard]] auto`, and its body brace below the return type.
    A line of nothing but closers is complete too, or `} // namespace` would
    swallow whatever follows it and be read at the wrong scope.
    """
    stripped = text.strip()
    if not stripped:
        return True
    return bool(CLOSERS_ONLY.match(stripped) or re.search(r"[{;]", stripped))


def logical_lines(lines: list) -> list:
    """(first physical line index, declaration text) with wrapped heads folded.

    A signature spread over five lines is one declaration, and treating each line
    as one reads a parameter -- `const JPH::Mat44& authored,` -- as a
    const-qualified variable at namespace scope. Folding also puts the body brace
    of a wrapped signature back on the line that declares the function, so the
    block it opens is the right kind of block.
    """
    out, buffer, start, depth = [], [], 0, 0
    for index, line in enumerate(lines):
        if not buffer:
            start = index
        buffer.append(line.strip())
        depth = max(0, depth + line.count("(") - line.count(")"))
        text = " ".join(part for part in buffer if part)
        if depth == 0 and statement_complete(text):
            out.append((start, text))
            buffer, depth = [], 0
    if buffer:
        out.append((start, " ".join(part for part in buffer if part)))
    return out


def scope_of(stack: list) -> str:
    """Namespace scope only if every open block is a namespace."""
    if any(frame[0] == "function" for frame in stack):
        return "function"
    if any(frame[0] == "class" for frame in stack):
        return "class"
    if any(frame[0] == "block" for frame in stack):
        return "block"
    return "namespace"


def declarator_of(line: str) -> str:
    """The part of a declaration before its body, initializer or terminator."""
    without_inline = INLINE.sub("", line, count=1)
    cut = re.search(r"[{;=]", without_inline)
    return without_inline[: cut.start()] if cut else without_inline


def shape_of(line: str, in_class: bool) -> str:
    """Function, data member, constant or plain variable -- from the declarator.

    A member function is reported as a function: rule 1 governs it the same way
    it governs one at namespace scope. Only a data member is a "member", and only
    `static` makes one, since a non-static member cannot be declared with a
    definition in the class body at all.
    """
    declarator = declarator_of(line)
    if "(" in declarator:
        return "function"
    if in_class and re.search(r"\bstatic\b", declarator):
        return "member"
    if CONST_QUALIFIED.search(declarator):
        return "constant"
    return "variable"


def collect(paths):
    """Every `inline` in a module unit, and every namespace-scope constant.

    Rule 2 runs in both directions, so a constant is collected whether or not it
    has the keyword: the check is as interested in the one that is missing it as
    in the one that has it.
    """
    found: list = []
    scanned = 0
    units = {"interface": 0, "implementation": 0}
    for path in iter_sources(paths):
        if is_vendored(path):
            continue
        text = path.read_text(errors="ignore")
        kind = module_kind(path, text)
        if not kind or kind not in units:
            continue
        scanned += 1
        units[kind] += 1
        relative = path.relative_to(ROOT) if path.is_relative_to(ROOT) else path
        clean = strip_comments_and_strings(text)
        lines = clean.split("\n")
        start = purview_start(clean)
        statements = [(index, line) for index, line in logical_lines(lines) if index >= start]

        # A stack of open blocks, so a line's scope is known: namespace frames
        # are still namespace scope, anything else is not.
        stack: list = []
        for index, line in statements:
            scope = scope_of(stack)
            # A one-line class (`struct State { inline static int s = 0; };`) opens
            # its body on the line that declares the member, so the stack does not
            # know yet: the brace position says which side of it the line is on.
            brace = line.find("{")
            one_line_class = False
            if scope == "namespace" and CLASS_OPEN.match(line) and brace >= 0:
                inline_at = INLINE.search(line)
                if inline_at and inline_at.start() > brace:
                    scope, one_line_class = "class", True
            in_class = scope == "class"
            exported = any(frame[0] == "namespace" and frame[2] for frame in stack) or line.startswith("export ")
            has_inline = bool(INLINE.search(line)) and not INLINE_NAMESPACE.search(line)

            if has_inline and scope in ("namespace", "class"):
                # For a class written on one line the declarator is the tail past
                # the brace; read from the head and it is the class's own name.
                shape_source = line[brace + 1 :] if one_line_class else line
                found.append(Declaration(relative, index + 1, line, shape_of(shape_source, in_class), True, scope, exported))
            elif scope == "namespace" and kind == "interface" and not has_inline:
                # Rule 2's other direction: a constant that lost its `inline`.
                declarator = declarator_of(line)
                if (
                    "(" not in declarator
                    and CONST_QUALIFIED.search(declarator)
                    and not EXTERN.search(declarator)
                    and not re.search(r"\bstatic\b", declarator)
                    and not line.startswith(("using", "static_assert", "return", "case", "namespace", "#"))
                ):
                    found.append(Declaration(relative, index + 1, line, "constant", False, scope, exported))

            opens, closes = line.count("{"), line.count("}")
            if opens:
                named = NAMESPACE_OPEN.match(line)
                if named:
                    stack.append(("namespace", named.group(1), line.lstrip().startswith("export ")))
                    opens -= 1
                elif ANON_NAMESPACE.match(line):
                    stack.append(("namespace", "(anonymous)", line.lstrip().startswith("export ")))
                    opens -= 1
                elif CLASS_OPEN.match(line):
                    stack.append(("class", CLASS_OPEN.match(line).group(1), exported))
                    opens -= 1
                elif ENUM_OPEN.match(line):
                    stack.append(("class", f"enum {ENUM_OPEN.match(line).group(1) or ''}".strip(), exported))
                    opens -= 1
                elif FUNCTIONISH.match(line):
                    stack.append(("function", "", exported))
                    opens -= 1
                for _ in range(opens):
                    stack.append(("block", "", exported))
            for _ in range(closes):
                if stack:
                    stack.pop()
    return found, scanned, units


def check(declarations, kind_of, violations) -> int:
    """Apply the three rules, returning how many constants kept their `inline`."""
    constants = 0
    for declaration in declarations:
        path, line, shape = declaration.path, declaration.line, declaration.shape
        unit = kind_of[path]
        if shape == "function":
            if declaration.has_inline:
                violations.append(
                    f"{path}:{line}: `inline` on a function in a module {unit} unit -- the module is "
                    f"compiled once, so there is no second translation unit for the keyword to reconcile"
                )
            else:
                pass                            # a function with no `inline`: what rule 1 asks for
        elif shape == "member":
            if declaration.has_inline and unit == "interface":
                violations.append(
                    f"{path}:{line}: `inline` static data member in a module interface unit -- no symbol "
                    f"is emitted for it, so an importer that inlines a member body is left with an "
                    f"undefined reference; declare it in the class and define it in the module purview"
                )
            elif declaration.has_inline:
                pass                            # a member in an implementation unit: nobody imports it
        elif shape == "constant":
            if unit == "interface" and not declaration.has_inline:
                violations.append(
                    f"{path}:{line}: a const-qualified variable at namespace scope has internal linkage, "
                    f"so this one is not in the module's interface at all -- no importer and no "
                    f"implementation unit of the module can name it; `inline` (or `extern`) is what gives "
                    f"it linkage"
                )
            elif unit == "interface":
                constants += 1
            elif declaration.has_inline:
                violations.append(
                    f"{path}:{line}: `inline` on a constant in a module implementation unit -- nothing "
                    f"here is visible to another unit either way, and the keyword widens a file-local "
                    f"constant to module linkage, where the same name in another unit of the module is "
                    f"the same entity"
                )
        elif declaration.has_inline:
            violations.append(
                f"{path}:{line}: `inline` on a variable in a module {unit} unit -- the variable already "
                f"has the linkage the module gives it, and the keyword adds nothing"
            )
    return constants


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
        help="print every `inline` and namespace-scope constant in a module unit, and exit",
    )
    args = parser.parse_args()

    declarations, scanned, units = collect(args.file)
    kind_of = {}
    for path in iter_sources(args.file):
        if is_vendored(path):
            continue
        text = path.read_text(errors="ignore")
        kind = module_kind(path, text)
        if kind in ("interface", "implementation"):
            relative = path.relative_to(ROOT) if path.is_relative_to(ROOT) else path
            kind_of[relative] = kind

    if args.list:
        for declaration in sorted(declarations, key=lambda d: (str(d.path), d.line)):
            state = "has inline" if declaration.has_inline else "no inline "
            print(f"{declaration.path}:{declaration.line}  [{declaration.kind} {state}]  {declaration.text[:88]}")
        print(
            f"\n{len(declarations)} declarations, {scanned} module units "
            f"({units['interface']} interface, {units['implementation']} implementation)."
        )
        return 0

    violations: list = []
    constants = check(declarations, kind_of, violations)

    if violations:
        print("Module linkage violated:", file=sys.stderr)
        for violation in sorted(set(violations)):
            print(f"  - {violation}", file=sys.stderr)
        print(
            "\nA module unit is compiled once, so `inline` on a function or a variable in one is a header\n"
            "habit with nothing to reconcile. Two spellings are the opposite: a namespace-scope constant in\n"
            "an interface unit needs `inline`, because const-qualified means internal linkage and an\n"
            "internal-linkage name never reaches the interface; and a static data member of an exported\n"
            "class is declared in the class and defined in the module purview, because `inline static`\n"
            "emits no symbol and leaves importers with an undefined reference.",
            file=sys.stderr,
        )
        return 1

    scope = "1 file" if args.file and len(args.file) == 1 else f"{scanned} module units"
    print(
        f"Module linkage OK ({scope}: {units['interface']} interface, {units['implementation']} "
        f"implementation; {constants} namespace-scope constants keep the `inline` that gives them "
        f"linkage, and nothing else in a module unit carries one)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
