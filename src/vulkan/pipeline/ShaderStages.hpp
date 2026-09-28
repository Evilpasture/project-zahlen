// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ShaderProgram.hpp"

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <array>
#include <cstdint>
#include <expected>
#include <span>
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

class ShaderStages {
  public:
    ShaderStages() = default;
    ~ShaderStages() = default;
    ShaderStages(const ShaderStages&)                    = delete;
    auto operator=(const ShaderStages&) -> ShaderStages& = delete;
    ShaderStages(ShaderStages&& other) noexcept;
    auto operator=(ShaderStages&& other) noexcept -> ShaderStages&;

    // Copies arbitrary caller-provided SPIR-V so the stage metadata stays valid.
    [[nodiscard("Shader creation may fail; verify validity before binding")]]
    static auto Create(const ZHLN_ShaderDesc& vert, const ZHLN_ShaderDesc& frag) -> std::expected<ShaderStages, ZHLN::ErrorCode>;

    // The caller keeps both byte buffers alive through reflection and pipeline
    // creation. Use this for embedded programs and synchronous material builds.
    [[nodiscard("Shader creation may fail; verify validity before binding")]]
    static auto CreateBorrowed(const ZHLN_ShaderDesc& vert, const ZHLN_ShaderDesc& frag) -> std::expected<ShaderStages, ZHLN::ErrorCode>;

    // Adopt file-loaded SPIR-V without a second copy. Empty vectors mean the
    // corresponding descriptor borrows embedded fallback bytes instead.
    [[nodiscard("Shader creation may fail; verify validity before binding")]]
    static auto CreateLoaded(
        const ZHLN_ShaderDesc& vert, std::vector<uint32_t> vertDisk, const ZHLN_ShaderDesc& frag, std::vector<uint32_t> fragDisk
    ) -> std::expected<ShaderStages, ZHLN::ErrorCode>;

    template <ShaderProgram Vert, ShaderProgram Frag>
    [[nodiscard("Shader creation may fail; verify validity before binding")]]
    static auto Create() -> std::expected<ShaderStages, ZHLN::ErrorCode> {
        static_assert(StageOf<Vert>() == VK_SHADER_STAGE_VERTEX_BIT, "Create() wants a vertex module first (<ShaderBindings.hpp>)");
        static_assert(StageOf<Frag>() == VK_SHADER_STAGE_FRAGMENT_BIT, "Create() wants a fragment module second (<ShaderBindings.hpp>)");
        return CreateBorrowed(CreateShaderDesc<Vert>(), CreateShaderDesc<Frag>());
    }

    [[nodiscard("Shader creation may fail; verify validity before binding")]]
    static auto CreateMesh(const ZHLN_ShaderDesc& task, const ZHLN_ShaderDesc& mesh, const ZHLN_ShaderDesc& frag)
        -> std::expected<ShaderStages, ZHLN::ErrorCode>;

    // As with CreateBorrowed, all three supplied SPIR-V spans must outlive use.
    [[nodiscard("Shader creation may fail; verify validity before binding")]]
    static auto CreateMeshBorrowed(const ZHLN_ShaderDesc& task, const ZHLN_ShaderDesc& mesh, const ZHLN_ShaderDesc& frag)
        -> std::expected<ShaderStages, ZHLN::ErrorCode>;

    template <ShaderProgram Task, ShaderProgram Mesh, ShaderProgram Frag>
    [[nodiscard("Shader creation may fail; verify validity before binding")]]
    static auto CreateMesh() -> std::expected<ShaderStages, ZHLN::ErrorCode> {
        static_assert(StageOf<Task>() == VK_SHADER_STAGE_TASK_BIT_EXT, "CreateMesh() wants a task module first (<ShaderBindings.hpp>)");
        static_assert(StageOf<Mesh>() == VK_SHADER_STAGE_MESH_BIT_EXT, "CreateMesh() wants a mesh module second (<ShaderBindings.hpp>)");
        static_assert(StageOf<Frag>() == VK_SHADER_STAGE_FRAGMENT_BIT, "CreateMesh() wants a fragment module third (<ShaderBindings.hpp>)");
        return CreateMeshBorrowed(CreateShaderDesc<Task>(), CreateShaderDesc<Mesh>(), CreateShaderDesc<Frag>());
    }

    [[nodiscard]] auto Get() const noexcept -> const ZHLN_ShaderStages* {
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
    explicit ShaderStages(const ZHLN_ShaderStages& raw) noexcept: _raw(raw) {
    }

    void RebindOwned() noexcept;

    ZHLN_ShaderStages                    _raw {};
    // Static shaders leave these empty; disk-loaded stages take ownership.
    std::array<std::vector<uint32_t>, 4> _ownedSpv {};
};

[[nodiscard]] constexpr auto AsSpirV(const void* data) -> const uint32_t* {
    return std::bit_cast<const uint32_t*>(data);
}
}
