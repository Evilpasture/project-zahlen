// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/Render/AssetDefaults.hpp>
#include <array>
#include <cstdint>
#include <string_view>
namespace ZHLN::GLTF {
enum class CapabilityKind : uint8_t { Core, Extension };
enum class CapabilitySupport : uint8_t { Supported, Partial };

struct Capability {
    std::string_view  name;
    CapabilityKind    kind;
    CapabilitySupport support = CapabilitySupport::Supported;
    std::string_view  limitation;
};

// What the importer actually consumes (not everything cgltf can parse).
// "Supported" means the data is routed to our renderer, not that its output
// is pixel-identical to a reference glTF viewer. Partial capabilities emit a
// yellow warning. Undeclared extensions are *not* implicitly supported.
static constexpr std::array kCapabilities {
    Capability {.name = "glTF 2.0 nodes, transforms and meshes", .kind = CapabilityKind::Core},
    Capability {.name = "TRIANGLES primitives", .kind = CapabilityKind::Core},
    Capability {.name = "pbrMetallicRoughness", .kind = CapabilityKind::Core},
    Capability {.name = "TEXCOORD_0/1", .kind = CapabilityKind::Core},
    Capability {.name = "COLOR_0", .kind = CapabilityKind::Core},
    Capability {.name = "JOINTS_0/WEIGHTS_0", .kind = CapabilityKind::Core},
    Capability {.name = "POSITION morph targets (first four)", .kind = CapabilityKind::Core},
    Capability {.name = "STEP animation", .kind = CapabilityKind::Core},
    Capability {.name = "LINEAR animation", .kind = CapabilityKind::Core, .support = CapabilitySupport::Partial, .limitation = "playback eases between keys"},
    Capability {.name = "KHR_mesh_quantization", .kind = CapabilityKind::Extension},
    Capability {.name = "KHR_texture_transform", .kind = CapabilityKind::Extension},
    Capability {.name = "KHR_materials_unlit", .kind = CapabilityKind::Extension},
    Capability {.name = "KHR_materials_emissive_strength", .kind = CapabilityKind::Extension},
    Capability {.name = "KHR_materials_clearcoat", .kind = CapabilityKind::Extension},
    Capability {.name = "KHR_materials_transmission", .kind = CapabilityKind::Extension},
    Capability {.name = "KHR_materials_ior", .kind = CapabilityKind::Extension},
    Capability {
        .name       = "KHR_materials_volume",
        .kind       = CapabilityKind::Extension,
        .support    = CapabilitySupport::Partial,
        .limitation = "attenuation color/distance are ignored"
    },
    Capability {.name = "KHR_materials_iridescence", .kind = CapabilityKind::Extension},
    Capability {.name = "KHR_materials_sheen", .kind = CapabilityKind::Extension},
    Capability {.name = "KHR_materials_anisotropy", .kind = CapabilityKind::Extension},
    // The response is resolved per fragment in every lit path: the forward and
    // transmission shaders read the instance, and the opaque path carries it
    // through the clearcoat G-buffer lanes and decodes it again in the lighting,
    // reflection and RTGI passes (see pbr_helpers.slang). Three shapes of the
    // extension are narrower than the spec. specularColorFactor above 1 is
    // clamped on import, since the response lane holds a multiple of the 0.04
    // baseline. The two texture references share the one sampler slot the
    // material layout has left, so when they name different images only the
    // strength half is bound, on the strength half's sampler and UV transform.
    // And a material that also declares
    // KHR_materials_clearcoat keeps the coat's lanes, so its opaque response
    // stays at the IOR baseline -- transmission still honors the extension.
    Capability {.name = "KHR_materials_specular", .kind = CapabilityKind::Extension},
    Capability {.name = "KHR_lights_punctual", .kind = CapabilityKind::Extension},
};

// Validation is advisory: even an unsupported *required* extension does not
// veto an otherwise readable model. E.g. KHR_materials_pbrSpecularGlossiness
// without a core PBR fallback renders with the default gray material. Parse
// failures and cgltf buffer-loading failures still fail the import.
// The engine's default presentation uses a low exposure and boosts imported
// emission accordingly. A glTF fidelity renderer instead sets this to 1.0f:
// emissiveFactor * KHR_materials_emissive_strength then stays in glTF's
// authored linear units. Emissive texture sampling remains sRGB -> linear in
// either mode. A virtualPath identifies one prefab AND one set of options;
// reusing a cached path with different options is rejected.
struct ImportOptions {
    float emissiveFactorScale = kGLTFEmissiveDisplayScale;
    // Preserve authored detail up to this edge length. The default keeps the
    // engine's existing texture memory budget; fidelity stills can opt into
    // higher resolution without changing other clients' imports.
    uint32_t maxTextureDimension = kGLTFDefaultMaxTextureDimension;
};

} // namespace ZHLN::GLTF
