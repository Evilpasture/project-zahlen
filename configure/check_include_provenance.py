#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reject symbols used without the header that declares them.

A translation unit may only name a type it can reach through its own includes.
That sounds like what a compiler checks, and it is -- but only for the symbols
it happens to instantiate, and only once a build reaches the file. The failure
this catches is the one that gets away: a file that compiles *today* because
some header it includes happens to include the declaration it wants, and stops
compiling the day that chain is cut. That is exactly how <Zahlen/Types.hpp>
became the engine's junk drawer -- light types reached Components.hpp, graphics
settings reached the renderer, Jolt reached the audio mixer, and nothing said so,
because no one of those files named the header it was really using.

So the rule is stated from the consumer's side: for every symbol in the table
below, the file that names it must reach a provider through its own include
closure -- directly, or through a header it includes, transitively. Reachability
is computed here, not compiled, which is why this runs at CMake configure time
and covers the whole tree in one pass.

Three kinds of provider, resolved three ways:

  * first-party headers -- a path in the repository, reached through the
    include closure;
  * third-party headers -- matched by spelling (``Jolt/``), since the vendored
    trees are submodules a checkout may not have;
  * standard headers -- matched by spelling (``span``, ``string``).

Two more rules keep the graph honest. An include that names a first-party header
and does not resolve is an error, not a third-party include: that is how a stale
`#include "Types.hpp"` is caught after the header it named is gone -- and for a
name whose header is no longer in the tree to be found, RETIRED_HEADERS below is
what remembers it. And a tree
that is umbrella-dependent on purpose (src/vulkan) is skipped, because its leaf
headers are not meant to stand alone.

The table is deliberately a list of *pairs*, not "everything in every header":
it names the symbols whose provenance this tree has already lost once. Add to it
when a symbol starts being reached by accident -- that is the moment the rule
needs to exist, not after the next sweep.

