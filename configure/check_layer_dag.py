#!/usr/bin/env python3
"""Forbid the one layer edge that must never exist: anything reaching into gameplay/.

check_core_layer_boundary.py covers one edge of the DAG: nothing under src/,
include/ or modules/ may reach into plugins/, extensions/ or gameplay/. It leaves
the edges *between* the optional layers to reader discipline, and the one that
matters had already been crossed:

  * extensions/Scripting/Lua/Scripting.cpp included
    <RagdollAuthoring/RagdollAuthoring.hpp> and called
    ZHLN::RagdollAuthoring::BuildHumanoidBipedRagdoll, and its target linked
    zahlen_ragdoll_authoring;
  * extensions/Scripting/Lua/DataModelBindings.cpp included
    <DataModel/DataModel.hpp>, and the same target linked zahlen_datamodel.

Both were circular in effect: a gameplay module could not run under a host that
did not link the scripting layer, and the scripting layer could not compile
without the gameplay modules. gameplay/ is the top of the optional stack --
domain logic is written *against* the reusable layers and never the other way
around -- so this check rejects a file under plugins/ or extensions/ that
includes a first-party header owned by gameplay/.

It reads sources and nothing else. The build graph is not inspected: CMake is
the top of the chain and no script rules on it. A target that links gameplay
without including it cannot call anything from it, so an edge that matters is
an include first, and that is the half this file checks.

Includes are resolved against the roots a layer actually publishes, the same
resolution check_core_layer_boundary.py uses: an include that no layer root
contains -- <Zahlen/Render/Types.hpp> off core's include/ -- is core's and is
not this check's business, and a quoted include is resolved next to the file
that wrote it first, exactly as the compiler would, so a relative hop out of the
layer is caught too.

Not enforced here, deliberately: plugins/ and extensions/ consume each other in
both directions in this tree (UITOML is a plugin that names GUI::UINode from
extensions/UI, and the network extensions build on the plugin serializers).
Those edges are a design decision recorded in ARCHITECTURE.md 1.2, not an
accident, and inventing a rule against them would mean failing the build on the
existing composition. gameplay/ is the only layer with a direction that is not
negotiable.

Composition is not checked: app/, samples/, tests/, tools/ and the root
CMakeLists may select any layer, and are expected to. The rule is about what a
layer may assume about another, not about who may assemble them.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# The layer nothing else may consume, and the layers that may not consume it.
GAMEPLAY = "gameplay"
BELOW_GAMEPLAY = ("plugins", "extensions")
SKIP_DIR_NAMES = {".git", "build", "target", "__pycache__", ".cache", "node_modules"}

# Where each layer publishes headers from: the whole tree, plus the nested roots
# a target adds on top. Mirrors LAYER_INCLUDE_ROOTS in
# check_core_layer_boundary.py so both checks agree on what a spelling resolves
# to.
LAYER_INCLUDE_ROOTS = {
    GAMEPLAY: (ROOT / GAMEPLAY, ROOT / GAMEPLAY / "ProjectLight"),
    "plugins": (ROOT / "plugins",),
    "extensions": (ROOT / "extensions", ROOT / "extensions" / "net"),
}

# Core's include roots: an include that resolves here is core's, whatever a
# basename elsewhere happens to collide with.
FIRST_PARTY_CORE_ROOTS = (ROOT / "include", ROOT / "src", ROOT / "modules")
VENDOR_CORE_ROOTS = (ROOT / "extern", ROOT / "third_party")
CORE_INCLUDE_ROOTS = (*FIRST_PARTY_CORE_ROOTS, *VENDOR_CORE_ROOTS)

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".cppm", ".ixx"}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.MULTILINE)

_LAYER_BASENAME_INDEX: dict[str, Path] | None = None
_FIRST_PARTY_CORE_BASENAMES: set[str] | None = None
_VENDOR_CORE_BASENAMES: set[str] | None = None


def _walk_files(root: Path):
    if not root.is_dir():
        return
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIR_NAMES)
        base = Path(dirpath)
        for name in sorted(filenames):
            candidate = base / name
            if candidate.is_file():
                yield candidate


def _layer_basename_index() -> dict[str, Path]:
    global _LAYER_BASENAME_INDEX
    if _LAYER_BASENAME_INDEX is None:
        index: dict[str, Path] = {}
        for _layer, roots in LAYER_INCLUDE_ROOTS.items():
            for root in roots:
                for candidate in _walk_files(root):
                    index.setdefault(candidate.name, candidate)
        _LAYER_BASENAME_INDEX = index
    return _LAYER_BASENAME_INDEX


def _first_party_core_basenames() -> set[str]:
    global _FIRST_PARTY_CORE_BASENAMES
    if _FIRST_PARTY_CORE_BASENAMES is None:
        names: set[str] = set()
        for root in FIRST_PARTY_CORE_ROOTS:
            for candidate in _walk_files(root):
                names.add(candidate.name)
        _FIRST_PARTY_CORE_BASENAMES = names
    return _FIRST_PARTY_CORE_BASENAMES


def _vendor_core_basenames() -> set[str]:
    global _VENDOR_CORE_BASENAMES
    if _VENDOR_CORE_BASENAMES is None:
        names: set[str] = set()
        for root in VENDOR_CORE_ROOTS:
            for candidate in _walk_files(root):
                names.add(candidate.name)
        _VENDOR_CORE_BASENAMES = names
    return _VENDOR_CORE_BASENAMES


def normalize(text: str) -> str:
    return text.replace("\\", "/")


def layer_of(path: Path) -> str | None:
    """The optional layer that owns ``path``, or None for core/composition."""
    try:
        relative = path.resolve().relative_to(ROOT.resolve())
    except ValueError:
        return None
    return relative.parts[0] if relative.parts and relative.parts[0] in (GAMEPLAY, *BELOW_GAMEPLAY) else None


def resolve_in_layer(include: str) -> Path | None:
    """The file ``include`` names under a layer's published roots, if any."""
    relative = normalize(include)
    if not relative or relative.startswith("/") or ".." in Path(relative).parts:
        return None

    for layer, roots in LAYER_INCLUDE_ROOTS.items():
        for root in roots:
            candidate = root / relative
            if candidate.is_file():
                return candidate

    # A slash-free include may resolve by basename on a published root, the way
    # a compiler would when the file sits at the top of an include directory.
    # Core wins that race in a real build (its roots come first on the include
    # path), so a name core also has is not treated as a layer include.
    # Check the pre-built optional-layer basename index first so stdlib/system
    # includes (<vector>, <string>, ...) never walk extern/ (Slang, Jolt, ...).
    if "/" not in relative:
        candidate = _layer_basename_index().get(relative)
        if candidate is None:
            return None
        if relative in _first_party_core_basenames() or relative in _vendor_core_basenames():
            return None
        return candidate
    return None


