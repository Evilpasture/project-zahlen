# Source Engine Asset Pipeline & World Ingestion Roadmap

## Overview
This roadmap tracks the complete integration of Source Engine assets (materials, textures, models, static props, and lightmaps) into Zahlen's renderer and ECS architecture, specifically targeting maps such as `gm_murder_drones.bsp` and external engine asset directories (`materials/`, `models/`, `lightmaps/`).

---

## Phase 1: Virtual File System, Material Resolution (.vmt/.vtf/.png), & Lightmap Binding (COMPLETED)
- [x] **Source VFS (`SourceVFS.hpp` / `SourceVFS.cpp`)**:
  - Case-insensitive file resolution for Windows/Linux path normalization.
  - Multi-root mounting (`import/engine_assets`, mod directories, custom paths).
  - Embedded BSP Pakfile (Lump 40 ZIP) inspection and extraction.
- [x] **VMT Parser (`VMTParser.hpp` / `VMTParser.cpp`)**:
  - Valve KeyValues lexical tokenizer and AST parser.
  - Shader classification (`LightmappedGeneric`, `VertexLitGeneric`, `UnlitGeneric`, `Water`, etc.).
  - Extraction of standard material properties:
    - `$basetexture` -> `albedoMap`
    - `$bumpmap` / `$normalmap` -> `normalMap`
    - `$translucent` / `$alphatest` -> `alphaBlend` / `alphaCutoff`
    - `$nocull` -> `doubleSided`
    - `$color` / `$color2` -> `baseColor`
- [x] **VTF Decoder (`VTFDecoder.hpp` / `VTFDecoder.cpp`)**:
  - Binary parser for Valve Texture Format (v7.0 - v7.5).
  - Decompression for compressed block formats:
    - DXT1 / BC1 (RGB 565 with 1-bit punch-through alpha)
    - DXT5 / BC3 (Interpolated alpha channel)
  - Uncompressed format conversion (BGR888, BGRA8888, RGB888, RGBA8888).
  - Mipmap level resolution and fallback selection.
- [x] **Loose Texture Support**:
  - Direct decoding for `.png`, `.jpg`, `.tga` via engine image decoders (`stbi_load_from_memory`).
- [x] **Lightmap Calculation & TEXCOORD_1 Binding (`BSPGeometry.cpp`, `BSPImporter.cpp`)**:
  - Luxel-to-UV mapping using `DTexInfo.lightmapVecs` and `DFace.lightmapTextureMinsInLuxels` / `lightmapTextureSizeInLuxels`.
  - Vertex emission into `VertexSurface.uv1` (TEXCOORD_1).
  - External PNG lightmap atlas integration (`lightmaps/<mapname>_lightmap0.png`) and internal luxel buffer sampling.
  - Binding lightmap atlas handles into `MaterialDesc.occlusionMap`.
- [x] **Verification**:
  - Comprehensive standalone unit and integration test suite (`tests/extras/TestBSPMaterials.cpp`).

---

## Phase 2: Game Lump Resolution (`LUMP_GAMELUMP` Lump 35 / `sprp` Static Props) (COMPLETED)
- [x] **Game Lump Directory Parsing (`BSPTypes.hpp`, `BSPRead.hpp`, `BSPRead.cpp`)**:
  - Parse `dgamelump_t` directory entries (`DGameLump`: ident, flags, version, fileofs, filelen) in Lump 35.
  - Safe bounds-checking and FourCC matching for `'sprp'` (0x70727073).
- [x] **Static Prop Lump ('sprp')**:
  - Parse dictionary entries (`char name[128]`, referenced `.mdl` models).
  - Parse and skip leaf mapping arrays (`uint16_t` leaf indices).
  - Dynamic stride and version handling across versions 4, 5, 6, 7, 8, 9, 10, and 11.
  - Read `origin`, `angles`, `propType`, `skin`, `diffuseModulation` (Color32), `fadeMinDist`, `fadeMaxDist`, `flags`, `forcedFadeScale`, and `uniformScale`.
- [x] **Scene & ECS Integration (`BSPScene.cpp`)**:
  - In `DescribeScene()`, convert static prop instances into `Scene::SceneEntity` with `BodyKind::Static` (when solid != 0) and `ShapeKind::Prefab`.
  - Maintain coordinate conversion to engine convention (Source Z-up inches -> engine Y-up meters) and Euler rotation.
  - Apply uniform scaling and per-instance diffuse modulation colors.
