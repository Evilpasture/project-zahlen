"""Cross-check the importer/shader ABI without needing a headless Vulkan device.

The typed GLB fixture in TestGLTFImport.cpp exercises the actual importer when a
GPU build is available. This test pins the wiring and the UV matrix algebra in
lighter CI environments; it does not claim to compile Slang or render a golden.
"""

from __future__ import annotations

import math
import re
import struct
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def source(path: str) -> str:
    return (ROOT / path).read_text()


class SheenTransformWiringTest(unittest.TestCase):
    def test_slot_numbers_and_two_sampler_words_match(self) -> None:
        types = source("include/Zahlen/Render/Types.hpp")
        shader = source("resources/shaders/common.slang")
        slots = types.split("enum class MaterialTextureSlot", 1)[1].split("Count", 1)[0]
        names = [line.strip().rstrip(",") for line in slots.splitlines() if line.strip().endswith(",")]
        self.assertEqual(len(names), 14)
        for index, name in enumerate(names):
            self.assertRegex(shader, rf"kSampler{name}\s*= {index}u;")
        self.assertIn("public float4 uvRow0[14];", source("resources/shaders/instance_data.slang"))
        self.assertIn("public float4 uvRow1[14];", source("resources/shaders/instance_data.slang"))
        self.assertIn("first + i < addresses.size()", source("src/render/RenderDrawCommands.cpp"))
        self.assertIn("slot < 8u ? codes.x : codes.y", shader)
        self.assertIn("-> std::unordered_map<cgltf_image*, std::array<TextureHandle, 2>>", source("extras/glTF/GLTFImporter.cpp"))

    def test_attribute_stride_matches_cpu_skinned_and_raster_paths(self) -> None:
        cpu = source("include/Zahlen/Vertex.hpp")
        raster = source("resources/shaders/instance_data.slang")
        skin = source("resources/shaders/skinning.slang")
        for name in ("normal", "tangent", "uv", "color", "uv1"):
            self.assertRegex(cpu, rf"Packed\w+\s+{name};")
            self.assertRegex(raster, rf"public uint {name};")
            self.assertRegex(skin, rf"uint {name};")
        self.assertIn("outNorm, outTang, uvRaw, colorRaw, attr.uv1", skin)
        self.assertIn("meshHeader.version = 5;", source("tools/zcook/Cook.cpp"))
        self.assertIn("kMeshVersion          = 5u", source("tests/helpers/CookerFixture.hpp"))

    def test_typed_fixture_contains_decodable_png(self) -> None:
        fixture = source("tests/render/TestGLTFImport.cpp").split("MakeSheenTransformFixture()", 1)[1]
        png = fixture.split("png {", 1)[1].split("};", 1)[0]
        data = bytes(int(octet, 16) for octet in re.findall(r"0x([0-9A-Fa-f]+)u", png))
        self.assertEqual(len(data), 70)
        self.assertTrue(data.startswith(b"\x89PNG\r\n\x1a\n"))
        pos = 8
        while pos < len(data):
            length = struct.unpack_from(">I", data, pos)[0]
            kind, payload = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + length]
            checksum = struct.unpack_from(">I", data, pos + 8 + length)[0]
            self.assertEqual(zlib.crc32(kind + payload), checksum)
            pos += 12 + length
        self.assertEqual(pos, len(data))

    def test_transform_order_override_and_mirrored_v(self) -> None:
        importer = source("extras/glTF/GLTFImporter.cpp")
        upload = source("src/render/RenderDrawCommands.cpp")
        shader = source("resources/shaders/material_model.slang")
        self.assertIn("view.transform.has_texcoord ? view.transform.texcoord : view.texcoord", importer)
        self.assertIn("attributes.unpackedUV1", source("resources/shaders/common.slang"))
        self.assertIn("cgltf_accessor_read_float(uv1Acc, vIdx, uv1, 2)", importer)
        self.assertIn("-s * transform.scale[1]", upload)
        self.assertIn("c * transform.scale[1]", upload)
        self.assertIn("dot(row0.xy, uv) + row0.z", shader)
        self.assertIn("dot(row1.xy, uv) + row1.z", shader)
        self.assertIn("row1.w == 1.0f ? uv1", shader)
        self.assertIn("TransformNormalSample(normalMap, i, kSamplerNormal)", shader)

        def apply(uv: tuple[float, float], offset: tuple[float, float],
                  scale: tuple[float, float], angle: float) -> tuple[float, float]:
            u, v = (uv[0] * scale[0], uv[1] * scale[1])
            return (offset[0] + math.cos(angle) * u - math.sin(angle) * v,
                    offset[1] + math.sin(angle) * u + math.cos(angle) * v)

        transformed = apply((0.5, 0.2), (0.2, 0.4), (2.0, -3.0), math.pi / 2)
        tiled = apply((0.25, 0.5), (0.0, 0.0), (30.0, -30.0), 0.0)
        self.assertAlmostEqual(transformed[0], 0.8)
        self.assertAlmostEqual(transformed[1], 1.4)
        self.assertEqual(tiled, (7.5, -15.0))

    def test_sheen_layer_and_color_space_are_distinct(self) -> None:
        importer = source("extras/glTF/GLTFImporter.cpp")
        material = source("resources/shaders/material_model.slang")
        lighting = source("resources/shaders/lighting.slang")
        reflection = source("resources/shaders/reflection.slang")
        self.assertIn("const ImportedTextureRef ref {image, srgb}", importer)
        self.assertIn("slot == MaterialTextureSlot::SheenColor", importer)
        self.assertIn("texJob.isSRGB ? 1 : 0", importer)
        self.assertIn("i.sheenRoughnessTexIndex", material)
        self.assertIn("kSamplerSheenRoughness).a", material)
        self.assertIn("kSamplerSheenColor).rgb", material)
        self.assertIn("s.anisotropy.w = s.occlusion", material)
        self.assertIn("SheenCharlieBRDF", lighting)
        self.assertIn("sheenAttSun", lighting)
        self.assertIn("sheenIBL", reflection)
        self.assertLess(reflection.index("float4 sheenRaw"), reflection.index("float4 coatRaw"))
        self.assertIn("info.maxLod = VK_LOD_CLAMP_NONE", source("src/render/init/RenderInitHeaps.cpp"))

    def test_seventh_attachment_is_written_and_read(self) -> None:
        shaders = ["resources/shaders/basic.slang", "resources/shaders/mesh_particle_render.slang"]
        for path in shaders:
            self.assertIn("float4 sheen : SV_Target6;", source(path))
        for path in ["GBufferBasePass", "GBufferResolvePass", "ViewmodelPass"]:
            root = "src/render/passes/gbuffer/"
            self.assertIn("Vk::ColorWrite<Res_Sheen>", source(root + path + ".hpp"))
            self.assertIn(".AddColor(in.sheen", source(root + path + ".cpp"))
        for path in ["ClusteredLightingPass", "ReflectionCompositePass"]:
            root = "src/render/passes/lighting/"
            self.assertIn("Vk::ShaderRead<Res_Sheen>", source(root + path + ".hpp"))
            self.assertIn('Vk::Slot<"texSheen">', source(root + path + ".cpp"))
        self.assertIn("Res_TransSheen    transSheenBuffer", source("src/render/TargetManager.hpp"))
        self.assertIn(".AddColor(sheen_att", source("src/render/passes/forward/TranslucentPrePass.cpp"))
        self.assertIn("Vk::ShaderRead<Res_TransSheen>", source("src/render/passes/lighting/TranslucentReflectionPass.hpp"))
        # A bound resource must also be declared on the graph pass. This
        # guards against accidentally replacing the clearcoat read with sheen.
        for path in ["ClusteredLightingPass", "ReflectionCompositePass", "TranslucentReflectionPass"]:
            stem = "src/render/passes/lighting/" + path
            reads = set(re.findall(r"Vk::Assume<Vk::(?:ShaderRead|ShaderReadGeneral)<(Res_\w+)>>", source(stem + ".cpp")))
            declaration = source(stem + ".hpp")
            for resource in reads:
                self.assertRegex(declaration, rf"Vk::(?:ShaderRead|ShaderReadGeneral)<{resource}>")


if __name__ == "__main__":
    unittest.main()
