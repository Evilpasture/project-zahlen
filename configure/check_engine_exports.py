#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later
"""Audit libzahlen_engine symbol visibility, export allowlist, and linked DSO .dynsym.

Configure-time mode (no arguments):
  1. Forbids ZHLN_API outside include/Zahlen/ (static/source consumer layers use
     ZHLN_STATIC_API; internal src/ headers stay unannotated).
  2. Extracts every ZHLN_API function and out-of-line class member declared in
     include/Zahlen/, rejects internal engine/ECS/render/physics/audio/Jolt names,
     and enforces exact bidirectional parity with cmake/zahlen_engine.map (while
     verifying private out-of-line implementation helpers stay unexported).
  3. Verifies CMake hidden-visibility presets, -Wl,--exclude-libs,ALL, version
     script wiring, OBJECT linkage for ZHLN_API sub-libraries, mimalloc host-only
     scoping, and zero private-archive links in consumer directories.

Binary audit mode (--dso path/to/libzahlen_engine.{so,dylib}):
  Inspects the linked DSO's dynamic symbol table via `nm` and asserts that every
  defined export matches cmake/zahlen_engine.map, zero STB_GNU_UNIQUE / internal /
  third-party / allocator / stdlib symbols leak, and the COM factory entry points
  plus cross-DSO error-registry anchor are present.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
VERSION_SCRIPT = ROOT / "cmake" / "zahlen_engine.map"
MIMALLOC_LIST = ROOT / "cmake" / "mimalloc_override.list"

SOURCE_SUFFIXES = frozenset({
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".cppm", ".ixx",
})

NON_PUBLIC_DIRS = (
    "src", "modules", "plugins", "extensions", "gameplay", "utils", "samples", "tests", "app",
)

CONSUMER_DIRS = (
    "app", "extensions", "plugins", "gameplay", "modules", "utils", "samples", "tests",
)

PRIVATE_ENGINE_TARGETS = (
    "zahlen_render",
    "zahlen_vulkan",
    "zahlen_window",
    "zahlen_physics",
    "zahlen_threading",
    "zahlen_filesystem",
    "zahlen_ecs",
    "zahlen_audio",
    "zahlen_miniaudio",
    "zahlen_presentation",
    "zahlen_shader_bytecode",
    "HostBlitSwapchain",
    "Jolt",
    "mimalloc-static",
    "spirv-reflect-static",
)

API_OBJECT_TARGETS = ("zahlen_threading", "zahlen_filesystem")

FORBIDDEN_INTERNAL_SCOPES = frozenset({
    "Engine",
    "World",
    "KernelSession",
    "AssetManager",
    "FramePlan",
    "PrefabFactory",
    "InputContext",
    "ArticulationContext",
    "RenderSystem",
    "CullingSystem",
    "RenderContext",
    "PhysicsContext",
    "AudioContext",
    "ECS",
    "Vk",
    "JPH",
})

# Standard ABI bookkeeping symbols that older ELF/Mach-O toolchains occasionally
# leave defined even under `local: *;`.
LINKER_BOOKKEEPING_SYMBOLS = frozenset({
    "_init", "_fini", "__bss_start", "_edata", "_end", "__dso_handle",
})

ALLOCATOR_OVERRIDE_SYMBOLS = frozenset({
    "malloc", "calloc", "realloc", "free", "cfree",
    "aligned_alloc", "posix_memalign", "_aligned_malloc", "memalign",
    "valloc", "pvalloc", "vfree", "reallocarray", "reallocf", "reallocarr",
    "malloc_size", "malloc_good_size", "malloc_usable_size",
    "strdup", "strndup",
    "__libc_malloc", "__libc_calloc", "__libc_realloc", "__libc_free",
    "__libc_cfree", "__libc_memalign", "__libc_valloc", "__libc_pvalloc",
    "__posix_memalign",
})

FORBIDDEN_SYMBOL_REGEXES: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("Engine::*", re.compile(r"^_ZN(?:K|O)?4ZHLN6Engine[0-9CD]")),
    ("World::*", re.compile(r"^_ZN(?:K|O)?4ZHLN5World[0-9CD]")),
    ("KernelSession::*", re.compile(r"^_ZN(?:K|O)?4ZHLN13KernelSession[0-9CD]")),
    ("AssetManager::*", re.compile(r"^_ZN(?:K|O)?4ZHLN12AssetManager[0-9CD]")),
    ("FramePlan::*", re.compile(r"^_ZN(?:K|O)?4ZHLN9FramePlan[0-9CD]")),
    ("PrefabFactory::*", re.compile(r"^_ZN(?:K|O)?4ZHLN13PrefabFactory[0-9CD]")),
    ("InputContext::*", re.compile(r"^_ZN(?:K|O)?4ZHLN12InputContext[0-9CD]")),
    ("ArticulationContext::*", re.compile(r"^_ZN(?:K|O)?4ZHLN19ArticulationContext[0-9CD]")),
    ("RenderSystem::*", re.compile(r"^_ZN(?:K|O)?4ZHLN12RenderSystem[0-9CD]")),
    ("CullingSystem::*", re.compile(r"^_ZN(?:K|O)?4ZHLN13CullingSystem[0-9CD]")),
    ("RenderContext::*", re.compile(r"^_ZN(?:K|O)?4ZHLN13RenderContext[0-9CD]")),
    ("PhysicsContext::*", re.compile(r"^_ZN(?:K|O)?4ZHLN14PhysicsContext[0-9CD]")),
    ("AudioContext::*", re.compile(r"^_ZN(?:K|O)?4ZHLN12AudioContext[0-9CD]")),
    ("ECS::*", re.compile(r"^_ZN(?:K|O)?4ZHLN3ECS[0-9]")),
    ("Vk::*", re.compile(r"^_ZN(?:K|O)?4ZHLN2Vk[0-9]")),
    ("JPH::*", re.compile(r"^_Z(?:N|NK|TVN|TIN|TSN|GVN)3JPH[0-9]")),
    ("miniaudio (ma_*)", re.compile(r"^ma_")),
    ("mimalloc (mi_*)", re.compile(r"^_?mi_")),
    ("meshoptimizer (meshopt_*)", re.compile(r"^meshopt_")),
    ("Clay (Clay_*)", re.compile(r"^Clay_")),
    ("VMA (vma* / Vma*)", re.compile(r"^(?:vma[A-Z]|_Z[A-Z]*[0-9]+Vma)")),
    ("volk (volk*)", re.compile(r"^volk[A-Z_]")),
    ("SPIRV-Reflect (spvReflect*)", re.compile(r"^spvReflect")),
    ("GLFW (glfw* / _glfw*)", re.compile(r"^_?glfw")),
    ("zstd (ZSTD_* / FSE_* / HUF_*)", re.compile(r"^(?:ZSTD|FSE|HUF|ZDICT)_")),
    ("C++ operator new/delete", re.compile(r"^_Z(?:nw|na|dl|da)")),
    ("C++ stdlib (_ZSt* / _ZNSt*)", re.compile(r"^_Z(?:St|NSt|NKSt|TVNSt|TINSt|TSNSt|GVNSt)")),
    ("C++ vtable/typeinfo (_ZTV/_ZTI/_ZTS)", re.compile(r"^_Z(?:TV|TI|TS|TT)")),
)

# Synthetic mangled symbols covering every forbidden internal class, private
# helper of a ZHLN_API class, allocator override, C++ stdlib template, and
# third-party library; none may match any pattern in cmake/zahlen_engine.map.
FORBIDDEN_CANARY_SYMBOLS: tuple[str, ...] = (
    "_ZN4ZHLN6Engine6CreateERKNS_12EngineConfigE",
    "_ZN4ZHLN5World4StepEf",
    "_ZN4ZHLN13KernelSession4InitEv",
    "_ZN4ZHLN12AssetManager4LoadEm",
    "_ZN4ZHLN9FramePlan5BuildEv",
    "_ZN4ZHLN13PrefabFactory11InstantiateEm",
    "_ZN4ZHLN12InputContext4PollEv",
    "_ZN4ZHLN19ArticulationContext6UpdateEf",
    "_ZN4ZHLN12RenderSystem7ExecuteEv",
    "_ZN4ZHLN13CullingSystem4CullEv",
    "_ZN4ZHLN13RenderContext10BeginFrameEv",
    "_ZN4ZHLN14PhysicsContext4StepEf",
    "_ZN4ZHLN12AudioContext4PlayEj",
    "_ZN4ZHLN3ECS8Registry6CreateEv",
    "_ZN4ZHLN2Vk8Instance6CreateEv",
    "_ZN4ZHLN2FS5Paths9IsDevTreeEv",
    "_ZN4ZHLN2FS5Paths17PipelineCacheFileB5cxx11Ev",
    "_ZN4ZHLN2FS5Paths13CrashDumpFileB5cxx11Ev",
    "_ZN4ZHLN2FS17VirtualFileSystem11ExecuteLoadEPNS0_11LoadRequestE",
    "_ZNK4ZHLN2FS17VirtualFileSystem19LoadFromDirectoriesERNS0_11LoadRequestE",
    "_ZNK4ZHLN2FS17VirtualFileSystem24IndexDirectoryTreeLockedERKNSt10filesystem7__cxx114pathE",
    "_ZNK4ZHLN2FS17VirtualFileSystem27RebuildDirectoryIndexLockedEv",
    "_ZN4ZHLN2FS17FileSystemWatcher4Impl3RunEv",
    "_ZN4ZHLN3COM12CreateEngine6NestedEv",
    "_ZN4ZHLN3COM18CreateEngineHelperEv",
    "_ZN3JPH13PhysicsSystem4InitEjjjj",
    "ma_engine_init",
    "mi_malloc",
    "_mi_process_init",
    "malloc",
    "free",
    "_Znwm",
    "_ZdlPv",
    "meshopt_buildMeshlets",
    "Clay_Initialize",
    "vmaCreateAllocator",
    "_ZN13VmaJsonWriterC1Ev",
    "volkInitialize",
    "spvReflectCreateShaderModule",
    "glfwInit",
    "_glfwPlatformInit",
    "ZSTD_decompress",
    "FSE_readNCount",
    "HUF_decompress",
    "_ZNSt6vectorIiSaIiEE17_M_realloc_appendIJiEEEvDpOT_",
    "_ZTVN4ZHLN3COM7IEngineE",
)

REQUIRED_DSO_PREFIXES: tuple[tuple[str, str], ...] = (
    ("ZHLN::COM::CreateEngine", "_ZN4ZHLN3COM12CreateEngine"),
    ("ZHLN::COM::CreateLogicWorld", "_ZN4ZHLN3COM16CreateLogicWorld"),
    ("ZHLN::COM::RenderGraphDump", "_ZN4ZHLN3COM15RenderGraphDump"),
    ("ZHLN::TemplatedDetail::RegisterEnumCategory", "_ZN4ZHLN15TemplatedDetail20RegisterEnumCategory"),
    ("ZHLN::TemplatedDetail::LookupErrorStrings", "_ZN4ZHLN15TemplatedDetail18LookupErrorStrings"),
    ("ZHLN::RegisterErrorCategory", "_ZN4ZHLN21RegisterErrorCategory"),
)

_COMMENT_OR_STR_RE = re.compile(
    r'//[^\n]*|/\*.*?\*/|R"([A-Za-z0-9_]{0,16})\(.*?\)\1"|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
    re.S,
)


def strip_comments_and_strings(text: str) -> str:
    def _repl(match: re.Match[str]) -> str:
        tok = match.group(0)
        if tok.startswith(('"', "'", 'R"')):
            return '""' + ("\n" * tok.count("\n"))
        return " " + ("\n" * tok.count("\n"))

    return _COMMENT_OR_STR_RE.sub(_repl, text)


def glob_to_regex(pattern: str) -> re.Pattern[str]:
    """Translate an ELF linker version-script glob (`*`, `?`, `[...]`) to a full-match regex."""
    out = ["^"]
    i = 0
    n = len(pattern)
    while i < n:
        ch = pattern[i]
        if ch == "*":
            out.append(".*")
        elif ch == "?":
            out.append(".")
        elif ch == "[":
            j = pattern.find("]", i + 1)
            if j == -1:
                raise ValueError(f"unterminated '[' in version-script pattern: {pattern}")
            cls = pattern[i + 1 : j]
            out.append("[" + re.escape(cls) + "]")
            i = j
        else:
            out.append(re.escape(ch))
        i += 1
    out.append("$")
    return re.compile("".join(out))


@dataclass(frozen=True)
class VersionRule:
    line: int
    pattern: str
    api_tag: str
    regex: re.Pattern[str]


def parse_version_script(path: Path, errors: list[str]) -> list[VersionRule]:
    if not path.is_file():
        errors.append(f"Missing ELF version script: {path.relative_to(ROOT).as_posix()}")
        return []
    text = path.read_text(encoding="utf-8")
    rules: list[VersionRule] = []
    section = ""
    saw_local_wildcard = False

    # Capture optional inline `/* @api ... */` annotations before stripping comments.
    api_by_line: dict[int, str] = {}
    for lineno, raw in enumerate(text.splitlines(), 1):
        m = re.search(r"/\*\s*@api\s+([^*]+?)\s*\*/", raw)
        if m:
            api_by_line[lineno] = m.group(1).strip()

    cleaned = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    for lineno, line in enumerate(cleaned.splitlines(), 1):
        s = line.strip()
        if not s or s in ("{", "};", "}"):
            continue
        if s == "global:":
            section = "global"
            continue
        if s == "local:":
            section = "local"
            continue
        if not s.endswith(";"):
            errors.append(f"cmake/zahlen_engine.map:{lineno}: malformed directive: {s}")
            continue
        pat = s[:-1].strip()
        if section == "local":
            if pat == "*":
                saw_local_wildcard = True
            else:
                errors.append(f"cmake/zahlen_engine.map:{lineno}: unexpected local pattern '{pat}' (expected '*;')")
        elif section == "global":
            if not re.match(r"^_Z(?:N|NK|NO)4ZHLN[0-9A-Za-z_\[\]\*]+$", pat):
                errors.append(
                    f"cmake/zahlen_engine.map:{lineno}: global pattern '{pat}' must be a scoped Itanium ZHLN symbol"
                )
                continue
            api_tag = api_by_line.get(lineno, "")
            if not api_tag:
                errors.append(f"cmake/zahlen_engine.map:{lineno}: missing /* @api ZHLN::... */ annotation on '{pat}'")
            try:
                rx = glob_to_regex(pat)
            except ValueError as exc:
                errors.append(f"cmake/zahlen_engine.map:{lineno}: {exc}")
                continue
            rules.append(VersionRule(line=lineno, pattern=pat, api_tag=api_tag, regex=rx))
        else:
            errors.append(f"cmake/zahlen_engine.map:{lineno}: entry outside global:/local: section: {s}")

    if not saw_local_wildcard:
        errors.append("cmake/zahlen_engine.map: missing `local: *;` catch-all to hide non-API symbols")

    for canary in FORBIDDEN_CANARY_SYMBOLS:
        for rule in rules:
            if rule.regex.match(canary):
                errors.append(
                    f"cmake/zahlen_engine.map:{rule.line}: pattern '{rule.pattern}' over-matches forbidden symbol '{canary}'"
                )
    return rules


def itanium_encode_scope(parts: tuple[str, ...]) -> str:
    return "".join(f"{len(p)}{p}" for p in parts)


def expected_symbol_key(
    scope: tuple[str, ...],
    name: str,
    *,
    is_const: bool,
    is_rvalue: bool,
    kind: str,
) -> tuple[str, str]:
    """Return (canonical_api_label, probe_mangled_symbol) for a ZHLN_API callable."""
    qual = "::".join((*scope, name))
    suffix = " const" if is_const else (" &&" if is_rvalue else "")
    label = f"{qual}{suffix}"
    enc_scope = itanium_encode_scope(scope)
    if kind == "ctor":
        return label, f"_ZN{enc_scope}C1"
    if kind == "dtor":
        return label, f"_ZN{enc_scope}D1Ev"
    if kind == "assign":
        return label, f"_ZN{enc_scope}aSEOS1_"
    prefix = "_ZNK" if is_const else ("_ZNO" if is_rvalue else "_ZN")
    return label, f"{prefix}{enc_scope}{len(name)}{name}Ev"


@dataclass(frozen=True)
class HeaderApiDecl:
    file: str
    line: int
    label: str
    probe: str
    kind: str


def _split_top_level_statements(body: str) -> list[tuple[int, str, bool]]:
    """Yield (line_offset_0based, header_text, has_brace_body) for statements in a class/namespace block."""
    items: list[tuple[int, str, bool]] = []
    paren = 0
    angle = 0
    start = 0
    line = 0
    stmt_line = 0
    saw_token = False
    i = 0
    n = len(body)
    while i < n:
        ch = body[i]
        if ch == "\n":
            line += 1
            i += 1
            continue
        if not saw_token and not ch.isspace():
            if ch == "#":
                nl = body.find("\n", i)
                if nl == -1:
                    break
                line += 1
                i = nl + 1
                start = i
                continue
            saw_token = True
            stmt_line = line
        if ch == "(":
            paren += 1
        elif ch == ")":
            paren = max(0, paren - 1)
        elif ch == ";" and paren == 0:
            items.append((stmt_line, body[start:i].strip(), False))
            start = i + 1
            saw_token = False
        elif ch == "{" and paren == 0:
            # Find matching '}'
            depth = 1
            header = body[start:i].strip()
            j = i + 1
            while j < n and depth > 0:
                if body[j] == "\n":
                    line += 1
                elif body[j] == "{":
                    depth += 1
                elif body[j] == "}":
                    depth -= 1
                j += 1
            items.append((stmt_line, header, True))
            # Skip optional trailing ';' after class/struct/enum body
            while j < n and body[j] in " \t":
                j += 1
            if j < n and body[j] == ";":
                j += 1
            start = j
            saw_token = False
            i = j
            continue
        i += 1
    _ = angle
    return items


def _find_matching_brace(text: str, open_idx: int) -> int:
    depth = 1
    i = open_idx + 1
    n = len(text)
    while i < n and depth > 0:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    return i - 1


def _parse_member_decl(
    stmt: str,
    scope: tuple[str, ...],
    class_name: str | None,
    rel: str,
    lineno: int,
    errors: list[str],
) -> HeaderApiDecl | None:
    s = " ".join(stmt.split())
    if not s or s.startswith(("using ", "typedef ", "friend ", "static_assert", "enum ", "concept ")):
        return None
    # Strip C++ attributes like [[nodiscard]], [[noreturn]], [[gnu::cold, gnu::noinline]].
    s = re.sub(r"\[\[[^\]]*\]\]\s*", "", s)
    s = re.sub(r"\bZHLN_API\b\s*", "", s).strip()
    if re.search(r"=\s*(?:delete|default)\s*$", s):
        return None
    if re.match(r"^(?:template\s*<|inline\b|constexpr\b|consteval\b)", s):
        return None
    if "(" not in s:
        return None
    if re.search(r"\bvirtual\b", s):
        errors.append(f"{rel}:{lineno}: virtual method in ZHLN_API surface requires explicit vtable audit: {s}")
        return None

    # Locate parameter list `(...)` belonging to the declarator (the first top-level `(`).
    p_open = s.find("(")
    depth = 1
    p_close = p_open + 1
    while p_close < len(s) and depth > 0:
        if s[p_close] == "(":
            depth += 1
        elif s[p_close] == ")":
            depth -= 1
        p_close += 1
    p_close -= 1
    head = s[:p_open].strip()
    tail = s[p_close + 1 :].strip()
    # Strip trailing return type `-> ...` before checking cv/ref qualifiers.
    arrow = tail.find("->")
    quals = tail[:arrow].strip() if arrow != -1 else tail
    is_const = bool(re.search(r"\bconst\b", quals))
    is_rvalue = "&&" in quals

    if class_name is not None:
        if head == f"~{class_name}":
            label, probe = expected_symbol_key(scope, f"~{class_name}", is_const=False, is_rvalue=False, kind="dtor")
            return HeaderApiDecl(rel, lineno, label, probe, "dtor")
        if re.fullmatch(rf"(?:explicit\s+)?{re.escape(class_name)}", head):
            label, probe = expected_symbol_key(scope, class_name, is_const=False, is_rvalue=False, kind="ctor")
            return HeaderApiDecl(rel, lineno, label, probe, "ctor")
        if re.search(r"\boperator\s*=$", head):
            label, probe = expected_symbol_key(scope, "operator=", is_const=False, is_rvalue=False, kind="assign")
            return HeaderApiDecl(rel, lineno, label, probe, "assign")

    m = re.search(r"([A-Za-z_][A-Za-z0-9_]*)$", head)
    if not m:
        return None
    fn_name = m.group(1)
    label, probe = expected_symbol_key(scope, fn_name, is_const=is_const, is_rvalue=is_rvalue, kind="fn")
    return HeaderApiDecl(rel, lineno, label, probe, "fn")


def _scan_block(
    text: str,
    base_line: int,
    scope: tuple[str, ...],
    rel: str,
    decls: list[HeaderApiDecl],
    private_helpers: list[HeaderApiDecl],
    errors: list[str],
    *,
    in_api_class: str | None = None,
    in_plain_class: str | None = None,
) -> None:
    access = "private" if (in_api_class and in_api_class.startswith("class:")) else "public"
    cls_name = in_api_class.split(":", 1)[1] if in_api_class else in_plain_class

    # Collect text of inline bodies inside a ZHLN_API class so we detect private
    # out-of-line helpers called by public inline templates (e.g. TextInputImpl).
    inline_body_calls: set[str] = set()
    if in_api_class:
        p = 0
        while p < len(text):
            b = text.find("{", p)
            if b == -1:
                break
            e = _find_matching_brace(text, b)
            body_slice = text[b + 1 : e]
            for call_m in re.finditer(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", body_slice):
                inline_body_calls.add(call_m.group(1))
            p = e + 1

    # Walk the block tracking access specifiers, nested namespaces/types, and declarations.
    i = 0
    n = len(text)
    line = base_line
    paren = 0
    start = 0
    stmt_line = base_line
    saw = False

    while i < n:
        ch = text[i]
        if ch == "\n":
            line += 1
            i += 1
            continue
        if not saw and not ch.isspace():
            if ch == "#":
                nl = text.find("\n", i)
                if nl == -1:
                    break
                line += 1
                i = nl + 1
                start = i
                continue
            saw = True
            stmt_line = line

        # Handle `public:` / `protected:` / `private:` labels inside class definitions.
        if (in_api_class or in_plain_class) and paren == 0 and ch == ":":
            prev_ch = text[i - 1] if i > 0 else ""
            next_ch = text[i + 1] if i + 1 < n else ""
            if prev_ch != ":" and next_ch != ":":
                label_tok = text[start:i].strip()
                if label_tok in ("public", "protected", "private"):
                    access = label_tok
                    start = i + 1
                    saw = False
                    i += 1
                    continue

        if ch == "(":
            paren += 1
        elif ch == ")":
            paren = max(0, paren - 1)
        elif ch == ";" and paren == 0:
            stmt = text[start:i].strip()
            has_api = bool(re.search(r"\bZHLN_API\b", stmt))
            if in_api_class or has_api:
                decl = _parse_member_decl(stmt, scope, cls_name, rel, stmt_line, errors)
                if decl is not None:
                    if has_api or access in ("public", "protected"):
                        decls.append(decl)
                    else:
                        short_name = decl.label.split("::")[-1].split()[0]
                        if decl.kind == "fn" and short_name in inline_body_calls:
                            decls.append(decl)
                        else:
                            private_helpers.append(decl)
            start = i + 1
            saw = False
        elif ch == "{" and paren == 0:
            header = " ".join(text[start:i].split())
            close_idx = _find_matching_brace(text, i)
            inner = text[i + 1 : close_idx]
            inner_start_line = text.count("\n", 0, i + 1) + base_line
            line = text.count("\n", 0, close_idx + 1) + base_line

            ns_m = re.match(r"^(?:inline\s+)?namespace\s+([A-Za-z_][A-Za-z0-9_:]*)$", header)
            cls_m = re.search(
                r"\b(class|struct)\s+(?:(?:\[\[[^\]]*\]\]|alignas\s*\([^)]*\))\s*)*(ZHLN_API\s+)?([A-Za-z_][A-Za-z0-9_]*)\b(?:\s*final)?(?:\s*:[^{]*)?$",
                header,
            )
            if ns_m and not in_api_class and not in_plain_class:
                ns_parts = tuple(p for p in ns_m.group(1).split("::") if p)
                _scan_block(inner, inner_start_line, (*scope, *ns_parts), rel, decls, private_helpers, errors)
            elif cls_m and not header.startswith("enum ") and "template" not in header:
                kw, api_tok, cname = cls_m.group(1), cls_m.group(2), cls_m.group(3)
                if api_tok:
                    _scan_block(
                        inner, inner_start_line, (*scope, cname), rel, decls, private_helpers, errors,
                        in_api_class=f"{kw}:{cname}",
                    )
                elif "ZHLN_API" in inner:
                    _scan_block(
                        inner, inner_start_line, (*scope, cname), rel, decls, private_helpers, errors,
                        in_plain_class=cname,
                    )

            j = close_idx + 1
            while j < n and text[j] in " \t":
                j += 1
            if j < n and text[j] == ";":
                j += 1
            start = j
            saw = False
            i = j
            continue
        i += 1


def _iter_source_files(*rel_dirs: str) -> list[Path]:
    git_bin = shutil.which("git")
    if git_bin and (ROOT / ".git").exists():
        proc = subprocess.run(
            [git_bin, "ls-files", "-z", "--", *rel_dirs],
            cwd=ROOT,
            capture_output=True,
        )
        if proc.returncode == 0:
            paths: list[Path] = []
            for raw in proc.stdout.split(b"\0"):
                if not raw:
                    continue
                candidate = ROOT / raw.decode("utf-8", errors="surrogateescape")
                if candidate.suffix in SOURCE_SUFFIXES and candidate.is_file():
                    paths.append(candidate)
            paths.sort()
            return paths
    fallback: list[Path] = []
    for rel_dir in rel_dirs:
        base = ROOT / rel_dir
        if not base.is_dir():
            continue
        for candidate in base.rglob("*"):
            if candidate.is_file() and candidate.suffix in SOURCE_SUFFIXES:
                fallback.append(candidate)
    fallback.sort()
    return fallback


def collect_public_api_decls(errors: list[str]) -> tuple[list[HeaderApiDecl], list[HeaderApiDecl]]:
    decls: list[HeaderApiDecl] = []
    private_helpers: list[HeaderApiDecl] = []
    for path in _iter_source_files("include/Zahlen"):
        rel = path.relative_to(ROOT).as_posix()
        if rel == "include/Zahlen/Common.h":
            continue
        raw = path.read_text(encoding="utf-8")
        if "ZHLN_API" not in raw:
            continue
        cleaned = strip_comments_and_strings(raw)
        if not re.search(r"\bZHLN_API\b", cleaned):
            continue
        _scan_block(cleaned, 1, (), rel, decls, private_helpers, errors)

    for decl in decls:
        parts = decl.label.split()[0].split("::")
        if not parts or parts[0] != "ZHLN":
            errors.append(f"{decl.file}:{decl.line}: ZHLN_API symbol outside namespace ZHLN: {decl.label}")
            continue
        for part in parts[1:]:
            if part in FORBIDDEN_INTERNAL_SCOPES:
                errors.append(
                    f"{decl.file}:{decl.line}: forbidden internal scope '{part}' marked ZHLN_API: {decl.label}"
                )
    return decls, private_helpers


def check_non_public_zhln_api(errors: list[str]) -> None:
    for path in _iter_source_files(*NON_PUBLIC_DIRS):
        raw = path.read_text(encoding="utf-8", errors="ignore")
        if "ZHLN_API" not in raw:
            continue
        cleaned = strip_comments_and_strings(raw)
        for lineno, line in enumerate(cleaned.splitlines(), 1):
            if re.search(r"\bZHLN_API\b", line):
                rel = path.relative_to(ROOT).as_posix()
                errors.append(
                    f"{rel}:{lineno}: ZHLN_API is reserved for libzahlen_engine public headers in "
                    "include/Zahlen/ (use ZHLN_STATIC_API for static/source libraries, or omit for internals)"
                )


def check_parity(
    decls: list[HeaderApiDecl],
    private_helpers: list[HeaderApiDecl],
    rules: list[VersionRule],
    errors: list[str],
) -> None:
    if not rules:
        return
    matched_rules: set[int] = set()
    api_tags = {r.api_tag for r in rules}
    decl_labels = {d.label for d in decls}

    for decl in decls:
        if decl.kind == "ctor":
            hits = [idx for idx, r in enumerate(rules) if r.api_tag == decl.label and "C[12]" in r.pattern]
        else:
            hits = [idx for idx, r in enumerate(rules) if r.regex.match(decl.probe)]
        if not hits:
            errors.append(
                f"{decl.file}:{decl.line}: ZHLN_API declaration '{decl.label}' is missing from "
                "cmake/zahlen_engine.map (would break ELF consumers while exporting on Windows/macOS)"
            )
        else:
            matched_rules.update(hits)

    for idx, rule in enumerate(rules):
        if idx not in matched_rules and rule.api_tag not in decl_labels:
            errors.append(
                f"cmake/zahlen_engine.map:{rule.line}: stale export rule '{rule.pattern}' "
                f"(@api {rule.api_tag}) has no matching ZHLN_API declaration in include/Zahlen/"
            )

    for decl in decls:
        if decl.label not in api_tags:
            errors.append(
                f"{decl.file}:{decl.line}: ZHLN_API declaration '{decl.label}' lacks an exact "
                "/* @api ... */ entry in cmake/zahlen_engine.map"
            )

    for helper in private_helpers:
        if helper.kind == "ctor":
            continue
        for rule in rules:
            if rule.regex.match(helper.probe):
                errors.append(
                    f"cmake/zahlen_engine.map:{rule.line}: rule '{rule.pattern}' leaks private helper "
                    f"'{helper.label}' from {helper.file}:{helper.line}"
                )


def _strip_cmake_comments(text: str) -> str:
    return re.sub(r"#[^\n]*", "", text)


def _check_target_hidden_props(
    rel: str,
    text: str,
    target: str,
    required_props: tuple[str, ...],
    errors: list[str],
) -> None:
    blocks = re.findall(
        rf"set_target_properties\s*\(\s*{re.escape(target)}\s+PROPERTIES\b(.*?)\)",
        text,
        flags=re.S,
    )
    merged = " ".join(blocks)
    for prop in required_props:
        if not re.search(prop, merged):
            errors.append(f"{rel}: target '{target}' is missing required property matching `{prop}`")


def check_cmake_invariants(errors: list[str]) -> None:
    root_cm = _strip_cmake_comments((ROOT / "CMakeLists.txt").read_text(encoding="utf-8"))
    all_vis = (
        r"\bC_VISIBILITY_PRESET\s+hidden\b",
        r"\bCXX_VISIBILITY_PRESET\s+hidden\b",
        r"\bOBJC_VISIBILITY_PRESET\s+hidden\b",
        r"\bOBJCXX_VISIBILITY_PRESET\s+hidden\b",
        r"\bVISIBILITY_INLINES_HIDDEN\s+ON\b",
    )
    _check_target_hidden_props("CMakeLists.txt", root_cm, "zahlen_engine", all_vis, errors)
    _check_target_hidden_props(
        "CMakeLists.txt",
        root_cm,
        "${ZHLN_ENGINE_LIB_TARGET}",
        (r"\bPOSITION_INDEPENDENT_CODE\s+ON\b", *all_vis),
        errors,
    )

    if len(re.findall(r"LINKER:--exclude-libs,ALL", root_cm)) < 2:
        errors.append("CMakeLists.txt: zahlen_engine must enforce LINKER:--exclude-libs,ALL on both ELF and MinGW")
    if "LINKER:--version-script=${ZHLN_ENGINE_VERSION_SCRIPT}" not in root_cm:
        errors.append("CMakeLists.txt: zahlen_engine must enforce LINKER:--version-script=${ZHLN_ENGINE_VERSION_SCRIPT}")
    if "LINK_DEPENDS" not in root_cm or "ZHLN_ENGINE_VERSION_SCRIPT" not in root_cm:
        errors.append("CMakeLists.txt: zahlen_engine must register cmake/zahlen_engine.map in LINK_DEPENDS")
    if "check_engine_exports.py" not in root_cm or "--dso" not in root_cm:
        errors.append("CMakeLists.txt: zahlen_engine must run configure/check_engine_exports.py --dso at POST_BUILD")
    if re.search(r"\bmimalloc-static\b", root_cm):
        errors.append("CMakeLists.txt: zahlen_engine must not link mimalloc-static into libzahlen_engine")

    # Third-party and platform archives folded into zahlen_engine.
    tp_cm = _strip_cmake_comments((ROOT / "cmake/ThirdParty.cmake").read_text(encoding="utf-8"))
    _check_target_hidden_props(
        "cmake/ThirdParty.cmake", tp_cm, "Jolt", (r"\bPOSITION_INDEPENDENT_CODE\s+ON\b", *all_vis), errors
    )
    _check_target_hidden_props(
        "cmake/ThirdParty.cmake",
        tp_cm,
        "meshoptimizer",
        (
            r"\bPOSITION_INDEPENDENT_CODE\s+ON\b",
            r"\bC_VISIBILITY_PRESET\s+hidden\b",
            r"\bCXX_VISIBILITY_PRESET\s+hidden\b",
            r"\bVISIBILITY_INLINES_HIDDEN\s+ON\b",
        ),
        errors,
    )
    _check_target_hidden_props(
        "cmake/ThirdParty.cmake",
        tp_cm,
        "spirv-reflect-static",
        (
            r"\bPOSITION_INDEPENDENT_CODE\s+ON\b",
            r"\bC_VISIBILITY_PRESET\s+hidden\b",
            r"\bCXX_VISIBILITY_PRESET\s+hidden\b",
            r"\bVISIBILITY_INLINES_HIDDEN\s+ON\b",
        ),
        errors,
    )
    _check_target_hidden_props(
        "cmake/ThirdParty.cmake",
        tp_cm,
        "glfw",
        (
            r"\bPOSITION_INDEPENDENT_CODE\s+ON\b",
            r"\bC_VISIBILITY_PRESET\s+hidden\b",
            r"\bOBJC_VISIBILITY_PRESET\s+hidden\b",
        ),
        errors,
    )
    if "if(APPLE OR USE_SANITIZERS)" not in tp_cm or "-fsanitize=" not in tp_cm:
        errors.append("cmake/ThirdParty.cmake: mimalloc must be disabled on APPLE, USE_SANITIZERS, and -fsanitize= builds")

    audio_cm = _strip_cmake_comments((ROOT / "src/audio/CMakeLists.txt").read_text(encoding="utf-8"))
    _check_target_hidden_props(
        "src/audio/CMakeLists.txt",
        audio_cm,
        "zahlen_miniaudio",
        (
            r"\bPOSITION_INDEPENDENT_CODE\s+ON\b",
            r"\bC_VISIBILITY_PRESET\s+hidden\b",
            r"\bOBJC_VISIBILITY_PRESET\s+hidden\b",
        ),
        errors,
    )

    blit_cm = _strip_cmake_comments((ROOT / "src/render/OpenGLHacks/CMakeLists.txt").read_text(encoding="utf-8"))
    _check_target_hidden_props(
        "src/render/OpenGLHacks/CMakeLists.txt",
        blit_cm,
        "HostBlitSwapchain",
        (r"\bPOSITION_INDEPENDENT_CODE\s+ON\b", *all_vis),
        errors,
    )

    # Sub-libraries owning ZHLN_API symbols must be OBJECT libraries (never STATIC
    # archives hidden by -Wl,--exclude-libs,ALL) and must never be linked into
    # sibling libraries in src/ (which would embed duplicate .o files).
    for obj_target, rel in (
        ("zahlen_threading", "src/threading/CMakeLists.txt"),
        ("zahlen_filesystem", "src/filesystem/CMakeLists.txt"),
    ):
        cm = _strip_cmake_comments((ROOT / rel).read_text(encoding="utf-8"))
        if not re.search(rf"\badd_library\s*\(\s*{obj_target}\s+OBJECT\b", cm):
            errors.append(
                f"{rel}: {obj_target} defines ZHLN_API symbols and must be an OBJECT library "
                "(STATIC archives are hidden by -Wl,--exclude-libs,ALL)"
            )

    for cm_path in sorted((ROOT / "src").rglob("CMakeLists.txt")):
        rel = cm_path.relative_to(ROOT).as_posix()
        cm = _strip_cmake_comments(cm_path.read_text(encoding="utf-8"))
        for block in re.findall(r"target_link_libraries\s*\((.*?)\)", cm, flags=re.S):
            tokens = block.split()
            if not tokens:
                continue
            owner = tokens[0]
            if owner == "zahlen_engine":
                continue
            for obj_target in (*API_OBJECT_TARGETS, "zahlen_ecs"):
                if obj_target in tokens[1:]:
                    errors.append(
                        f"{rel}: target '{owner}' links OBJECT library '{obj_target}' "
                        "(would duplicate object files; only zahlen_engine may link it)"
                    )

    # Verify host-only mimalloc attachment and executable sanitizer hook scoping.
    eng_cm = _strip_cmake_comments((ROOT / "src/engine/CMakeLists.txt").read_text(encoding="utf-8"))
    if not MIMALLOC_LIST.is_file():
        errors.append("Missing cmake/mimalloc_override.list for host executable mimalloc interposition")
    if (
        "$<BUILD_LOCAL_INTERFACE:$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:mimalloc-static>>"
        not in eng_cm
    ):
        errors.append(
            "src/engine/CMakeLists.txt: mimalloc-static must attach strictly via "
            "$<BUILD_LOCAL_INTERFACE:$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:mimalloc-static>>"
        )
    if "--export-dynamic-symbol=" not in eng_cm or "zahlen_sanitizer_options" not in eng_cm:
        errors.append("src/engine/CMakeLists.txt: zahlen_sanitizer_options executable export hooks must remain intact")

    # Consumer layers outside src/ must never link internal engine sub-libraries.
    private_pat = re.compile(r"\b(?:" + "|".join(re.escape(t) for t in PRIVATE_ENGINE_TARGETS) + r")\b")
    for folder in CONSUMER_DIRS:
        base = ROOT / folder
        if not base.is_dir():
            continue
        for cm_path in sorted(base.rglob("CMakeLists.txt")):
            rel = cm_path.relative_to(ROOT).as_posix()
            cm = _strip_cmake_comments(cm_path.read_text(encoding="utf-8"))
            for block in re.findall(r"target_link_libraries\s*\((.*?)\)", cm, flags=re.S):
                for hit in sorted(set(private_pat.findall(block))):
                    errors.append(f"{rel}: consumer target links private engine target '{hit}'")


def _demangle(sym: str) -> str:
    cppfilt = shutil.which("c++filt")
    if not cppfilt or not sym.startswith("_Z"):
        return sym
    try:
        out = subprocess.check_output([cppfilt, sym], text=True, stderr=subprocess.DEVNULL).strip()
        return f"{out} [{sym}]" if out and out != sym else sym
    except OSError:
        return sym


def audit_dso(dso_path: Path) -> int:
    errors: list[str] = []
    rules = parse_version_script(VERSION_SCRIPT, errors)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1

    if not dso_path.is_file():
        print(f"Engine DSO export audit failed: file not found: {dso_path}", file=sys.stderr)
        return 1

    nm_bin = shutil.which("nm") or shutil.which("llvm-nm")
    if not nm_bin:
        print("Engine DSO export audit failed: neither `nm` nor `llvm-nm` found in PATH", file=sys.stderr)
        return 1

    is_macho = dso_path.suffix == ".dylib" or sys.platform == "darwin"
    cmd = [nm_bin, "-gU", str(dso_path)] if is_macho else [nm_bin, "-D", "--defined-only", str(dso_path)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        print(f"Engine DSO export audit failed running {' '.join(cmd)}:\n{proc.stderr.strip()}", file=sys.stderr)
        return 1

    exported: list[tuple[str, str]] = []
    for raw in proc.stdout.splitlines():
        parts = raw.split()
        if len(parts) < 2:
            continue
        sym_type = parts[-2] if len(parts) >= 3 else parts[0]
        sym = parts[-1]
        # Strip ELF version suffix (`@@Base`) if a toolchain emits one.
        sym = sym.split("@", 1)[0]
        if is_macho and sym.startswith("_"):
            sym = sym[1:]
        if sym in LINKER_BOOKKEEPING_SYMBOLS:
            continue
        # Absolute version-node marker symbols (`A`).
        if sym_type == "A":
            continue
        exported.append((sym_type, sym))

    if not exported:
        print(f"Engine DSO export audit failed: {dso_path} exports zero dynamic symbols", file=sys.stderr)
        return 1

    leaks: list[str] = []
    for sym_type, sym in exported:
        if sym_type == "u":
            leaks.append(f"  STB_GNU_UNIQUE leak: {_demangle(sym)}")
            continue
        if sym in ALLOCATOR_OVERRIDE_SYMBOLS:
            leaks.append(f"  allocator interposition leak: {sym}")
            continue
        matched_forbidden = False
        for category, rx in FORBIDDEN_SYMBOL_REGEXES:
            if rx.search(sym):
                leaks.append(f"  forbidden {category} export: {_demangle(sym)}")
                matched_forbidden = True
                break
        if matched_forbidden:
            continue
        if not any(rule.regex.match(sym) for rule in rules):
            leaks.append(f"  unallowlisted export: {_demangle(sym)}")

    sym_names = [sym for _, sym in exported]
    for label, prefix in REQUIRED_DSO_PREFIXES:
        if not any(s.startswith(prefix) for s in sym_names):
            leaks.append(f"  missing required public symbol: {label} ({prefix}*)")

    if leaks:
        header = (
            f"Engine DSO export audit FAILED for {dso_path.name} "
            f"({len(leaks)} violation(s) across {len(exported)} dynamic symbols):"
        )
        preview = "\n".join(leaks[:40])
        more = f"\n  ... and {len(leaks) - 40} more" if len(leaks) > 40 else ""
        print(f"{header}\n{preview}{more}", file=sys.stderr)
        return 1

    print(
        f"Engine DSO export audit OK ({dso_path.name}: {len(exported)} exported API symbols; "
        "0 internal, third-party, stdlib, or allocator leaks)."
    )
    return 0


def run_source_audit() -> int:
    errors: list[str] = []
    check_non_public_zhln_api(errors)
    rules = parse_version_script(VERSION_SCRIPT, errors)
    decls, private_helpers = collect_public_api_decls(errors)
    check_parity(decls, private_helpers, rules, errors)
    check_cmake_invariants(errors)

    if errors:
        print("Engine symbol-export governance failed:\n  " + "\n  ".join(errors), file=sys.stderr)
        return 1

    print(
        f"Engine export boundary OK ({len(rules)} version-script rules in sync with "
        f"{len(decls)} ZHLN_API declarations; {len(private_helpers)} private helpers sealed)."
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--dso",
        type=Path,
        default=None,
        help="Audit the dynamic symbol table of a linked libzahlen_engine.{so,dylib} via nm",
    )
    args = parser.parse_args()
    if args.dso is not None:
        return audit_dso(args.dso)
    return run_source_audit()


if __name__ == "__main__":
    raise SystemExit(main())