This runs before every configure, so it is written to stay under a second: one
walk of the tree, one read and one scan per file, one lookup per include. The
tree's own file list is built once and resolution is a set membership test
against it, never a realpath per candidate root; the symbol table is one
alternation rather than a search per symbol; the self-definition test is one
scan per file rather than one per name. The first version of this check did all
three the obvious way and took twenty seconds, which is the kind of configure
step people learn to skip -- if you add a rule, keep this shape, and measure it
before you keep a slower one.
"""

from __future__ import annotations

import re
import sys
from functools import cache, lru_cache
from os.path import isfile, join, normpath
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SOURCE_ROOTS = (
    "include",
    "src",
    "extras",
    "modules",
    "tools",
    "tests",
    "samples",
    "app",
)
SOURCE_SUFFIXES = {
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".inl",
    ".ipp",
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".ixx",
    ".cppm",
}
HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp"}
SKIP_DIR_NAMES = {".git", "build", "__pycache__", "node_modules", ".cache"}

# --- Provider table ------------------------------------------------------------------
#
# symbol -> provider. A first-party provider is a repository path; a THIRD_PARTY
# entry is a spelling pattern; a STANDARD entry is a header name without its
# angle brackets, because that is what the include regex captures.

FIRST_PARTY: dict[str, tuple[str, ...]] = {
    # Core
    "AssetID": ("include/Zahlen/Core/AssetID.hpp",),
    "MaterialID": ("include/Zahlen/Core/AssetID.hpp",),
    "InvalidAssetID": ("include/Zahlen/Core/AssetID.hpp",),
    "InvalidMaterialID": ("include/Zahlen/Core/AssetID.hpp",),
    "HashAssetID": ("include/Zahlen/Core/AssetID.hpp",),
    "EnableEnumFlags": ("include/Zahlen/Core/EnumFlags.hpp",),
    "EnumFlag": ("include/Zahlen/Core/EnumFlags.hpp",),
    # Geometry
    "Extent2D": ("include/Zahlen/Geometry2D.hpp",),
    "Offset2D": ("include/Zahlen/Geometry2D.hpp",),
    "ScissorRect": ("include/Zahlen/Geometry2D.hpp",),
    "ViewportRect": ("include/Zahlen/Geometry2D.hpp",),
    # Vertex stream
    "Packed1010102": ("include/Zahlen/Vertex.hpp",),
    "PackedHalf2": ("include/Zahlen/Vertex.hpp",),
    "PackedRGBA8": ("include/Zahlen/Vertex.hpp",),
    "VertexPosition": ("include/Zahlen/Vertex.hpp",),
    "VertexAttributes": ("include/Zahlen/Vertex.hpp",),
    "VertexSkin": ("include/Zahlen/Vertex.hpp",),
    # Audio
    "AudioHandle": ("include/Zahlen/Audio/AudioTypes.hpp",),
    "SynthHandle": ("include/Zahlen/Audio/AudioTypes.hpp",),
    "AudioWaveformType": ("include/Zahlen/Audio/AudioTypes.hpp",),
    "AudioFilterType": ("include/Zahlen/Audio/AudioTypes.hpp",),
    "AudioNoiseType": ("include/Zahlen/Audio/AudioTypes.hpp",),
    # GUI
    "UIBatch": ("include/Zahlen/gui/UIData.hpp",),
    "UIDrawData": ("include/Zahlen/gui/UIData.hpp",),
    "GlyphMetric": ("include/Zahlen/gui/Font.hpp",),
    "FontAtlas": ("include/Zahlen/gui/Font.hpp",),
    # Renderer vocabulary
    "TextureHandle": ("include/Zahlen/Render/Handles.hpp",),
    "BufferHandle": ("include/Zahlen/Render/Handles.hpp",),
    "PipelineHandle": ("include/Zahlen/Render/Handles.hpp",),
    "ResourceGroupHandle": ("include/Zahlen/Render/Handles.hpp",),
    "SystemTextures": ("include/Zahlen/Render/Handles.hpp",),
    "RenderAttachment": ("include/Zahlen/Render/Handles.hpp",),
    "Mesh": ("include/Zahlen/Render/Types.hpp",),
    "Material": ("include/Zahlen/Render/Types.hpp",),
    "DrawFlags": ("include/Zahlen/Render/Types.hpp",),
    "GPUVolumetricVolume": ("include/Zahlen/Render/Types.hpp",),
    "CSGOperation": ("include/Zahlen/Render/Types.hpp",),
    "CSGModifier": ("include/Zahlen/Render/Types.hpp",),
    # Meshlet contract
    "GPUMeshlet": ("include/Zahlen/Meshlet.hpp",),
    "MeshletBuildResult": ("include/Zahlen/Meshlet.hpp",),
    "kMeshletMaxVertices": ("include/Zahlen/Meshlet.hpp",),
    "kMeshletMaxTriangles": ("include/Zahlen/Meshlet.hpp",),
    "kMeshletConeWeight": ("include/Zahlen/Meshlet.hpp",),
    "kMeshletsPerTaskGroup": ("include/Zahlen/Meshlet.hpp",),
    "kMeshShaderGroupSize": ("include/Zahlen/Meshlet.hpp",),
    # Reached by accident before the sweep: the ECS spells LightType, the renderer
    # spells GraphicsSettings, and neither included its own header.
    "LightType": ("include/Zahlen/Render/GpuEnums.hpp",),
    "ParticleAlignment": ("include/Zahlen/Render/GpuEnums.hpp",),
    "QualityLevel": ("include/Zahlen/GraphicsSettings.hpp",),
    "AAMode": ("include/Zahlen/GraphicsSettings.hpp",),
    "AAState": ("include/Zahlen/GraphicsSettings.hpp",),
    "GISettings": ("include/Zahlen/GraphicsSettings.hpp",),
    "ShadowSettings": ("include/Zahlen/GraphicsSettings.hpp",),
    "RayTracingConfig": ("include/Zahlen/GraphicsSettings.hpp",),
    "EnvironmentSettings": ("include/Zahlen/GraphicsSettings.hpp",),
    "GraphicsSettings": ("include/Zahlen/GraphicsSettings.hpp",),
    "Hash64": ("include/Zahlen/Core/Hash.hpp",),
    "Hash32": ("include/Zahlen/Core/Hash.hpp",),
    "HashCombine": ("include/Zahlen/Core/Hash.hpp",),
}

THIRD_PARTY: dict[str, str] = {
    "JPH::": r"(^|/)Jolt/",
}

STANDARD: dict[str, str] = {
    "std::span": "span",
    "std::string_view": "string_view",
    "std::array": "array",
    "std::optional": "optional",
    "std::unique_ptr": "memory",
    "std::make_unique": "memory",
    "std::vector": "vector",
    "std::string": "string",
}

# A file that re-exports instead of using: it is not compiled on its own and its
# own include list is not the thing under test.
SKIP_PATHS = {"modules/zahlen.cppm"}

# Trees whose headers are umbrella-dependent on purpose. src/vulkan's leaf
# headers carry
#
#     #ifndef ZHLN_RENDERING_HPP_INCLUDED
#     #error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#     #endif
#
# and include nothing themselves: the RHI's public surface is the umbrella, and a
# leaf is only ever compiled underneath it. Holding those files to "reach what
# you name" would demand includes their design forbids, so the rule starts above
# the umbrella. Their vocabulary is local to that layer anyway -- Vk's
# TextureHandle is a heap handle and Vk's Extent2D() is a method, neither of them
# the public engine types this table is about.
SKIP_PREFIXES = ("src/vulkan/",)

# A file that defines a name itself is not reaching for someone else's: `using
# TextureHandle = HeapHandle<...>` and `auto Extent2D() const` are locals, and
# matching them would be a false positive of exactly the kind that makes a guard
# untrustworthy. Collected in one scan per file rather than one scan per symbol,
# because the definition is what lets a name be used in the same file at all.
SELF_DEFINITION_RE = re.compile(
    r"\b(?:using|typedef)\s+([A-Za-z_]\w*)"
    r"|\b(?:struct|class|union|enum\s+class)\s+([A-Za-z_]\w*)"
    r"|\b(?:constexpr\s+)?auto\s+([A-Za-z_]\w*)\s*\("
)


def defined_symbols(body: str) -> frozenset[str]:
    """Every name ``body`` declares for itself, at any of the shapes above."""
    return frozenset(
        name
        for match in SELF_DEFINITION_RE.finditer(body)
        for name in match.groups()
        if name
    )


INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.MULTILINE)

# One ordered scan over everything that is not code. The order *is* the point:
# at any position the first alternative that matches wins, so a `//` inside a
# string literal belongs to that literal and a `"` inside a comment belongs to
# the comment. Stripping comments first and literals second -- the obvious way
# round -- gets this wrong: it truncates `out.Line("a // b")` at the `//` and
# leaves an unterminated literal behind, which is exactly how a generated-code
# string in tools/zshader became a phantom std::span dependency.
SCAN_RE = re.compile(
    r'(?P<include>^\s*#\s*include\s*[<"][^>"]+[>"])'
    r'|(?P<raw>R"(?P<delim>[^()\s]{0,16})\((?P<body>.*?)\)(?P=delim)")'
    r"|(?P<block>/\*.*?\*/)"
    r"|(?P<line>//[^\n]*)"
    r'|(?P<str>"(?:\\.|[^"\\\n])*")'
    r"|(?P<chr>'(?:\\.|[^'\\\n])*')",
    re.DOTALL | re.MULTILINE,
)


def strip_noncode(text: str, keep_includes: bool) -> str:
    """Blank what cannot hold a symbol, keeping the line structure intact.

    ``keep_includes`` keeps `#include` directives verbatim, which is what the
    include graph needs: the spelling of a quoted include is an ordinary string
    literal to every other pattern here, and blanking it would erase the very
    dependency this file is about.

    Raw strings go in both modes, because a header's *text* is not a header:
    tools/zshader/Emit.cpp carries the text of the files it generates inside
    R"ZHLN(...)" literals, and the `#include "Rendering.hpp"` in there is a line
    of generated output, not a dependency of Emit.cpp. Reading it as one invents
    an edge to src/vulkan and a spelling that resolves nowhere.
    """

    def replace(match: re.Match[str]) -> str:
        if keep_includes and match.lastgroup == "include":
            return match.group()
        return "\n" * match.group().count("\n")

    return SCAN_RE.sub(replace, text)


def strip_views(text: str) -> tuple[str, str]:
    """One scan, both views: the include graph's text and the symbol scan's.

    ``(with includes, inert)``. Same walk, same match set, same blanking rules
    as strip_noncode in either mode -- this is that function returning both of
    its answers, because a file needs both and scanning it twice was a third of
    this check's cost.

    The inert view is what keeps a *name* in output from counting as a use of
    it: tools/zshader/Emit.cpp writes `out.Line("extern const std::span<const
    uint8_t> {};", ...)`, naming std::span in text it emits and never compiles
    itself, and literal noise like that makes a guard nobody can satisfy.
    """
    keep: list[str] = []
    inert: list[str] = []
    position = 0
    for match in SCAN_RE.finditer(text):
        untouched = text[position : match.start()]
        keep.append(untouched)
        inert.append(untouched)
        position = match.end()
        blank = "\n" * match.group().count("\n")
        if match.lastgroup == "include":
            keep.append(match.group())
            inert.append(blank)
        else:
            keep.append(blank)
            inert.append(blank)
    tail = text[position:]
    keep.append(tail)
    inert.append(tail)
    return "".join(keep), "".join(inert)


# Headers this tree removed on purpose, by basename. A deleted header cannot be
# found by looking for the names that exist, so a leftover `#include "Types.hpp"`
# resolves nowhere and is indistinguishable from an external header by spelling
# alone. A name lands here when its header is deleted, and leaving it off would
# only mean the stale include compiles until the day someone deletes the
# namesake that was making the rule fire by accident.
RETIRED_HEADERS = frozenset({"Types.hpp"})


@lru_cache(maxsize=1)
def tree_files() -> dict[str, Path]:
    """Every file of this repository, keyed by its repo-relative path, walked once.

    Resolution used to ask the filesystem per candidate -- `(root / spelling)
    .resolve()` and then `.is_file()`, a realpath and a stat for every include
    times every root that include could be reached from. For this tree that is
    hundreds of thousands of syscalls for an answer that cannot change while the
    check runs, and it cost more than everything else here together. The walk
    happens once; every lookup after it is a dict hit.

    ``extern/`` and ``third_party/`` are deliberately not walked: they are
    submodules with tens of thousands of files, no rule here is about their
    contents, and a configure that spends a second enumerating a vendored tree
    to answer a question about *this* tree has the trade backwards. A spelling
    that lands there still resolves -- see Index.resolve, which falls back to
    the filesystem exactly once per distinct spelling.
    """
    files: dict[str, Path] = {}
    for name in SOURCE_ROOTS:
        base = ROOT / name
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if not path.is_file() or any(part in SKIP_DIR_NAMES for part in path.parts):
                continue
            files[path.relative_to(ROOT).as_posix()] = path
    return files


@lru_cache(maxsize=1)
def resolvable_paths() -> frozenset[str]:
    """The same walk, spelled absolutely using POSIX form."""
    return frozenset((ROOT / relative).as_posix() for relative in tree_files())


@lru_cache(maxsize=1)
def directory_entries() -> dict[str, frozenset[str]]:
    """For each directory in the walk, the names directly inside it.

    The cheap half of resolution: a root can only produce a spelling if it has
    that spelling's first component, and asking a set is one lookup instead of
    building and normalizing a candidate path for every root in the repository.
    """
    entries: dict[str, set[str]] = {}
    for relative, path in tree_files().items():
        parent = (
            join(str(ROOT), Path(relative).parent.as_posix())
            if "/" in relative
            else str(ROOT)
        )
        entries.setdefault(parent, set()).add(Path(relative).name)
    for root in (ROOT, *search_roots()):
        try:
            if root.is_dir():
                entries.setdefault(str(root), set()).update(
                    child.name for child in root.iterdir()
                )
        except OSError:
            pass
    return {directory: frozenset(names) for directory, names in entries.items()}


@lru_cache(maxsize=1)
def first_party_header_names() -> frozenset[str]:
    """Basenames of every header in include/ and src/.

    Used to tell a broken first-party include from a third-party one: a bare
    `#include "Types.hpp"` that resolves nowhere is pointing at a header of ours
    that no longer exists, while `#include "vk_mem_alloc.h"` is simply external.

    Cached, and it has to be: this is asked once per unresolved include line, and
    re-walking two trees to answer it is what made the first version of this
    check take twenty seconds.
    """
    return frozenset(
        Path(relative).name
        for relative in tree_files()
        if relative.startswith(("include/", "src/"))
        and Path(relative).suffix in HEADER_SUFFIXES
    )


def search_roots() -> list[Path]:
    """The include roots a compile of this tree would have, generously.

    Generous on purpose: a root that does not exist costs nothing, and a missing
    root would turn a real violation into a false "unresolved". Resolution only
    ever decides *reachability*, which is what is being asserted.
    """
    roots: list[Path] = [ROOT / "include", ROOT, ROOT / "src"]
    for name in (
        "src",
        "extras",
        "extern",
        "third_party",
        "tests",
        "modules",
        "tools",
        "samples",
        "app",
        "include",
    ):
        base = ROOT / name
        if not base.is_dir():
            continue
        roots.append(base)
        for child in sorted(p for p in base.iterdir() if p.is_dir()):
            roots.append(child)
            for grandchild in sorted(p for p in child.iterdir() if p.is_dir()):
                roots.append(grandchild)
    return roots


def source_files() -> list[Path]:
    files: list[Path] = []
    for name in SOURCE_ROOTS:
        base = ROOT / name
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            if any(part in SKIP_DIR_NAMES for part in path.parts):
                continue
            files.append(path)
    return sorted(set(files))


@cache
def quoted_roots(including: Path) -> tuple[Path, ...]:
    """Where a quoted include of ``including`` may legitimately look.

    A quoted include resolves beside the including file, then up through the
    directories that own it -- the subtree roots a target puts on its own
    include path -- and nowhere else. Searching every root in the repository
    would resolve `#include "Types.hpp"` against an unrelated subsystem's
    private header of the same name, which is how a broken include hides.

    The cross-subsystem seams are the documented exceptions, each one a
    PRIVATE include path in CMake: src/render reaches src/window and
    src/vulkan for the presentation seam and the RHI, src/window reaches
    src/engine for the TTY backend, and the engine keeps its systems in a
    second include directory of its own target.

    Cached per file: it is asked once per include in that file, and walking to
    the root and resolving symlinks every time is a syscall storm of its own.
    """
    roots: list[Path] = []
    current = including.parent
    while True:
        roots.append(current)
        if current == ROOT or current.parent == current:
            break
        current = current.parent
    # The public tree, and src/ itself: several targets (the composition
    # root, the tools, the extras that drive the engine's systems) put
    # `${PROJECT_SOURCE_DIR}/src` on their include path and spell a header
    # `"engine/Platform.hpp"` from there.
    roots.append(ROOT / "include")
    roots.append(ROOT / "src")
    try:
        relative = including.relative_to(ROOT).as_posix()
    except ValueError:
        relative = including.as_posix()
    for prefix, extra in (
        (
            "src/engine/",
            ROOT / "src/engine/system",
        ),  # one target, two PRIVATE include dirs
        ("src/engine/system/", ROOT / "src/engine"),
        ("src/render/", ROOT / "src/window"),
        ("src/render/", ROOT / "src/vulkan"),
        ("src/render/", ROOT / "src/render/init"),
        ("src/window/", ROOT / "src/engine"),
    ):
        if relative.startswith(prefix) and extra.is_dir():
            roots.append(extra)
    return tuple(roots)


class Index:
    """The include graph, with the include paths a target would really have."""

    def __init__(self) -> None:
        self.roots = search_roots()
        self.includes: dict[Path, list[tuple[str, str]]] = {}
        self.resolved: dict[Path, list[Path]] = {}
        self.texts: dict[Path, tuple[str, str]] = {}
        self._closure_cache: dict[Path, frozenset[Path]] = {}
        self._angle_cache: dict[str, Path | None] = {}
        self._resolvable = resolvable_paths()
        self._entries = directory_entries()

    def read(self, path: Path) -> None:
        if path in self.includes:
            return
        try:
            views = strip_views(path.read_text(encoding="utf-8", errors="ignore"))
        except OSError:
            self.includes[path] = []
            self.resolved[path] = []
            self.texts[path] = ("", "")
            return
        self.texts[path] = views
        found = INCLUDE_RE.findall(views[0])
        self.includes[path] = found
        self.resolved[path] = [
            r for r in (self.resolve(path, d, s) for d, s in found) if r is not None
        ]

    def resolve(self, including: Path, delimiter: str, spelling: str) -> Path | None:
        """The file a spelling names, without asking the filesystem.

        `root / spelling`, normalized, is looked up in the walk tree_files()
        already did. A spelling that can leave those roots -- absolute, or
        carrying a `..` -- keeps the syscall version, because normalizing it as
        a string would answer a different question than the compiler asks.

        An angle include is answered once and remembered: its roots are this
        repository's, the same list for every file, and a third-party spelling
        such as <Jolt/Jolt.h> is the expensive case -- every root tried and
        missed before the one that has it.
        """
        if delimiter == '"':
            return self._search(quoted_roots(including), spelling)
        if spelling in self._angle_cache:
            return self._angle_cache[spelling]
        found = self._search(self.roots, spelling)
        self._angle_cache[spelling] = found
        return found

    def _search(
        self, roots: tuple[Path, ...] | list[Path], spelling: str
    ) -> Path | None:
        if spelling.startswith("/") or ".." in spelling:
            return self._stat_search(roots, spelling)
        first = spelling.split("/", 1)[0]
        entries = self._entries
        for root in roots:
            if first not in entries.get(str(root), ()):
                continue
            candidate = Path(normpath(join(str(root), spelling))).as_posix()
            if candidate in self._resolvable:
                return Path(candidate)
        # Nothing in the tree this walk covers. That is either a vendored header
        # (extern/, third_party/ -- see tree_files) or a spelling that resolves
        # nowhere at all, and both are answered the way they always were, by
        # asking the filesystem. The angle cache is what keeps this from running
        # per include rather than per distinct spelling.
        return self._stat_search(roots, spelling)

    def _stat_search(
        self, roots: tuple[Path, ...] | list[Path], spelling: str
    ) -> Path | None:
        """The filesystem's answer, one stat per root that could possibly have it.

        Only roots that carry the spelling's first component are asked, and the
        candidate is spelled as `root / spelling` rather than resolved: the
        compiler looks for a file at that path, and a realpath per root per
        spelling is what made the first version of this check slow.
        """
        first = spelling.split("/", 1)[0]
        # A spelling that climbs out of its root (`"../RenderInternal.hpp"`) has
        # no first component to match against, so only the filesystem can answer
        # it. Everything else is filtered first: a root without that component
        # cannot produce the spelling whatever else is true.
        unfiltered = ".." in spelling or spelling.startswith("/")
        entries = self._entries
        for root in roots:
            if not unfiltered and first not in entries.get(str(root), ()):
                continue
            candidate = normpath(join(str(root), spelling))
            if isfile(candidate):
                return Path(candidate)
        return None

    # Iterative, not recursive: the first-party graph has cycles (a facade and
    # the headers it re-exports include each other), and a recursive walk that
    # memoizes on return cannot terminate on one.
    def closure(self, path: Path) -> frozenset[Path]:
        if path in self._closure_cache:
            return self._closure_cache[path]
        seen: set[Path] = set()
        pending = [path]
        while pending:
            current = pending.pop()
            if current in seen:
                continue
            seen.add(current)
            self.read(current)
            pending.extend(self.resolved.get(current, []))
        result = frozenset(seen)
        self._closure_cache[path] = result
        return result

    def spellings(self, path: Path) -> frozenset[str]:
        """Every include spelling reachable from ``path``, at any depth."""
        return frozenset(
            spelling
            for reachable in self.closure(path)
            for _, spelling in self.includes.get(reachable, [])
        )


# Every tracked name in one alternation, so a file is scanned once for all of
# them rather than once per symbol -- 58 searches over every file was the second
# cost of this check after resolution. Longest first: with `\b` boundaries a
# shorter name cannot match inside a longer one, but leftmost-first alternation
# would still let the short one win and hide the long one.
WORD_SYMBOLS: tuple[str, ...] = tuple(
    sorted(
        (
            name
            for name in (*FIRST_PARTY, *STANDARD, *THIRD_PARTY)
            if not name.endswith("::")
        ),
        key=len,
        reverse=True,
    )
)
WORD_SYMBOL_RE = re.compile(
    r"\b(?:" + "|".join(re.escape(name) for name in WORD_SYMBOLS) + r")\b"
)

# Names that are a namespace prefix rather than a name: `JPH::` is found by
# substring, which is what the per-symbol test did for them.
PREFIX_SYMBOLS: tuple[str, ...] = tuple(
    name for name in THIRD_PARTY if name.endswith("::")
)


def main() -> int:
    index = Index()
    violations: list[tuple[str, str, str]] = []
    dangling: list[tuple[str, str, int]] = []

    for path in source_files():
        relative = path.relative_to(ROOT).as_posix()
        if relative in SKIP_PATHS or relative.startswith(SKIP_PREFIXES):
            continue
        index.read(path)
        raw, body = index.texts[path]
        reached = {p.relative_to(ROOT).as_posix() for p in index.closure(path)}
        external = index.spellings(path)

        # An include that does not resolve is broken rather than third-party when
        # it can only have meant this repository: it spells the public tree, it is
        # a bare name that is exactly the name of a first-party header, or it
        # names one this tree retired -- the stale `#include "Types.hpp"` this
        # sweep had to fix is that case, and it is invisible to a rule that only
        # reads the "Zahlen/" prefix.
        for line_number, line in enumerate(raw.splitlines(), start=1):
            match = INCLUDE_RE.match(line)
            if not match:
                continue
            spelling = match.group(2)
            if index.resolve(path, match.group(1), spelling) is not None:
                continue
            if (
                spelling.startswith("Zahlen/")
                or spelling.startswith("include/")
                or Path(spelling).name in first_party_header_names()
                or Path(spelling).name in RETIRED_HEADERS
            ):
                dangling.append((relative, spelling, line_number))

        # One pass for every name in the table, then the rules for the names
        # this file actually carries. A file that defines a name itself --
        # `using TextureHandle = HeapHandle<...>` inside src/vulkan -- is not
        # reaching for someone else's, so the definition test still runs first.
        used: set[str] = set(WORD_SYMBOL_RE.findall(body))
        used.update(prefix for prefix in PREFIX_SYMBOLS if prefix in body)
        defined = defined_symbols(body)
        for symbol in used:
            if symbol in defined:
                continue
            providers = FIRST_PARTY.get(symbol)
            if providers is not None:
                if any(provider in reached for provider in providers):
                    continue
                violations.append((relative, symbol, "/".join(providers)))
                continue
            pattern = THIRD_PARTY.get(symbol)
            if pattern is not None:
                if any(re.search(pattern, spelling) for spelling in external):
                    continue
                violations.append((relative, symbol, f"an include matching {pattern}"))
                continue
            header = STANDARD.get(symbol)
            if header is not None:
                if header in external:
                    continue
                violations.append((relative, symbol, f"<{header}>"))

    if dangling:
        print(
            "Broken includes -- a first-party header is named but does not resolve:",
            file=sys.stderr,
        )
        for relative, spelling, line_number in dangling:
            print(f"  {relative}:{line_number} <{spelling}>", file=sys.stderr)

    if violations:
        print("Symbols reached without the header that declares them:", file=sys.stderr)
        current = None
        for relative, symbol, provider in sorted(violations):
            if relative != current:
                print(f"\n  {relative}", file=sys.stderr)
                current = relative
            print(f"    {symbol}  ->  needs {provider}", file=sys.stderr)
        print(
            "\nName the header yourself: a file must reach every type it spells through its own includes, "
            "not through whatever its includes happen to include.",
            file=sys.stderr,
        )
        return 1

    if dangling:
        return 1

    print(
        f"Include provenance OK ({len(source_files())} source files, "
        f"{len(FIRST_PARTY)} first-party symbols, {len(THIRD_PARTY)} third-party namespaces tracked)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
