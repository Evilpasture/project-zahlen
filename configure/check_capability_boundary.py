#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reject optional layers that reach past the capability boundary.

``include/`` is the engine's API and the capability facades on ``Engine`` are
what it hands out. An optional layer -- ``extensions/``, ``plugins/``,
``gameplay/``, ``samples/``, ``app/``, ``utils/`` -- consumes that API. It must
not reach the objects behind the facades, because holding one is how a consumer
becomes a second owner of engine state.

What is checked, and why each one is a door rather than a style preference:

1. **The internal asset-cache header.** ``<Zahlen/Engine/AssetManager.hpp>``
   declares the cache and, with it, the cooked container/pak formats and the
   mount/load verbs. A consumer that includes it can mount a pak the engine is
   already using and evict prefabs out from under a live scene. The capability
   is ``Engine::Assets()``; the cook formats belong to a build-tree tool.

2. **The removed engine accessors.** ``GetPlatformHost``, ``GetWindow``,
   ``GetInternalRegistry``, ``GetUpdateGraph`` and ``GetRenderGraph`` are
   private now, so a consumer naming one does not compile -- but the name coming
   back in a diff is the moment to ask why, not after the build breaks. The
   capability spellings are ``Engine::Host()``, ``Engine::Entities()`` and a
   module's ``addGraphSystems``.

3. **An internal service as a system parameter.** ``ECS::Res<AssetManager>``,
   ``ECS::Res<RenderContext>`` and their siblings ask a graph for the object the
   bundle deliberately withholds. ``SystemGraph``'s resolver already fails the
   build for these (see ``IsWithheldService``); this check reports the same
   mistake where it is written, with the capability to use instead.

Deliberately *not* checked: ``Module``, ``SystemGraph``, ``EntityCommandBuffer``,
``Entities`` and ``Query``. Those are the sanctioned extension surface -- a
module is what an optional layer installs, a graph is what it adds systems to,
and a command buffer is an ordinary system parameter. The public headers
themselves are likewise out of scope: they are the API, and a forward
declaration in one is how a header names a type it does not use.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent

CONSUMER_ROOTS = ("extensions", "plugins", "gameplay", "samples", "app", "utils")

SOURCE_SUFFIXES = frozenset({".h", ".hpp", ".hh", ".hxx", ".inl", ".ipp", ".cpp", ".cc", ".cxx", ".cppm", ".ixx"})

SKIP_DIR_NAMES = frozenset({".git", "build", "target", "__pycache__", ".cache", "node_modules", "extern", "third_party"})

