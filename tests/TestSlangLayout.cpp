// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Types.hpp>
#include <array>
#include <cstddef>
#include <cstring>
#include <engine/graphics/Resources.hpp>
#include <render/SlangTypeLayout.hpp>

namespace {

template <typename T>
[[nodiscard]] auto ReflectAbi() -> std::expected<ZHLN::Vk::SlangTypeLayout, ZHLN::Error> {
    return ZHLN::Vk::ReflectTypeLayout(
        ZHLN::Resource::gpu_abi_comp.data(), ZHLN::Resource::gpu_abi_comp.size(), ZHLN::Reflect::AnnotatedName<T>()
    );
}

} // namespace

struct SlangLayoutTestSuite {
    struct Tests {
        std::expected<void, ZHLN::Error> gpu_uniform_and_light_layouts_match_cpp() {
            auto frame = ReflectAbi<ZHLN::FrameUniforms>();
            if (!frame) {
                return std::unexpected(frame.error());
            }
            auto light = ReflectAbi<ZHLN::Light>();
            if (!light) {
                return std::unexpected(light.error());
            }
            ZHLN::Test::ExpectEq(frame->size, static_cast<uint32_t>(sizeof(ZHLN::FrameUniforms)));
            ZHLN::Test::ExpectEq(light->size, static_cast<uint32_t>(sizeof(ZHLN::Light)));
            ZHLN::Test::ExpectTrue(frame->FieldOffset("viewProj").value_or(1) == 0);
            ZHLN::Test::ExpectTrue(frame->FieldOffset("invProj").has_value());
            ZHLN::Test::ExpectTrue(light->FieldOffset("position").value_or(1) == 0);
            ZHLN::Test::ExpectTrue(light->FieldOffset("positionView").has_value());
            return {};
        }

        std::expected<void, ZHLN::Error> particle_and_emitter_layouts_match_cpp() {
            auto particle = ReflectAbi<ZHLN::Particle>();
            if (!particle) {
                return std::unexpected(particle.error());
            }
            auto particle3d = ReflectAbi<ZHLN::Particle3D>();
            if (!particle3d) {
                return std::unexpected(particle3d.error());
            }
            auto emitter = ReflectAbi<ZHLN::ParticleEmitterParams>();
            if (!emitter) {
                return std::unexpected(emitter.error());
            }
            auto meshEmit = ReflectAbi<ZHLN::MeshParticleEmitterParams>();
            if (!meshEmit) {
                return std::unexpected(meshEmit.error());
            }
            ZHLN::Test::ExpectEq(particle->size, static_cast<uint32_t>(sizeof(ZHLN::Particle)));
            ZHLN::Test::ExpectEq(particle3d->size, static_cast<uint32_t>(sizeof(ZHLN::Particle3D)));
            ZHLN::Test::ExpectEq(emitter->size, static_cast<uint32_t>(sizeof(ZHLN::ParticleEmitterParams)));
            ZHLN::Test::ExpectEq(meshEmit->size, static_cast<uint32_t>(sizeof(ZHLN::MeshParticleEmitterParams)));
            return {};
        }

        std::expected<void, ZHLN::Error> instance_and_cluster_layouts_match_cpp() {
            auto instance = ReflectAbi<ZHLN::InstanceData>();
            if (!instance) {
                return std::unexpected(instance.error());
            }
            auto meshlet = ReflectAbi<ZHLN::GPUMeshlet>();
            if (!meshlet) {
                return std::unexpected(meshlet.error());
            }
            auto bounds = ReflectAbi<ZHLN::ClusterBounds>();
            if (!bounds) {
                return std::unexpected(bounds.error());
            }
            auto volume = ReflectAbi<ZHLN::ClusterVolume>();
            if (!volume) {
                return std::unexpected(volume.error());
            }
            ZHLN::Test::ExpectEq(instance->size, static_cast<uint32_t>(sizeof(ZHLN::InstanceData)));
            ZHLN::Test::ExpectEq(meshlet->size, static_cast<uint32_t>(sizeof(ZHLN::GPUMeshlet)));
            ZHLN::Test::ExpectEq(bounds->size, static_cast<uint32_t>(sizeof(ZHLN::ClusterBounds)));
            ZHLN::Test::ExpectEq(volume->size, static_cast<uint32_t>(sizeof(ZHLN::ClusterVolume)));
            ZHLN::Test::ExpectTrue(instance->FieldOffset("meshletAddress").has_value());
            return {};
        }

