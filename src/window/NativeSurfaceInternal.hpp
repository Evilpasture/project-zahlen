// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "PresentationTarget.hpp"
#include <cstdint>
#include <type_traits>
#include <utility>
#include <variant>

namespace ZHLN {


struct Win32Target {
    void* hwnd      = nullptr;
    void* hinstance = nullptr;
};

struct WaylandTarget {
    void* display = nullptr;
    void* surface = nullptr;
};

struct X11Target {
    void* display = nullptr;
    unsigned long window = 0;
};

struct CocoaTarget {
    void* caMetalLayer = nullptr;
};

struct DrmTarget {
    int      fd          = -1;
    uint32_t connectorId = 0;
    uint32_t crtcId      = 0;
};

struct HeadlessTarget {};

using NativeSurfaceVariant = std::variant<HeadlessTarget, Win32Target, WaylandTarget, X11Target, CocoaTarget, DrmTarget>;

struct NativeSurfaceHandle::Impl {
    NativeSurfaceVariant target;

    template <typename T>
        requires(!std::is_same_v<std::remove_cvref_t<T>, Impl>)
    explicit Impl(T&& t): target(std::forward<T>(t)) {
    }
};

template <class... Ts>
struct Overloaded: Ts... {
    using Ts::operator()...;
};

template <typename Visitor>
decltype(auto) Visit(const NativeSurfaceHandle& handle, Visitor&& visitor) {
    const NativeSurfaceHandle::Impl& impl = *handle._impl;
    return std::visit(std::forward<Visitor>(visitor), impl.target);
}

}
