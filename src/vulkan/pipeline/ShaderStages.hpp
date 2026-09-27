// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ShaderProgram.hpp"

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace ZHLN::Vk {

enum class ShaderStageCreationError : uint8_t {
    FileOpenFailed ZHLN_ANNOTATION(ZHLN::Description<"Shader file open failed">{}) = 1,
    InvalidSpirvSize ZHLN_ANNOTATION(ZHLN::Description<"Invalid SPIR-V size">{}),
    ShaderLoadingFailed ZHLN_ANNOTATION(ZHLN::Description<"Shader loading failed">{}),
    VertexShaderEmpty ZHLN_ANNOTATION(ZHLN::Description<"Vertex shader is empty">{}),
    ShaderModuleCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Shader module creation failed">{}),
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
    constexpr ShaderStages() = default;
    constexpr ShaderStages(VkDevice device, const ZHLN_ShaderStages raw): _device(device), _raw(raw) {
    }
    constexpr ShaderStages(VkDevice device, const ZHLN_ShaderStages raw, std::vector<uint32_t> vertSpv, std::vector<uint32_t> fragSpv):
        _device(device), _raw(raw), _vertSpv(std::move(vertSpv)), _fragSpv(std::move(fragSpv)) {
    }
    constexpr ShaderStages(
        VkDevice                device,
        const ZHLN_ShaderStages raw,
        std::vector<uint32_t>   vertSpv,
        std::vector<uint32_t>   fragSpv,
        std::vector<uint32_t>   taskSpv,
        std::vector<uint32_t>   meshSpv
    ): _device(device), _raw(raw), _vertSpv(std::move(vertSpv)), _fragSpv(std::move(fragSpv)), _taskSpv(std::move(taskSpv)), _meshSpv(std::move(meshSpv)) {
    }

    ~ShaderStages();
    ShaderStages(const ShaderStages&)                    = delete;
    auto operator=(const ShaderStages&) -> ShaderStages& = delete;
    ShaderStages(ShaderStages&& other) noexcept;
    auto operator=(ShaderStages&& other) noexcept -> ShaderStages&;

    [[nodiscard("Shader creation may fail; verify validity before binding")]]
    static auto Create(VkDevice device, const ZHLN_ShaderDesc& vert, const ZHLN_ShaderDesc& frag) -> std::expected<ShaderStages, ZHLN::ErrorCode>;

    template <ShaderProgram Vert, ShaderProgram Frag>
    [[nodiscard("Shader creation may fail; verify validity before binding")]]
    static auto Create(VkDevice device) -> std::expected<ShaderStages, ZHLN::ErrorCode> {
        static_assert(StageOf<Vert>() == VK_SHADER_STAGE_VERTEX_BIT, "Create() wants a vertex module first (<ShaderBindings.hpp>)");
        static_assert(StageOf<Frag>() == VK_SHADER_STAGE_FRAGMENT_BIT, "Create() wants a fragment module second (<ShaderBindings.hpp>)");
        return Create(device, CreateShaderDesc<Vert>(), CreateShaderDesc<Frag>());
    }

    [[nodiscard("Shader creation may fail; verify validity before binding")]]
    static auto CreateMesh(VkDevice device, const ZHLN_ShaderDesc& task, const ZHLN_ShaderDesc& mesh, const ZHLN_ShaderDesc& frag)
        -> std::expected<ShaderStages, ZHLN::ErrorCode>;

    template <ShaderProgram Task, ShaderProgram Mesh, ShaderProgram Frag>
    [[nodiscard("Shader creation may fail; verify validity before binding")]]
    static auto CreateMesh(VkDevice device) -> std::expected<ShaderStages, ZHLN::ErrorCode> {
        static_assert(StageOf<Task>() == VK_SHADER_STAGE_TASK_BIT_EXT, "CreateMesh() wants a task module first (<ShaderBindings.hpp>)");
        static_assert(StageOf<Mesh>() == VK_SHADER_STAGE_MESH_BIT_EXT, "CreateMesh() wants a mesh module second (<ShaderBindings.hpp>)");
        static_assert(StageOf<Frag>() == VK_SHADER_STAGE_FRAGMENT_BIT, "CreateMesh() wants a fragment module third (<ShaderBindings.hpp>)");
        return CreateMesh(device, CreateShaderDesc<Task>(), CreateShaderDesc<Mesh>(), CreateShaderDesc<Frag>());
    }

    [[nodiscard]] constexpr auto Get() const -> const ZHLN_ShaderStages* {
        return &_raw;
    }
    [[nodiscard]] constexpr auto GetVertSpv() const noexcept -> std::span<const uint32_t> {
        return _vertSpv;
    }
    [[nodiscard]] constexpr auto GetFragSpv() const noexcept -> std::span<const uint32_t> {
        return _fragSpv;
    }
    [[nodiscard]] constexpr auto GetTaskSpv() const noexcept -> std::span<const uint32_t> {
        return _taskSpv;
    }
    [[nodiscard]] constexpr auto GetMeshSpv() const noexcept -> std::span<const uint32_t> {
        return _meshSpv;
    }
    [[nodiscard]] constexpr auto IsMeshPipeline() const noexcept -> bool {
        return _raw.mesh.handle != VK_NULL_HANDLE;
    }
    [[nodiscard("Always verify shader stages are valid before pipeline creation")]]
    constexpr auto Valid() const -> bool {
        return _raw.vert.handle != VK_NULL_HANDLE || _raw.mesh.handle != VK_NULL_HANDLE;
    }

  private:
    VkDevice              _device = VK_NULL_HANDLE;
    ZHLN_ShaderStages     _raw {};
    std::vector<uint32_t> _vertSpv {};
    std::vector<uint32_t> _fragSpv {};
    std::vector<uint32_t> _taskSpv {};
    std::vector<uint32_t> _meshSpv {};
};

[[nodiscard]] constexpr auto AsSpirV(const void* data) -> const uint32_t* {
    return std::bit_cast<const uint32_t*>(data);
}
}
