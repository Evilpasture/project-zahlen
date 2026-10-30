// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Render/Meshlet.hpp>
#include <Zahlen/Render/Vertex.hpp>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

enum class MeshletTestError : uint8_t {
    LayoutDrift ZHLN_ANNOTATION(ZHLN::Description<"A cooked vertex record changed stride, so the on-disk stream no longer matches the shader.">{}) = 1,
    NotDeterministic ZHLN_ANNOTATION(ZHLN::Description<"BuildMeshlets produced different output for identical input.">{}),
    StreamInvariantBroken ZHLN_ANNOTATION(ZHLN::Description<"A meshlet stream invariant the mesh shader relies on was violated.">{}),
    StrideForwardingBroken ZHLN_ANNOTATION(ZHLN::Description<"The VertexPosition overload disagreed with the explicit-stride form.">{}),
};

namespace {

// A subdivided grid rather than a bare cube: 32x32 quads spans multiple
// meshlets, so the shader-facing bounds and word alignment are exercised at
// more than one offset. The amount of padding is not a behavioral contract.
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

// Bit-exact comparison of the three logical meshlet streams. MeshletDesc consists
// of 15 contiguous 32-bit fields (60 bytes, zero padding), so memcmp pins both
// exact float bit patterns and cooked .zmesh reproducibility.
bool StreamsEqual(const ZHLN::MeshletBuildResult& a, const ZHLN::MeshletBuildResult& b) {
    if (a != b) {
        return false;
    }
    return std::memcmp(a.meshlets.data(), b.meshlets.data(), a.meshlets.size() * sizeof(ZHLN::MeshletDesc)) == 0 &&
           std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(uint32_t)) == 0 &&
           std::memcmp(a.triangles.data(), b.triangles.data(), a.triangles.size()) == 0;
}

} // namespace

namespace {

struct MeshletTestSuite {
    struct Tests {
        // The cooker serializes these two remaining vertex types as raw
        // records; the other vertex and meshlet layouts already have public
        // header static_asserts, so do not repeat them here.
        std::expected<void, ZHLN::ErrorCode> cooked_vertex_record_strides_match_the_format() {
            if (!ZHLN::Test::ExpectEq(sizeof(ZHLN::VertexPosition), size_t {12}) ||
                !ZHLN::Test::ExpectEq(sizeof(ZHLN::VertexSkin), size_t {12})) {
                return std::unexpected(MeshletTestError::LayoutDrift);
            }
            return {};
        }

        // The whole "byte-identical between cooked and JIT" claim rests on
        // BuildMeshlets being a pure function of its inputs. Both paths call the
        // same inline function, so determinism here is what makes the claim true.
        std::expected<void, ZHLN::ErrorCode> build_is_deterministic() {
            const auto positions = MakeGridPositions();
            const auto indices   = MakeGridIndices();

            const auto a = ZHLN::BuildMeshlets(indices, positions);
            const auto b = ZHLN::BuildMeshlets(indices, positions);

            if (a.Empty()) {
                return std::unexpected(MeshletTestError::StreamInvariantBroken);
            }
            if (!StreamsEqual(a, b)) {
                return std::unexpected(MeshletTestError::NotDeterministic);
            }
            return {};
        }

        // Invariants the task/mesh shaders depend on, checked against real output
        // rather than assumed from the meshoptimizer contract.
        std::expected<void, ZHLN::ErrorCode> stream_invariants_hold() {
            const auto positions = MakeGridPositions();
            const auto indices   = MakeGridIndices();
            const auto built     = ZHLN::BuildMeshlets(indices, positions);

            if (built.Empty()) {
                return std::unexpected(MeshletTestError::StreamInvariantBroken);
            }

            // Micro-indices are read as uint*, so every offset and the total
            // length must be 4-byte aligned.
            if (!ZHLN::Test::ExpectTrue(built.triangles.size() % 4 == 0)) {
                return std::unexpected(MeshletTestError::StreamInvariantBroken);
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
        std::expected<void, ZHLN::ErrorCode> vertex_position_overload_matches_explicit_stride() {
            const auto positions = MakeGridPositions();
            const auto indices   = MakeGridIndices();

            const auto viaSpan = ZHLN::BuildMeshlets(indices, positions);
            const auto viaRaw  = ZHLN::BuildMeshlets(indices, &positions[0].position[0], positions.size(), sizeof(ZHLN::VertexPosition));

            if (!StreamsEqual(viaSpan, viaRaw)) {
                return std::unexpected(MeshletTestError::StrideForwardingBroken);
            }

            const std::vector<ZHLN::VertexPosition> soup = {{{-1.0f, -1.0f, 0.0f}}, {{1.0f, -1.0f, 0.0f}}, {{0.0f, 1.0f, 0.0f}}};
            const std::vector<uint32_t>             seq  = {0, 1, 2};
            if (!ZHLN::Test::ExpectEq(ZHLN::BuildMeshlets(soup), ZHLN::BuildMeshlets(seq, soup))) {
                return std::unexpected(MeshletTestError::StrideForwardingBroken);
            }
            return {};
        }

        // Degenerate input must yield an empty result so callers fall back to the
        // classic indexed draw path instead of building a broken meshlet stream.
        std::expected<void, ZHLN::ErrorCode> degenerate_input_yields_empty_result() {
            const auto positions = MakeGridPositions();

            if (!ZHLN::Test::ExpectTrue(ZHLN::BuildMeshlets(std::span {positions}.first(2)).Empty())) {
                return std::unexpected(MeshletTestError::StreamInvariantBroken);
            }

            if (!ZHLN::Test::ExpectTrue(ZHLN::BuildMeshlets(std::vector<uint32_t> {0, 1}, positions).Empty())) {
                return std::unexpected(MeshletTestError::StreamInvariantBroken);
            }

            const std::vector<uint32_t> indices = MakeGridIndices();
            if (!ZHLN::Test::ExpectTrue(ZHLN::BuildMeshlets(indices, nullptr, 0, 0).Empty())) {
                return std::unexpected(MeshletTestError::StreamInvariantBroken);
            }

            // A stride too narrow to hold a float3 must be rejected rather than
            // reading out of bounds.
            if (!ZHLN::Test::ExpectTrue(
                ZHLN::BuildMeshlets(indices, &positions[0].position[0], positions.size(), sizeof(float) * 2).Empty()
            )) {
                return std::unexpected(MeshletTestError::StrideForwardingBroken);
            }
            return {};
        }
    };
};

} // namespace
