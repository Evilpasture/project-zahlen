// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ShaderProgram.hpp"

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>
#include <vector>

namespace ZHLN::Vk {

enum class ShaderStageCreationError : uint8_t {
    FileOpenFailed ZHLN_ANNOTATION(ZHLN::Description<"Shader file open failed">{}) = 1,
    InvalidSpirvSize ZHLN_ANNOTATION(ZHLN::Description<"Invalid SPIR-V size">{}),
    ShaderLoadingFailed ZHLN_ANNOTATION(ZHLN::Description<"Shader loading failed">{}),
    VertexShaderEmpty ZHLN_ANNOTATION(ZHLN::Description<"Vertex or mesh shader is empty">{}),
};

[[nodiscard]] constexpr auto CreateShaderDesc(const uint32_t* code, size_t size, const char* entry = nullptr) -> ZHLN_ShaderDesc {
    return ZHLN_ShaderDesc {.code = code, .size = size, .entry_point = entry};
}

template <typename T, size_t Extent>
[[nodiscard]] constexpr auto CreateShaderDesc(std::span<T, Extent> codeSpan, const char* entry = nullptr) -> ZHLN_ShaderDesc {
    return ZHLN_ShaderDesc {.code = std::bit_cast<const uint32_t*>(codeSpan.data()), .size = codeSpan.size_bytes(), .entry_point = entry};
}

// A stage source names the module one pipeline stage is built from: where the
// cooked bytes live for a hot reload, the generated fallback that stands in for
// them, and the entry point the module declares. The stage travels in the type
// and is a Vulkan stage flag, so a source cannot name a module compiled for a
// different stage, and task, mesh and ray-tracing stages fit without a table.
template <VkShaderStageFlagBits Stage>
struct ShaderStageSource {
    static constexpr VkShaderStageFlagBits stage = Stage;
    const char*                            path = nullptr;
    std::span<const uint8_t>               fallback {};
    const char*                            entryPoint = nullptr;
};

using VertexStageSource   = ShaderStageSource<VK_SHADER_STAGE_VERTEX_BIT>;
using FragmentStageSource = ShaderStageSource<VK_SHADER_STAGE_FRAGMENT_BIT>;
using ComputeStageSource  = ShaderStageSource<VK_SHADER_STAGE_COMPUTE_BIT>;
using TaskStageSource     = ShaderStageSource<VK_SHADER_STAGE_TASK_BIT_EXT>;
using MeshStageSource     = ShaderStageSource<VK_SHADER_STAGE_MESH_BIT_EXT>;

// The module already knows the stage it was compiled for (ShaderProgram.hpp),
// so naming the module names the stage: one template argument, nothing to keep
// in step, and no assert needed to catch a caller who got it wrong.
template <ShaderProgram Module>
[[nodiscard]] auto MakeStageSource() noexcept -> ShaderStageSource<Module::Stage> {
    return {.path = Module::Path, .fallback = Module::Bytes(), .entryPoint = Module::EntryPoint};
}

// Validated stage metadata and borrowed SPIR-V. Copying this view copies no
// bytecode. The supplied code must remain alive and unchanged through
// reflection and the synchronous vkCreateGraphicsPipelines call. Entry-point
// names live in the view itself; descriptors returned by Vertex/Fragment must
// not outlive it.
class ShaderStagesView {
  public:
    ShaderStagesView() = default;

    [[nodiscard("Shader creation may fail; verify validity before binding")]]
    static auto Create(const ZHLN_ShaderDesc& vert, const ZHLN_ShaderDesc& frag) -> std::expected<ShaderStagesView, ZHLN::ErrorCode>;

    template <ShaderProgram Vert, ShaderProgram Frag>
    [[nodiscard("Shader creation may fail; verify validity before binding")]]
    static auto Create() -> std::expected<ShaderStagesView, ZHLN::ErrorCode> {
        static_assert(StageOf<Vert>() == VK_SHADER_STAGE_VERTEX_BIT, "Create() wants a vertex module first (<ShaderBindings.hpp>)");
        static_assert(StageOf<Frag>() == VK_SHADER_STAGE_FRAGMENT_BIT, "Create() wants a fragment module second (<ShaderBindings.hpp>)");
        return Create(CreateShaderDesc<Vert>(), CreateShaderDesc<Frag>());
    }

    [[nodiscard("Shader creation may fail; verify validity before binding")]]
    static auto CreateMesh(const ZHLN_ShaderDesc& task, const ZHLN_ShaderDesc& mesh, const ZHLN_ShaderDesc& frag)
        -> std::expected<ShaderStagesView, ZHLN::ErrorCode>;