# Completed COM host ports are not allowed to regress to native facades. The
# explicit set grows as plugin/extension ports land; remaining native targets
# are not silently counted as migrated.
COM_HOST_PORTS = frozenset({
    "extensions/VFX/ExplosionHost.hpp", "extensions/VFX/ExplosionHost.cpp", "extensions/VFX/ExplosionPolicy.hpp",
    "extensions/VFX/LightningHost.hpp", "extensions/VFX/LightningHost.cpp",
    "extensions/VFX/LightningGeometry.hpp", "extensions/VFX/LightningGeometry.cpp",
    "extensions/Terrain/TerrainGeometry.cpp", "extensions/Terrain/TerrainGeometry.hpp",
    "extensions/Terrain/TerrainHost.cpp", "extensions/Terrain/TerrainHost.hpp",
    "extensions/VFX/CombatFX.hpp", "extensions/VFX/CombatFXLifecycle.cpp",
    "utils/scene/TerrainDiagnostic.hpp",
    "utils/scene/main.cpp", "gameplay/FallbackScene/FallbackScene.cpp", "gameplay/FallbackScene/FallbackScene.hpp",
    "extensions/FreeCam/FreeCam.cpp", "extensions/FreeCam/FreeCam.hpp",
    "extensions/Camera/TargetCamera.cpp", "extensions/Camera/TargetCamera.hpp",
    "utils/scene/SceneDocument.cpp", "utils/scene/SceneDocument.hpp",
    "plugins/Scene/Scene.cpp", "plugins/Scene/Scene.hpp",
    "plugins/BSP/BSPScene.cpp", "plugins/BSP/BSPScene.hpp",
    "plugins/Scene/SceneFields.hpp", "plugins/Scene/SceneExtraction.hpp",
    "app/Application.cpp", "app/HostUI.cpp", "app/HostRunner.cpp", "app/main.cpp", "app/UIEditor.cpp",
    "app/include/Zahlen/App.hpp", "app/include/Zahlen/HostUI.hpp", "app/include/Zahlen/HostRunner.hpp",
    "app/include/Zahlen/Zahlen.hpp", "plugins/Fonts/Fonts.cpp", "plugins/Fonts/Fonts.hpp",
    "plugins/BSP/StudioModelAssets.cpp", "plugins/BSP/StudioModelAssets.hpp",
    "plugins/BSP/StudioMaterial.hpp", "plugins/BSP/VMTLighting.cpp", "plugins/BSP/VMTLighting.hpp",
    "plugins/BSP/SourceVFS.cpp", "plugins/BSP/SourceVFS.hpp",
    "plugins/BSP/BSPAssets.cpp", "plugins/BSP/BSPAssets.hpp", "plugins/BSP/BSPMaterial.hpp",
    "plugins/BSP/BSPPreparation.cpp", "plugins/BSP/BSPPreparation.hpp",
    "plugins/BSP/AssetUpload.hpp", "plugins/BSP/SourceAsset.hpp",
    "plugins/glTF/GLTFDocument.cpp", "plugins/glTF/GLTFDocument.hpp",
    "plugins/glTF/GLTFAssets.cpp", "plugins/glTF/GLTFAssets.hpp", "plugins/glTF/GLTFOptions.hpp",
})
NATIVE_HOST_HEADER = re.compile(r"^(?:FreeCam/NativeFreeCam\.hpp|Camera/NativeTargetCamera\.hpp|NativeHostUI/|Scene/NativeScene\.hpp|NativeSceneDocument\.hpp|Zahlen/(?:Engine/(?:Engine|ModelPrefab|PrefabFactory|FramePlan|GameModule|Scene)\.hpp|Engine/Capabilities/|ECS/ECS\.hpp|Render/(?:Render|RenderContext)\.hpp|Window/(?:Window|PlatformHost)\.hpp))")

# The internal cache header, and the paths allowed to include it.
INTERNAL_HEADERS = {"Zahlen/Engine/AssetManager.hpp": "Engine::Assets() -> AssetLibrary"}
ALLOWED_INTERNAL_HEADER_INCLUDERS = {
    # Reproduces the cook path; it reads the cooked container formats and writes
    # the cache the pipeline produces. Not a host program.
    "utils/zcook/Cook.cpp",
    "utils/zcook/FontBake.cpp",
}

# Removed accessors, and what replaces each.
REMOVED_ACCESSORS = {
    "GetPlatformHost": "Engine::Host()",
    "GetWindow": "Engine::Host()",
    "GetInternalRegistry": "Engine::Entities()",
    "GetUpdateGraph": "Module::addGraphSystems",
    "GetRenderGraph": "Module::addGraphSystems",
}

# A system parameter that asks for an object no bundle provides. PhysicsContext,
# RenderContext, AudioContext and SceneData are *not* here: they are members of
# SimServices/RenderServices, and a node in the owning graph resolves them by
# type on purpose. The cache is the one the bundles stopped carrying.
WITHHELD_PARAMETERS = {
    "AssetManager": "Engine::Assets()",
    "PlatformHost": "Engine::Host()",
}

