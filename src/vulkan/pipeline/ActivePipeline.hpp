// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <optional>
#include <span>

namespace ZHLN::Vk {

// Typestate for recorded draws: these values have no way to exist unless the
// invariant they carry has already been proven -- at record time, never inside
// the draw. An execute path that takes them instead of raw handles cannot be
// called with the invalid state at all, so there is nothing left to assert and
// nothing left for [[assume(false)]] to turn into undefined behavior. The same
// pattern CommandRecorder and ExecutableCommands already use.

// A pipeline that has been built and validated. There is no way to construct
// one except TryGet(), and no way to modify one afterwards.
class ActivePipeline {
  public:
    [[nodiscard]] static auto TryGet(const Pipeline& pipeline) noexcept -> std::optional<ActivePipeline> {
        if (!pipeline.Valid()) {
            return std::nullopt;
        }
        return ActivePipeline {pipeline.Get()};
    }

    // The variant at `index`, or nothing when the index is out of range or the
    // variant was never built: the bounds proof lives here, not in the draw.
    [[nodiscard]] static auto TryGet(std::span<const Pipeline> variants, uint32_t index) noexcept -> std::optional<ActivePipeline> {
        if (index >= variants.size() || !variants[index].Valid()) {
            return std::nullopt;
        }
        return ActivePipeline {variants[index].Get()};
    }

    [[nodiscard]] auto Get() const noexcept -> VkPipeline {
        return m_pipeline;
    }

    // Implicit, so the value drops straight into the Vulkan C API.
    operator VkPipeline() const noexcept {
        return m_pipeline;
    }

  private:
    explicit ActivePipeline(VkPipeline pipeline) noexcept: m_pipeline(pipeline) {
    }

    VkPipeline m_pipeline;
};

// A heap-index push whose offset is known non-zero. Create() refuses the zero
// offset, so Push() never has to ask whether it should.
class BoundHeapPass {
  public:
    [[nodiscard]] static auto Create(uint32_t indexPushOffset, uint32_t slot) noexcept -> std::optional<BoundHeapPass> {
        if (indexPushOffset == 0) {
            return std::nullopt;
        }
        return BoundHeapPass {indexPushOffset, slot};
    }

    void Push(VkCommandBuffer cmd) const noexcept {
        PushHeapIndex(cmd, m_offset, m_slot);
    }

  private:
    BoundHeapPass(uint32_t offset, uint32_t slot) noexcept: m_offset(offset), m_slot(slot) {
    }

    uint32_t m_offset;
    uint32_t m_slot;
};

static_assert(!std::is_default_constructible_v<ActivePipeline>);
static_assert(!std::is_constructible_v<ActivePipeline, VkPipeline>);
static_assert(!std::is_default_constructible_v<BoundHeapPass>);
static_assert(!std::is_constructible_v<BoundHeapPass, uint32_t, uint32_t>);

}