    template <ShaderProgram Task, ShaderProgram Mesh, ShaderProgram Frag>
    [[nodiscard("Shader creation may fail; verify validity before binding")]]
    static auto CreateMesh() -> std::expected<ShaderStagesView, ZHLN::ErrorCode> {
        static_assert(StageOf<Task>() == VK_SHADER_STAGE_TASK_BIT_EXT, "CreateMesh() wants a task module first (<ShaderBindings.hpp>)");
        static_assert(StageOf<Mesh>() == VK_SHADER_STAGE_MESH_BIT_EXT, "CreateMesh() wants a mesh module second (<ShaderBindings.hpp>)");
        static_assert(StageOf<Frag>() == VK_SHADER_STAGE_FRAGMENT_BIT, "CreateMesh() wants a fragment module third (<ShaderBindings.hpp>)");
        return CreateMesh(CreateShaderDesc<Task>(), CreateShaderDesc<Mesh>(), CreateShaderDesc<Frag>());
    }

    [[nodiscard]] constexpr auto Get() const noexcept -> const ZHLN_ShaderStages* {
        return &_raw;
    }
    [[nodiscard]] auto Vertex() const noexcept -> ZHLN_ShaderDesc {
        return {.code = _raw.vert.code, .size = _raw.vert.size, .entry_point = _raw.vert.code ? _raw.vert.entry_point : nullptr};
    }
    [[nodiscard]] auto Fragment() const noexcept -> ZHLN_ShaderDesc {
        return {.code = _raw.frag.code, .size = _raw.frag.size, .entry_point = _raw.frag.code ? _raw.frag.entry_point : nullptr};
    }
    [[nodiscard]] auto IsMeshPipeline() const noexcept -> bool {
        return _raw.mesh.code != nullptr;
    }
    [[nodiscard("Always verify shader stages are valid before pipeline creation")]]
    auto Valid() const noexcept -> bool {
        return _raw.vert.code != nullptr || _raw.mesh.code != nullptr;
    }

  private:
    friend class OwnedShaderStages;
    explicit ShaderStagesView(ZHLN_ShaderStages raw) noexcept: _raw(raw) {
    }

    ZHLN_ShaderStages _raw {};
};

static_assert(std::is_trivially_copyable_v<ShaderStagesView>);

// Disk overrides and alignment copies are owned; generated fallbacks are
// borrowed. Recompute the span after a move, never cache a pointer into storage.
struct ShaderBytecode {
    std::span<const std::byte> fallback {};
    std::vector<uint32_t>     storage {};

    [[nodiscard]] auto Code() const noexcept -> std::span<const std::byte> {
        return storage.empty() ? fallback : std::as_bytes(std::span {storage});
    }
};

// Owns only bytecode and pointer-free stage metadata. A view is materialized
// from the current buffers on demand, so moving this owner never needs to
// repair shader pointers. Borrowed fallbacks must outlive each view's use
// and remain immutable through the synchronous reflection/build calls.
class OwnedShaderStages {
  public:
    OwnedShaderStages(const OwnedShaderStages&) = delete;
    auto operator=(const OwnedShaderStages&) -> OwnedShaderStages& = delete;
    OwnedShaderStages(OwnedShaderStages&&) noexcept = default;
    auto operator=(OwnedShaderStages&&) noexcept -> OwnedShaderStages& = default;

    [[nodiscard("Shader creation may fail; verify validity before binding")]]
    static auto Create(ShaderBytecode vert, ShaderBytecode frag, const char* vertEntry = nullptr, const char* fragEntry = nullptr)
        -> std::expected<OwnedShaderStages, ZHLN::ErrorCode>;

    [[nodiscard("Shader creation may fail; verify validity before binding")]]
    static auto CreateMesh(
        ShaderBytecode task, ShaderBytecode mesh, ShaderBytecode frag,
        const char* taskEntry = nullptr, const char* meshEntry = nullptr, const char* fragEntry = nullptr
    ) -> std::expected<OwnedShaderStages, ZHLN::ErrorCode>;

    // Call only on a live (not moved-from) owner. The result borrows this
    // owner's SPIR-V until reflection/pipeline creation completes.
    [[nodiscard]] auto View() const noexcept -> ShaderStagesView;

  private:
    struct StageMetadata {
        VkShaderStageFlagBits stage {};
        std::array<char, sizeof(ZHLN_Shader::entry_point)> entryPoint {};
        uint32_t viewMask = 0;
    };

    OwnedShaderStages(ShaderBytecode vert, ShaderBytecode frag, ShaderBytecode task, ShaderBytecode mesh, const ShaderStagesView& validated) noexcept;
    [[nodiscard]] static auto MetadataOf(const ZHLN_Shader& shader) noexcept -> StageMetadata;
    [[nodiscard]] static auto MakeStage(const ShaderBytecode& source, const StageMetadata& meta) noexcept -> ZHLN_Shader;

    ShaderBytecode _vert;
    ShaderBytecode _frag;
    ShaderBytecode _task;
    ShaderBytecode _mesh;
    StageMetadata  _vertMeta;
    StageMetadata  _fragMeta;
    StageMetadata  _taskMeta;
    StageMetadata  _meshMeta;
};

}