# Known remaining graph access. Closing these is its own item -- each one is a
# synchronous system invocation from a host entry point, and each needs a
# supported way to run one system outside the frame before the call can be
# rewritten. Listed so the set cannot grow: a new one fails this check, and a
# fixed one should be deleted from here.
PENDING_GRAPH_ACCESS = {
    ("extensions/Camera/NativeTargetCamera.cpp", "GetRenderGraph"),
    ("extensions/CharacterController/CharacterController.cpp", "GetUpdateGraph"),
    ("extensions/FreeCam/NativeFreeCam.cpp", "GetRenderGraph"),
    ("extensions/VFX/LightningUpdate.cpp", "GetUpdateGraph"),
    ("extensions/net/Network/NetworkReplicator.cpp", "GetUpdateGraph"),
    ("gameplay/Interaction/BSPInteraction.cpp", "GetUpdateGraph"),
    ("samples/BSPMurderDronesSample.cpp", "GetUpdateGraph"),
}

COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)
INCLUDE = re.compile(r'#\s*include\s*[<"]([^>"]+)[>"]')


def strip_comments(text: str) -> str:
    return COMMENT.sub(" ", text)


def line_of(text: str, pattern: re.Pattern[str]) -> int:
    match = pattern.search(text)
    if match is None:
        return 0
    return text.count("\n", 0, match.start()) + 1


def check_source(path: Path) -> list[str]:
    relative = path.relative_to(REPOSITORY_ROOT).as_posix()
    try:
        raw = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []

    violations: list[str] = []

    # 1. The internal cache header.
    for line_number, line in enumerate(raw.splitlines(), start=1):
        match = INCLUDE.search(line)
        if match is None:
            continue
        header = match.group(1)
        if relative in COM_HOST_PORTS and NATIVE_HOST_HEADER.search(header):
            violations.append(f"{relative}:{line_number}: migrated COM host includes native header <{header}>")
        if header in INTERNAL_HEADERS and relative not in ALLOWED_INTERNAL_HEADER_INCLUDERS:
            violations.append(f"{relative}:{line_number}: includes <{header}>; use {INTERNAL_HEADERS[header]} instead")

    text = strip_comments(raw)

    if relative in COM_HOST_PORTS:
        native = re.compile(r"\bEngine\s*(?:[&*]|::)|\b(?:NativeHostUI|SetWindowDrawCallback|SetUICallback)\b")
        if native.search(text):
            violations.append(f"{relative}:{line_of(text, native)}: migrated host must use COM IEngine/capabilities and explicit UI submission")

    # 2. A removed accessor, called or declared.
    for accessor, replacement in REMOVED_ACCESSORS.items():
        pattern = re.compile(rf"(?<![\w]){accessor}\s*\(")
        if pattern.search(text) and (relative, accessor) not in PENDING_GRAPH_ACCESS:
            violations.append(f"{relative}:{line_of(text, pattern)}: calls {accessor}(); use {replacement} instead")

    # 3. An internal service as a system parameter.
    for name, replacement in WITHHELD_PARAMETERS.items():
        pattern = re.compile(rf"ECS::(?:Res|ResMut|Optional)\s*<\s*(?:const\s+)?{name}\s*&?\s*>")
        if pattern.search(text):
            violations.append(
                f"{relative}:{line_of(text, pattern)}: asks a graph for {name}; use {replacement} instead"
            )

    return violations


def main() -> int:
    if not any((REPOSITORY_ROOT / root).is_dir() for root in CONSUMER_ROOTS):
        print(f"ERROR: no consumer tree found under {REPOSITORY_ROOT}", file=sys.stderr)
        return 2

    violations: list[str] = []
    for root in CONSUMER_ROOTS:
        directory = REPOSITORY_ROOT / root
        if not directory.is_dir():
            continue
        for path in sorted(directory.rglob("*")):
            if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            if SKIP_DIR_NAMES.intersection(path.parts):
                continue
            violations.extend(check_source(path))

    if violations:
        print("Capability boundary violations:", file=sys.stderr)
        for violation in violations:
            print(f"  - {violation}", file=sys.stderr)
        print(
            "An optional layer talks to the engine through the capability on Engine, not through the object "
            "behind it. If the capability is missing an operation, add it there; if the caller is genuinely "
            "engine code, move it into src/ rather than widening the boundary.",
            file=sys.stderr,
        )
        return 1

    print("Capability boundary OK (consumers use capabilities; no internal service and no removed accessor).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
