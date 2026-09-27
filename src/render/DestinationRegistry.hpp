// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include "Rendering.hpp"
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/FrameResult.hpp>
#include <Zahlen/Render/Handles.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ZHLN {

class PresentationTarget;

enum class DestinationError : uint8_t {
    RegistryFull ZHLN_ANNOTATION(ZHLN::Description<"No room for another window: the destination table is full">{}) = 1,
    NativeSwapchainRequired ZHLN_ANNOTATION(ZHLN::Description<"A second window needs native swapchain presentation">{}),
    DeviceUnavailable ZHLN_ANNOTATION(ZHLN::Description<"There is no device to build a destination presenter on">{}),
    SurfaceUnusable ZHLN_ANNOTATION(ZHLN::Description<"The window's surface is null, or its extent is empty">{}),
    PresentFormatMismatch ZHLN_ANNOTATION(ZHLN::Description<"The window's present format does not match the primary swapchain">{}),
    NoActiveFrame ZHLN_ANNOTATION(ZHLN::Description<"A window attachment was asked for outside BeginFrame/EndFrame">{}),
    SlotRetired ZHLN_ANNOTATION(ZHLN::Description<"The record asked about was retired; the image it named is gone">{}),
};

class DestinationRegistry {
  public:
    class Handle {
      public:
        constexpr Handle() noexcept = default;

        [[nodiscard]] static constexpr auto Make(uint32_t index, uint32_t serial) noexcept -> Handle {
            return Handle {kTag | (static_cast<uint64_t>(serial) << kSerialShift) | (static_cast<uint64_t>(index) & kIndexMask)};
        }

        [[nodiscard]] static constexpr auto FromRaw(uint64_t raw) noexcept -> std::optional<Handle> {
            if ((raw & kTagMask) != kTag) {
                return std::nullopt;
            }
            const Handle handle {raw};
            if (handle.Serial() == 0) {
                return std::nullopt;
            }
            return handle;
        }

        [[nodiscard]] static constexpr auto FromTexture(TextureHandle texture) noexcept -> std::optional<Handle> {
            return FromRaw(static_cast<uint64_t>(texture));
        }

        [[nodiscard]] constexpr auto Index() const noexcept -> uint32_t {
            return static_cast<uint32_t>(_raw & kIndexMask);
        }
        [[nodiscard]] constexpr auto Serial() const noexcept -> uint32_t {
            return static_cast<uint32_t>((_raw & kSerialMask) >> kSerialShift);
        }
        [[nodiscard]] constexpr auto Raw() const noexcept -> uint64_t {
            return _raw;
        }
        [[nodiscard]] constexpr auto AsTexture() const noexcept -> TextureHandle {
            return static_cast<TextureHandle>(_raw);
        }
        [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
            return _raw != 0;
        }

        [[nodiscard]] static constexpr auto WrapSerial(uint64_t counter) noexcept -> uint32_t {
            return static_cast<uint32_t>(counter) & kSerialValueMask;
        }

        friend constexpr auto operator==(const Handle&, const Handle&) noexcept -> bool = default;

      private:
        constexpr explicit Handle(uint64_t raw) noexcept: _raw(raw) {}

        static constexpr uint32_t kIndexBits       = 24;
        static constexpr uint32_t kSerialBits      = 24;
        static constexpr uint32_t kSerialShift     = kIndexBits;
        static constexpr uint64_t kIndexMask       = (1ull << kIndexBits) - 1ull;
        static constexpr uint64_t kSerialMask      = ((1ull << kSerialBits) - 1ull) << kSerialShift;
        static constexpr uint64_t kTag             = 0x5AFE'0000'0000'0000ull;
        static constexpr uint64_t kTagMask         = 0xFFFF'0000'0000'0000ull;
        static constexpr uint64_t kSerialValueMask = (1ull << kSerialBits) - 1ull;

        static_assert((kIndexMask & kSerialMask) == 0, "the index and serial fields must not overlap");
        static_assert((kTag & (kIndexMask | kSerialMask)) == 0, "the tag must not overlap the fields");
        static_assert((kTagMask & (kIndexMask | kSerialMask)) == 0, "the tag mask must cover exactly the tag's bits");

        uint64_t _raw = 0;
    };

    struct Rendered {
        enum class By : uint8_t {
            Scene,
            UI,
            FrameFill,
        };

        By by = By::FrameFill;

        [[nodiscard]] constexpr auto Drawn() const noexcept -> bool {
            return by != By::FrameFill;
        }
    };

    struct Record {
        Handle handle {};

        uint32_t serial        = 0;
        uint32_t bindlessIndex = 0;

        Vk::ImageSlice image {};
        bool presentable = false;

        std::optional<Rendered> content {};

        uint64_t generation = 0;
        Vk::AttachmentLayout trackedLayout = Vk::AttachmentLayout::Undefined;

        const PresentationTarget* target = nullptr;

        [[nodiscard]] auto GetRenderedContent() const noexcept -> FrameOutcome<Rendered>;
    };

    class DestinationRecording {
      public:
        DestinationRecording() noexcept = default;
        ~DestinationRecording() noexcept;
        DestinationRecording(DestinationRecording&& other) noexcept;
        auto operator=(DestinationRecording&& other) noexcept -> DestinationRecording&;
        DestinationRecording(const DestinationRecording&)                    = delete;
        auto operator=(const DestinationRecording&) -> DestinationRecording& = delete;

