// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Meshlet.hpp>
#include <Zahlen/Types.hpp>
#include <cmath>
#include <cmath>
#include <cstring>
#include <vector>

enum class MeshletTestError : uint8_t {
    LayoutDrift[[= ZHLN::Description<"A GPU vertex/meshlet struct changed size, so cooked and JIT streams no longer match the shaders.">{}]] = 1,
    NotDeterministic[[= ZHLN::Description<"BuildMeshlets produced different output for identical input.">{}]],
    StreamInvariantBroken[[= ZHLN::Description<"A meshlet stream invariant the mesh shader relies on was violated.">{}]],
    StrideForwardingBroken[[= ZHLN::Description<"The VertexPosition overload disagreed with the explicit-stride form.">{}]],
};

namespace {

// A subdivided grid rather than a bare cube: kMeshletMaxTriangles is 124, so a
// 12-triangle cube lands in a single meshlet and the interesting invariants --
// per-meshlet 4-byte re-alignment of the micro-index stream, and padding between
// meshlets -- would never be exercised. 32x32 quads gives ~2048 triangles, which
// spans many meshlets and forces non-multiple-of-4 triangle counts to be padded.
constexpr uint32_t kGridQuads = 32;

std::vector<ZHLN::VertexPosition> MakeGridPositions() {
    std::vector<ZHLN::VertexPosition> out;
    out.reserve((kGridQuads + 1) * (kGridQuads + 1));
    for (uint32_t z = 0; z <= kGridQuads; ++z) {
        for (uint32_t x = 0; x <= kGridQuads; ++x) {
            out.push_back({{static_cast<float>(x) * 0.25f, std::sin(static_cast<float>(x) * 0.3f) * 0.5f, static_cast<float>(z) * 0.25f}});
        }
    }
    return out;
}

std::vector<uint32_t> MakeGridIndices() {
    std::vector<uint32_t> out;
    out.reserve(kGridQuads * kGridQuads * 6);
    const uint32_t stride = kGridQuads + 1;
    for (uint32_t z = 0; z < kGridQuads; ++z) {
        for (uint32_t x = 0; x < kGridQuads; ++x) {
            const uint32_t a = z * stride + x;
            const uint32_t b = a + 1;
            const uint32_t c = a + stride;
            const uint32_t d = c + 1;
            out.insert(out.end(), {a, c, b, b, c, d});
        }
    }
    return out;
}

// Byte-level comparison of the three streams the GPU consumes.
bool StreamsEqual(const ZHLN::MeshletBuildResult& a, const ZHLN::MeshletBuildResult& b) {
    if (a.meshlets.size() != b.meshlets.size() || a.vertices.size() != b.vertices.size() || a.triangles.size() != b.triangles.size()) {
        return false;
    }
    return std::memcmp(a.meshlets.data(), b.meshlets.data(), a.meshlets.size() * sizeof(ZHLN::GPUMeshlet)) == 0 &&
           std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(uint32_t)) == 0 &&
           std::memcmp(a.triangles.data(), b.triangles.data(), a.triangles.size()) == 0;
}

} // namespace

struct MeshletTestSuite {
    struct Tests {
        // The cooker writes sizeof(VertexAttributes) etc. straight into .zmesh
        // (Cook.cpp:97-114) and the JIT importer sizes its VBOs the same way
        // (GLTFImporter.cpp:553-558). Both move together if a packing type
        // changes, so the C++ side stays self-consistent -- but the compiled
        // shaders would silently disagree.
        //
        // GPUMeshlet is already pinned by static_assert (Types.hpp:186-187);
        // VertexPosition/VertexAttributes/VertexSkin (Types.hpp:64-77) are only
        // documented in comments, so those three are the real gap covered here.
        // GPUMeshlet is re-checked as a guard against the assert being removed.
        std::expected<void, ZHLN::Error> gpu_stream_layout_is_pinned() {
            auto chkPos = ZHLN::Test::AssertEq(sizeof(ZHLN::VertexPosition), size_t {12});
            if (!chkPos) {
                return chkPos;
            }
            auto chkAttr = ZHLN::Test::AssertEq(sizeof(ZHLN::VertexAttributes), size_t {16});
            if (!chkAttr) {
                return chkAttr;
            }
            auto chkSkin = ZHLN::Test::AssertEq(sizeof(ZHLN::VertexSkin), size_t {12});
            if (!chkSkin) {
                return chkSkin;
            }
            // Meshlet.hpp documents a 64-byte stride for GPUMeshlet.
            auto chkMeshlet = ZHLN::Test::AssertEq(sizeof(ZHLN::GPUMeshlet), size_t {64});
            if (!chkMeshlet) {
                return chkMeshlet;
            }
            return {};
        }

