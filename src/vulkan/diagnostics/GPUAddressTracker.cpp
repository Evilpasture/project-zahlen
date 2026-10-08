// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Rendering.hpp"
#include "GPUAddressTracker.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace ZHLN::Vk {
namespace {

[[nodiscard]] auto RangeEnd(VkDeviceAddress base, VkDeviceSize size, VkDeviceAddress& end) noexcept -> bool {
    if (size == 0 || size > std::numeric_limits<VkDeviceAddress>::max() - base) {
        return false;
    }
    end = base + size;
    return true;
}

void SortByBase(std::vector<GpuAllocationSymbol>& entries) noexcept {
    std::ranges::sort(entries, [](const GpuAllocationSymbol& a, const GpuAllocationSymbol& b) {
        if (a.baseAddress != b.baseAddress) {
            return a.baseAddress < b.baseAddress;
        }
        if (a.size != b.size) {
            return a.size < b.size;
        }
        if (a.objectType != b.objectType) {
            return a.objectType < b.objectType;
        }
        return a.objectHandle < b.objectHandle;
    });
}

} // namespace

void GPUAddressTracker::SetEnabled(bool enabled) noexcept {
    const std::lock_guard lock(_mutex);
    const bool wasEnabled = _enabled.load(std::memory_order_relaxed);
    if (!enabled || !wasEnabled) {
        _entries.clear();
    }
    _enabled.store(enabled, std::memory_order_release);
}

auto GPUAddressTracker::Enabled() const noexcept -> bool {
    return _enabled.load(std::memory_order_acquire);
}

void GPUAddressTracker::OnBindingEvent(
    const VkDeviceAddressBindingCallbackDataEXT& binding,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData
) noexcept {
    if (!_enabled.load(std::memory_order_acquire)) {
        return;
    }

    VkDeviceAddress bindingEnd = 0;
    if (!RangeEnd(binding.baseAddress, binding.size, bindingEnd)) {
        return;
    }

    const std::lock_guard lock(_mutex);
    if (!_enabled.load(std::memory_order_relaxed)) {
        return;
    }

    const uint32_t objectCount = callbackData != nullptr && callbackData->pObjects != nullptr ? callbackData->objectCount : 0;
    if (objectCount == 0) {
        ProcessBindingObjectLocked(binding, VK_OBJECT_TYPE_UNKNOWN, 0, nullptr);
        return;
    }

    for (uint32_t i = 0; i < objectCount; ++i) {
        const VkDebugUtilsObjectNameInfoEXT& object = callbackData->pObjects[i];
        ProcessBindingObjectLocked(binding, object.objectType, object.objectHandle, object.pObjectName);
    }
}

void GPUAddressTracker::SetObjectName(VkObjectType type, uint64_t handle, std::string_view name) noexcept {
    if (handle == 0 || !_enabled.load(std::memory_order_acquire)) {
        return;
    }

    const std::lock_guard lock(_mutex);
    if (!_enabled.load(std::memory_order_relaxed)) {
        return;
    }

    for (GpuAllocationSymbol& entry: _entries) {
        if (entry.objectType == type && entry.objectHandle == handle) {
            entry.name.assign(name);
        }
    }
}

auto GPUAddressTracker::Resolve(VkDeviceAddress address) const noexcept -> std::vector<GpuAllocationSymbol> {
    std::vector<GpuAllocationSymbol> matches;
    if (!_enabled.load(std::memory_order_acquire)) {
        return matches;
    }

    const std::lock_guard lock(_mutex);
    if (!_enabled.load(std::memory_order_relaxed)) {
        return matches;
    }

    // Overlapping address ranges are legal (for example, aliased resources),
    // so retain every containing range instead of stopping at the nearest base.
    const auto stop = std::upper_bound(_entries.begin(), _entries.end(), address, [](VkDeviceAddress value, const GpuAllocationSymbol& entry) {
        return value < entry.baseAddress;
    });
    for (auto it = _entries.begin(); it != stop; ++it) {
        VkDeviceAddress end = 0;
        if (RangeEnd(it->baseAddress, it->size, end) && address < end) {
            matches.push_back(*it);
        }
    }
    return matches;
}

auto GPUAddressTracker::LiveBindingCount() const noexcept -> std::size_t {
    const std::lock_guard lock(_mutex);
    return _entries.size();
}

void GPUAddressTracker::ProcessBindingObjectLocked(
    const VkDeviceAddressBindingCallbackDataEXT& binding,
    VkObjectType type,
    uint64_t handle,
    const char* name
) noexcept {
    const bool objectKnown = type != VK_OBJECT_TYPE_UNKNOWN || handle != 0;
    if (binding.bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT) {
        BindLocked(binding, type, handle, name);
    } else if (binding.bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT) {
        UnbindLocked(binding, type, handle, objectKnown);
    }
}

void GPUAddressTracker::BindLocked(
    const VkDeviceAddressBindingCallbackDataEXT& binding,
    VkObjectType type,
    uint64_t handle,
    const char* name
) noexcept {
    GpuAllocationSymbol symbol {
        .baseAddress      = binding.baseAddress,
        .size             = binding.size,
        .objectHandle     = handle,
        .objectType       = type,
        .flags            = binding.flags,
        .isDriverInternal = (binding.flags & VK_DEVICE_ADDRESS_BINDING_INTERNAL_OBJECT_BIT_EXT) != 0,
        .name             = name != nullptr ? std::string(name) : std::string {},
    };

    const auto existing = std::ranges::find_if(_entries, [&](const GpuAllocationSymbol& entry) {
        return entry.baseAddress == symbol.baseAddress && entry.size == symbol.size && entry.objectType == symbol.objectType &&
               entry.objectHandle == symbol.objectHandle && entry.flags == symbol.flags;
    });
    if (existing != _entries.end()) {
        *existing = std::move(symbol);
    } else {
        _entries.push_back(std::move(symbol));
    }
    SortByBase(_entries);
}

void GPUAddressTracker::UnbindLocked(
    const VkDeviceAddressBindingCallbackDataEXT& binding,
    VkObjectType type,
    uint64_t handle,
    bool objectKnown
) noexcept {
    VkDeviceAddress unbindEnd = 0;
    if (!RangeEnd(binding.baseAddress, binding.size, unbindEnd)) {
        return;
    }

    std::vector<GpuAllocationSymbol> remaining;
    remaining.reserve(_entries.size() + 1);
    for (const GpuAllocationSymbol& entry: _entries) {
        VkDeviceAddress entryEnd = 0;
        if (!RangeEnd(entry.baseAddress, entry.size, entryEnd)) {
            remaining.push_back(entry);
            continue;
        }

        const bool sameObject = !objectKnown || (entry.objectType == type && entry.objectHandle == handle);
        const bool overlaps   = entry.baseAddress < unbindEnd && binding.baseAddress < entryEnd;
        if (!sameObject || !overlaps) {
            remaining.push_back(entry);
            continue;
        }

        // Unbind reports can split a previously reported range. Subtract the
        // unbound portion while preserving any still-live prefix/suffix.
        if (entry.baseAddress < binding.baseAddress) {
            GpuAllocationSymbol prefix = entry;
            prefix.size = binding.baseAddress - entry.baseAddress;
            remaining.push_back(std::move(prefix));
        }
        if (unbindEnd < entryEnd) {
            GpuAllocationSymbol suffix = entry;
            suffix.baseAddress = unbindEnd;
            suffix.size        = entryEnd - unbindEnd;
            remaining.push_back(std::move(suffix));
        }
    }
    _entries = std::move(remaining);
    SortByBase(_entries);
}

} // namespace ZHLN::Vk