        auto Open(VkCommandBuffer slot) noexcept -> VkCommandBuffer;
        void Close() noexcept;
        void Discard() noexcept;

        [[nodiscard]] auto Command() const noexcept -> VkCommandBuffer {
            return cmd;
        }
        [[nodiscard]] auto IsOpen() const noexcept -> bool {
            return open;
        }

      private:
        VkCommandBuffer cmd  = VK_NULL_HANDLE;
        bool            open = false;
    };

    struct WindowEntry {
        const PresentationTarget*               target    = nullptr;
        Vk::SwapchainPresenter*                 presenter = nullptr;
        std::unique_ptr<Vk::SwapchainPresenter> ownedPresenter;
        uint32_t imageIndex    = 0;
        bool     imageAcquired = false;
        ZHLN::Array<Handle> recordHandles;
        uint64_t cachedGeneration = 0;

        DestinationRecording recording;

        [[nodiscard]] auto IsPrimary() const noexcept -> bool {
            return ownedPresenter == nullptr;
        }
        [[nodiscard]] auto Presenter() const noexcept -> Vk::SwapchainPresenter& {
            return ownedPresenter != nullptr ? *ownedPresenter : *presenter;
        }
    };

    struct Miss {
        enum class Reason : uint8_t {
            NotAHandle ZHLN_ANNOTATION(ZHLN::Description<"The attachment carries no destination handle"> {}) = 1,
            SlotNeverHeld ZHLN_ANNOTATION(ZHLN::Description<"The handle names a slot this registry has never held"> {}),
            SlotRetired ZHLN_ANNOTATION(ZHLN::Description<"The slot was retired; the destination it named is gone"> {}),
            SlotReVended
                ZHLN_ANNOTATION(ZHLN::Description<"The slot has been vended again since; another destination holds it now"> {}),
            SlotReVendedThisFrame
                ZHLN_ANNOTATION(ZHLN::Description<"The slot was re-vended, and the new incarnation is this frame's destination"> {}),
            StaleGeneration
                ZHLN_ANNOTATION(ZHLN::Description<"The record is still live, but the presentation generation it was built from is gone"> {}),
            NothingVended ZHLN_ANNOTATION(ZHLN::Description<"No destination has been vended this frame"> {}),
        };

        Reason reason = Reason::NotAHandle;

        Handle asked {};

        std::optional<Record> live;

        const PresentationTarget* target = nullptr;

        [[nodiscard]] constexpr auto Adoptable() const noexcept -> bool {
            return reason == Reason::SlotReVendedThisFrame && live.has_value();
        }
    };

    static constexpr size_t kMaxWindows = 8;

    DestinationRegistry() noexcept = default;
    ~DestinationRegistry() noexcept;
    DestinationRegistry(DestinationRegistry&&) noexcept;
    auto operator=(DestinationRegistry&&) noexcept -> DestinationRegistry&;
    DestinationRegistry(const DestinationRegistry&)                    = delete;
    auto operator=(const DestinationRegistry&) -> DestinationRegistry& = delete;


    [[nodiscard]] auto Find(const PresentationTarget& target) noexcept -> WindowEntry*;
    [[nodiscard]] auto Find(const PresentationTarget& target) const noexcept -> const WindowEntry*;
    [[nodiscard]] auto Windows() noexcept -> std::span<WindowEntry>;
    [[nodiscard]] auto Full() const noexcept -> bool;
    auto Attach(WindowEntry entry) noexcept -> WindowEntry*;
    void Detach(const PresentationTarget& target) noexcept;
    void Clear() noexcept;

    [[nodiscard]] auto LiveGeneration(const PresentationTarget& target) noexcept -> uint64_t;


    [[nodiscard]] auto Register(Record record) noexcept -> Handle;

    [[nodiscard]] auto Resolve(const RenderAttachment& attachment) noexcept -> std::expected<Record, Miss>;

    [[nodiscard]] auto Records() noexcept -> std::span<Record>;

    void NoteWritten(const RenderAttachment& attachment, Rendered::By by, Vk::AttachmentLayout layout) noexcept;
    void NoteWritten(const RenderAttachment& attachment, Rendered::By by, VkImageLayout layout) = delete;

    void Retire(const PresentationTarget* owner) noexcept;


    void BeginFrame() noexcept;

    void               SetActive(const PresentationTarget* target) noexcept;
    [[nodiscard]] auto ActiveTarget() const noexcept -> const PresentationTarget*;

    [[nodiscard]] auto ActiveDestination() const noexcept -> const WindowEntry*;

    [[nodiscard]] auto ActiveImageIndex() const noexcept -> uint32_t;

    [[nodiscard]] auto DestinationOf(const Record& record) const noexcept -> const WindowEntry*;

    void CloseRecordings() noexcept;

    [[nodiscard]] auto ActiveRecord() noexcept -> std::expected<Record, Miss>;


    [[nodiscard]] auto UnwrittenWarned() const noexcept -> bool;
    void               NoteUnwrittenWarned() noexcept;

  private:
    const PresentationTarget* activeTarget = nullptr;

    std::vector<WindowEntry> windows;
    std::vector<Record>      records;

    uint64_t nextSerial = 1;

    bool unwrittenWarned = false;
};

static_assert(sizeof(DestinationRegistry::Handle) == sizeof(uint64_t));

}