        // The whole "byte-identical between cooked and JIT" claim rests on
        // BuildMeshlets being a pure function of its inputs. Both paths call the
        // same inline function, so determinism here is what makes the claim true.
        std::expected<void, ZHLN::Error> build_is_deterministic() {
            const auto positions = MakeGridPositions();
            const auto indices   = MakeGridIndices();

            const auto a = ZHLN::BuildMeshlets(indices, positions);
            const auto b = ZHLN::BuildMeshlets(indices, positions);

            if (a.Empty()) {
                return std::unexpected(MeshletTestError::StreamInvariantBroken);
            }
            // If this ever drops to 1 the alignment and padding assertions below
            // become vacuous, so fail loudly instead of passing on a technicality.
            auto multi = ZHLN::Test::AssertTrue(a.meshlets.size() > 1);
            if (!multi) {
                return multi;
            }
            if (!StreamsEqual(a, b)) {
                return std::unexpected(MeshletTestError::NotDeterministic);
            }
            return {};
        }

        // Invariants the task/mesh shaders depend on, checked against real output
        // rather than assumed from the meshoptimizer contract.
        std::expected<void, ZHLN::Error> stream_invariants_hold() {
            const auto positions = MakeGridPositions();
            const auto indices   = MakeGridIndices();
            const auto built     = ZHLN::BuildMeshlets(indices, positions);

            if (built.Empty()) {
                return std::unexpected(MeshletTestError::StreamInvariantBroken);
            }

            // Micro-indices are read as uint*, so every offset and the total
            // length must be 4-byte aligned.
            auto chkTail = ZHLN::Test::AssertTrue(built.triangles.size() % 4 == 0);
            if (!chkTail) {
                return chkTail;
            }

            // The stream must actually have been padded somewhere, otherwise the
            // alignment checks are passing on a mesh that never needed them.
            size_t rawTriangleBytes = 0;
            for (const auto& m: built.meshlets) {
                rawTriangleBytes += static_cast<size_t>(m.triangleCount) * 3;
            }
            auto chkPadded = ZHLN::Test::AssertTrue(built.triangles.size() > rawTriangleBytes);
            if (!chkPadded) {
                return chkPadded;
            }

            for (const auto& m: built.meshlets) {
                if (m.triangleOffset % 4 != 0) {
                    return std::unexpected(MeshletTestError::StreamInvariantBroken);
                }
                if (m.vertexCount == 0 || m.triangleCount == 0) {
                    return std::unexpected(MeshletTestError::StreamInvariantBroken);
                }
                if (m.vertexCount > ZHLN::kMeshletMaxVertices || m.triangleCount > ZHLN::kMeshletMaxTriangles) {
                    return std::unexpected(MeshletTestError::StreamInvariantBroken);
                }
                if (static_cast<size_t>(m.vertexOffset) + m.vertexCount > built.vertices.size()) {
                    return std::unexpected(MeshletTestError::StreamInvariantBroken);
                }
                if (static_cast<size_t>(m.triangleOffset) + (static_cast<size_t>(m.triangleCount) * 3) > built.triangles.size()) {
                    return std::unexpected(MeshletTestError::StreamInvariantBroken);
                }
            }

            // Every meshlet vertex index must address the real position pool.
            for (const uint32_t v: built.vertices) {
                if (v >= positions.size()) {
                    return std::unexpected(MeshletTestError::StreamInvariantBroken);
                }
            }
            return {};
        }

        // Both pipelines use the span<VertexPosition> overload. It must agree
        // exactly with the explicit-stride form, otherwise the shared function is
        // not actually shared in behaviour.
        std::expected<void, ZHLN::Error> vertex_position_overload_matches_explicit_stride() {
            const auto positions = MakeGridPositions();
            const auto indices   = MakeGridIndices();

            const auto viaSpan = ZHLN::BuildMeshlets(indices, positions);
            const auto viaRaw  = ZHLN::BuildMeshlets(indices, &positions[0].position[0], positions.size(), sizeof(ZHLN::VertexPosition));

            if (!StreamsEqual(viaSpan, viaRaw)) {
                return std::unexpected(MeshletTestError::StrideForwardingBroken);
            }
            return {};
        }

        // Degenerate input must yield an empty result so callers fall back to the
        // classic indexed draw path instead of building a broken meshlet stream.
        std::expected<void, ZHLN::Error> degenerate_input_yields_empty_result() {
            const auto positions = MakeGridPositions();

            auto tooFewIndices = ZHLN::Test::AssertTrue(ZHLN::BuildMeshlets(std::vector<uint32_t> {0, 1}, positions).Empty());
            if (!tooFewIndices) {
                return tooFewIndices;
            }

            const std::vector<uint32_t> indices = MakeGridIndices();
            auto nullPositions                  = ZHLN::Test::AssertTrue(ZHLN::BuildMeshlets(indices, nullptr, 0, 0).Empty());
            if (!nullPositions) {
                return nullPositions;
            }

            // A stride too narrow to hold a float3 must be rejected rather than
            // reading out of bounds.
            auto narrowStride = ZHLN::Test::AssertTrue(
                ZHLN::BuildMeshlets(indices, &positions[0].position[0], positions.size(), sizeof(float) * 2).Empty()
            );
            if (!narrowStride) {
                return narrowStride;
            }
            return {};
        }
    };
};

// Exported for the assets group binary (RunAssetsTests.cpp), which
// aggregates every suite in this directory through Runner::RunDeferred.
auto RunMeshletsSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<MeshletTestSuite>();
}

