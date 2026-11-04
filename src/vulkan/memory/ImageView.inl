#pragma once
#include "ImageView.hpp"

namespace ZHLN::Vk {

constexpr auto GetFormatAspect(Vk::Format format) noexcept -> Vk::ImageAspect {
    switch (format) {
        case Vk::Format::Undefined: return Vk::ImageAspect::None;
        case Vk::Format::D16Unorm:
        case Vk::Format::X8D24UnormPack32:
        case Vk::Format::D32Sfloat:
            return Vk::ImageAspect::Depth;
        case Vk::Format::S8Uint:
            return Vk::ImageAspect::Stencil;
        case Vk::Format::D16UnormS8Uint:
        case Vk::Format::D24UnormS8Uint:
        case Vk::Format::D32SfloatS8Uint:
            return Vk::ImageAspect::Depth | Vk::ImageAspect::Stencil;
        default:
            return Vk::ImageAspect::Color;
    }
}

}
