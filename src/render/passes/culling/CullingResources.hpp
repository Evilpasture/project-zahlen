// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Rendering.hpp"

namespace ZHLN {

struct CullingResources {
    Vk::HeapPassBindings   hizHeapBindings;
    Vk::ReflectedLayout    hizDescLayout;
    Vk::DynamicComputePass hizGeneratePass;

    Vk::HeapPassBindings   cullingHeapBindings;
    Vk::ReflectedLayout    cullingLayout;
    Vk::DynamicComputePass cullingPass;

    Vk::HeapPassBindings clusterCullingHeapBindings;
    Vk::ReflectedLayout  clusterCullingDescLayout;
    Vk::FixedComputePass clusterCullingPass;

    Vk::HeapPassBindings clusterBoundsHeapBindings;
    Vk::ReflectedLayout  clusterBoundsDescLayout;
    Vk::FixedComputePass clusterBoundsPass;
};

} // namespace ZHLN
