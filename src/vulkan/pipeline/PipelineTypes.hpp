// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../core/Handles.hpp"
#include <array>
#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace ZHLN::Vk {

struct RuntimeAttachmentFormats {};

template <Vk::Format DepthFormat, Vk::Format... ColorFormats>
struct AttachmentFormats {
    static constexpr Vk::Format depth_format = DepthFormat;
    static constexpr std::array<Vk::Format, sizeof...(ColorFormats)> color_formats {ColorFormats...};
};

template <typename Formats, Vk::Format... Added>
struct AppendAttachmentColors {
    using type = RuntimeAttachmentFormats;
};

template <Vk::Format Depth, Vk::Format... Colors, Vk::Format... Added>
struct AppendAttachmentColors<AttachmentFormats<Depth, Colors...>, Added...> {
    using type = std::conditional_t<
        ((Added != Vk::Format::Undefined) && ...), AttachmentFormats<Depth, Colors..., Added...>, RuntimeAttachmentFormats
    >;
};

template <typename Formats, Vk::Format Depth>
struct SetAttachmentDepth {
    using type = RuntimeAttachmentFormats;
};

template <Vk::Format OldDepth, Vk::Format... Colors, Vk::Format Depth>
struct SetAttachmentDepth<AttachmentFormats<OldDepth, Colors...>, Depth> {
    using type = std::conditional_t<Depth == Vk::Format::Undefined, RuntimeAttachmentFormats, AttachmentFormats<Depth, Colors...>>;
};

template <typename Formats>
struct WithoutAttachmentDepth {
    using type = RuntimeAttachmentFormats;
};

template <Vk::Format Depth, Vk::Format... Colors>
struct WithoutAttachmentDepth<AttachmentFormats<Depth, Colors...>> {
    using type = AttachmentFormats<Vk::Format::Undefined, Colors...>;
};

template <typename Formats>
struct ClearAttachmentColors {
    using type = RuntimeAttachmentFormats;
};

template <Vk::Format Depth, Vk::Format... Colors>
struct ClearAttachmentColors<AttachmentFormats<Depth, Colors...>> {
    using type = AttachmentFormats<Depth>;
};

template <size_t ColorCount, bool HasDepth, typename Formats>
class PipelineBuilder;

template <size_t ColorCount, bool HasDepth, typename Formats = RuntimeAttachmentFormats>
class TypedPipeline {
  public:
    using FormatSet = Formats;

    TypedPipeline() = default;
    explicit TypedPipeline(Pipeline&& pipeline) noexcept requires std::same_as<Formats, RuntimeAttachmentFormats>:
        handle(std::move(pipeline)) {}

    auto operator=(Pipeline&& pipeline) noexcept -> TypedPipeline& requires std::same_as<Formats, RuntimeAttachmentFormats> {
        handle = std::move(pipeline);
        return *this;
    }

    [[nodiscard]] auto Get() const noexcept -> VkPipeline {
        return handle.Get();
    }
    [[nodiscard]] auto Valid() const noexcept -> bool {
        return handle.Valid();
    }
    explicit operator bool() const noexcept {
        return Valid();
    }
    [[nodiscard]] auto Release() noexcept -> Pipeline {
        return std::move(handle);
    }

  private:
    template <size_t, bool, typename>
    friend class PipelineBuilder;
    struct BuilderToken {};
    explicit TypedPipeline(Pipeline&& pipeline, BuilderToken) noexcept: handle(std::move(pipeline)) {}

    Pipeline handle;
};

} // namespace ZHLN::Vk
