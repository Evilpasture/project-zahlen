#include "DescriptorLayout.hpp"
#include <print>

namespace ZHLN::Vk {

void ReportBindlessRegistryExceeded(uint32_t bindingID, uint32_t capacity) noexcept {
    std::println(stderr, "[BindlessRegistry<{}>] FATAL: Exceeded capacity of {} slots.",
                 bindingID, capacity);
}

} // namespace ZHLN::Vk