        std::expected<void, ZHLN::Error> push_constant_layouts_match_cpp() {
            auto fog = ReflectAbi<ZHLN::VolumetricFogPushConstants>();
            if (!fog) {
                return std::unexpected(fog.error());
            }
            auto light = ReflectAbi<ZHLN::VolumetricLightInjectPushConstants>();
            if (!light) {
                return std::unexpected(light.error());
            }
            auto temp = ReflectAbi<ZHLN::VolumetricTemporalPushConstants>();
            if (!temp) {
                return std::unexpected(temp.error());
            }
            auto obj = ReflectAbi<ZHLN::ObjectConstants>();
            if (!obj) {
                return std::unexpected(obj.error());
            }
            auto ui = ReflectAbi<ZHLN::UIObjectConstants>();
            if (!ui) {
                return std::unexpected(ui.error());
            }
            ZHLN::Test::ExpectEq(fog->size, static_cast<uint32_t>(sizeof(ZHLN::VolumetricFogPushConstants)));
            ZHLN::Test::ExpectEq(light->size, static_cast<uint32_t>(sizeof(ZHLN::VolumetricLightInjectPushConstants)));
            ZHLN::Test::ExpectEq(temp->size, static_cast<uint32_t>(sizeof(ZHLN::VolumetricTemporalPushConstants)));
            ZHLN::Test::ExpectEq(obj->size, static_cast<uint32_t>(sizeof(ZHLN::ObjectConstants)));
            ZHLN::Test::ExpectEq(ui->size, static_cast<uint32_t>(sizeof(ZHLN::UIObjectConstants)));
            return {};
        }

        std::expected<void, ZHLN::Error> heap_push_data_layout_is_present_in_abi_spirv() {
            auto heap = ZHLN::Vk::ReflectHeapPushDataLayout(ZHLN::Resource::gpu_abi_comp.data(), ZHLN::Resource::gpu_abi_comp.size());
            if (!heap) {
                return std::unexpected(heap.error());
            }
            ZHLN::Test::ExpectTrue((heap->frameAddressOffsets[0] % 8) == 0);
            ZHLN::Test::ExpectTrue(heap->frameAddressOffsets[1] >= heap->frameAddressOffsets[0] + 8);
            ZHLN::Test::ExpectTrue(heap->heapIndexOffset >= heap->frameAddressOffsets.back() + 8);
            ZHLN::Test::ExpectTrue(heap->requiredSize >= heap->heapIndexOffset + sizeof(uint32_t));
            return {};
        }

        std::expected<void, ZHLN::Error> reflected_field_writer_honors_offsets() {
            auto layout = ReflectAbi<ZHLN::ObjectConstants>();
            if (!layout) {
                return std::unexpected(layout.error());
            }
            std::array<std::byte, 16> blob {};
            const uint32_t            instanceId   = 7;
            const uint32_t            isShadowPass = 1;
            ZHLN::Test::ExpectTrue(ZHLN::Vk::WriteReflectedField(blob, *layout, "instanceId", instanceId));
            ZHLN::Test::ExpectTrue(ZHLN::Vk::WriteReflectedField(blob, *layout, "isShadowPass", isShadowPass));
            uint32_t readId = 0;
            uint32_t readSh = 0;
            std::memcpy(&readId, blob.data() + layout->FieldOffset("instanceId").value_or(0), sizeof(readId));
            std::memcpy(&readSh, blob.data() + layout->FieldOffset("isShadowPass").value_or(0), sizeof(readSh));
            ZHLN::Test::ExpectEq(readId, instanceId);
            ZHLN::Test::ExpectEq(readSh, isShadowPass);
            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<SlangLayoutTestSuite>();
}