def gameplay_include(source: Path, delimiter: str, include: str) -> Path | None:
    relative = normalize(include)
    if not relative or relative.startswith("/"):
        return None

    # Quoted includes are searched next to the including file first, exactly as
    # the compiler does. This is what catches a relative hop out of the layer --
    # and it is also where the search *stops*: a quoted include that exists
    # beside its includer never reaches the include path, so a same-named file
    # elsewhere must not be reported. Two layers each owning a
    # "BSPInteraction.hpp" is legal; the plugin's own file is what
    # #include "BSPInteraction.hpp" resolves to inside plugins/BSP, and
    # gameplay's is reached by the angled <Interaction/...> spelling instead.
    if delimiter == '"':
        beside = (source.parent / relative).resolve()
        if beside.is_file():
            return beside if layer_of(beside) == GAMEPLAY else None

    resolved = resolve_in_layer(include)
    if resolved is not None and layer_of(resolved) == GAMEPLAY:
        return resolved
    return None


def check_includes(violations: list[str]) -> None:
    for layer in BELOW_GAMEPLAY:
        root = ROOT / layer
        if not root.is_dir():
            continue
        for source in (path for path in _walk_files(root) if path.suffix.lower() in SOURCE_SUFFIXES):
            text = source.read_text(encoding="utf-8", errors="ignore")
            for match in INCLUDE_RE.finditer(text):
                owner = gameplay_include(source, match.group(1), match.group(2))
                if owner is None:
                    continue
                violations.append(
                    f"{source.relative_to(ROOT)} includes {owner.relative_to(ROOT)} "
                    f"(written as '{match.group(2)}'): {layer}/ may not consume {GAMEPLAY}/"
                )


def main() -> int:
    violations: list[str] = []

    check_includes(violations)

    if violations:
        print(f"Layer edge into {GAMEPLAY}/ found where none is allowed:", file=sys.stderr)
        for violation in sorted(set(violations)):
            print(f"  - {violation}", file=sys.stderr)
        print(
            "Invert it: have the lower layer name a capability it owns -- a registry entry, "
            "an event, an interface -- and let the gameplay layer implement it. "
            "Zahlen/ScriptCommandRegistry.hpp is the worked example.",
            file=sys.stderr,
        )
        return 1

    print("Layer DAG OK (no plugins/ or extensions/ source includes a gameplay/ header).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