- [x] **Source VFS Model Resolution (`SourceVFS.hpp`, `SourceVFS.cpp`)**:
  - Added `ResolveModel()` for case-insensitive `.mdl` path discovery with automated prefixing and extension resolution.
- [x] **Verification**:
  - Comprehensive test suite (`tests/extras/TestBSPStaticProps.cpp`) with 6 tests covering directory parsing, version 4/8/11 unpacking, Scene entity generation, and VFS model resolution.
  - Hardened BSP parser against map discrepancies (`gm_murder_drones.bsp`):
    - Aligned `Lump` enum values with Valve Source SDK specification (`DispVerts = 33`, `Leafs = 10`, `FaceIds = 11`, `DispTris = 48`, `Cubemaps = 42`, added `FacesHDR = 58`).
    - Added fallback to `Lump::FacesHDR` when standard `Lump::Faces` length is 0 (HDR-only compilations).
    - Added adaptive leaf stride detection for 30-byte (`DLeaf`), 32-byte (padded `DLeaf`), and 56-byte (Source 2013 ambient lighting `dleaf_t`) formats.
    - Added corruption and LZMA compression detection for Game Lump static props.

---

## Phase 3: StudioModel Pipeline (.mdl, .vvd, .dx90.vtx, .phy) (COMPLETED)
- [x] **StudioModel Header & Data Structures (`StudioModelTypes.hpp`, `StudioModelLoader.hpp`, `StudioModelLoader.cpp`)**:
  - Parse `studiohdr_t` (version 44, 48, 49) with exact size `static_assert(sizeof(StudioHdr) == 248)`.
  - Parse body parts, submodels, bones, attachments, hitboxes.
  - Parse material directories (`cdtextures`) and texture names (`StudioTexture`).
  - Strict validation of header magic `'IDST'` and cross-checksum verification against VVD and VTX.
- [x] **Valve Vertex Data (`.vvd`)**:
  - Validate `VvdHeader` ('IDSV', version 4) and checksum matching.
  - Extract vertex positions, normals, tangents, texture coordinates, and bone weight arrays.
  - Evaluate fixup tables for LOD 0 slicing and re-indexing.
  - Transform positions and normals from Source coordinate frame (Z-up inches) to engine frame (Y-up meters).
- [x] **Valve Triangle Index Data (`.dx90.vtx` / `.vtx`)**:
  - Validate `VtxHeader` (version 7) and checksum matching.
  - Traverse body parts, models, LOD 0 meshes, strip groups, and strips.
  - Unpack indexed trilists and alternating-parity triangle strips into planar triangle indices.
  - Support unaligned 16-bit index buffers via memory-safe loaders.
- [x] **ModelPrefab Generation & Engine Upload (`StudioModelImporter.hpp`, `StudioModelImporter.cpp`)**:
  - Assemble cooked vertex buffers (`VertexPosition`, `VertexTangentFrame`, `VertexSurface`, `VertexSkin`) and GPU index buffers.
  - Generate meshlets via `BuildMeshlets` and `PackMeshlets`.
  - Resolve model materials via `SourceVFS` (`.vmt` scripts, `.vtf` textures, and loose image fallbacks) and assign to `ModelPrefab`.
  - Cache loaded prefabs into `AssetManager` keyed by `HashAssetPath(virtualPath)`.
  - Automatic static prop preloading integrated into `LoadBSPPrefabFromMemory` in `BSPImporter.cpp`.
- [x] **Collision / Physics Mesh (`.phy`)**:
  - Parse `PhyHeader` and `PhyCompactSurface` collision hulls.
  - Instantiate Jolt physics box colliders / convex hulls when `options.buildColliders` is enabled.
- [x] **Verification**:
  - Comprehensive unit test suite (`tests/extras/TestBSPStudioModel.cpp`) covering header parsing, checksum mismatch rejection, VVD fixup table LOD evaluation, VTX strip group unpacking, PHY collision parsing, and end-to-end geometry coordinate assembly.

---

## Phase 4: Dynamic Prop Entities & Ragdoll / Physics Binding (PLANNED)
- [ ] **Dynamic Prop Entities**:
  - Map `prop_physics`, `prop_dynamic`, `prop_door_rotating` to ECS components.
  - Bind physics shapes (`JPH::ConvexHullShape`) to `PhysicsComponent` with mass, inertia, and surface properties.
- [ ] **Multi-LOD Runtime Switching**:
  - Parse LOD 1..N from `.vtx` and `.vvd` for distance-based LOD streaming.
