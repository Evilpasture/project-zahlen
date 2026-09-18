// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/DestinationRegistry.hpp
//
// The registry behind `RenderAttachment`: it hands out the handles a caller's
// attachment carries, turns one back into the VkImage/view/extent a pass binds,
// and tracks which presentation generation that image belonged to.
//
// It is a manager, not a table, because three of the four things a record needs
// to be trustworthy are not fields on the record: the window that owns the
// image, the generation its swapchain was on when the record was built, and the
// frame that has written it. All three are decided and rechecked here.
//
// Deliberately not `RenderTarget*`: `Vk::RenderTarget<VkFormat>` is a physical
// bundle -- an owned Image, its ImageView, an extent and the create-info its
// heap descriptor needs. A record here is a *key* into a registry that hands
// back such a bundle, and the two vocabularies have to stay apart. This layer's
// word for one is "destination" (see RenderAttachment in <Zahlen/Types.hpp>).
//
// No Vulkan calls: acquiring, presenting and clearing stay with the render
// context, which owns the device. What lives here is bookkeeping, and the
// invariants below are why it needs to be one object:
//
//   * a record index handed to a caller never shifts, because retired slots are
//     reused in place rather than erased;
//   * a handle is only valid for the incarnation it was minted for, so a record
//     whose slot went to another image is rejected rather than resolved;
//   * a handle survives its record's retirement as a *rejection*, which is what
//     makes a stale attachment a skipped pass instead of a driver complaint
//     about a destroyed image view.

#pragma once

#include "Rendering.hpp"
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Types.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ZHLN {

class Window;

/// One window's worth of presentation resources, or one render texture. The
/// registry owns the *table*; the images belong to the swapchain or the texture
/// heap, and the primary window's session belongs to the render context.
class DestinationRegistry {
  public:
    /// The handle this registry mints for a record. Callers hold it as
    /// `RenderAttachment::texture`, which is the public name for one.
    ///
    /// Layout: [tag:16][serial:24][index:24]. The index is what resolves the
    /// handle back to a record, so it is stored explicitly. An earlier revision
    /// assumed the handle's counter *was* the index, which held only while
    /// records were appended and never reused; the first recycled slot made
    /// every resolve miss. The serial gives each record a distinct identity, so
    /// a handle vended for a record that has since been retired (and whose slot
    /// went to another image) is rejected instead of silently resolving to the
    /// new occupant. The tag keeps that space disjoint from the hashed asset
    /// ids TextureHandle also carries, so a handle minted here can be compared
    /// and logged unambiguously.
    ///
    /// The layout is private, and that is the point: nothing outside this class
    /// shifts, masks or compares a field. `Make` and `FromRaw` are the two ways
    /// in, `Index` and `Serial` the two ways out.
    class Handle {
      public:
        /// The retired-slot marker, not a handle. `FromRaw` refuses it: the
        /// registry never mints serial 0, which is what makes a stale handle
        /// distinguishable from a live one that merely names slot 0.
        constexpr Handle() noexcept = default;

        [[nodiscard]] static constexpr auto Make(uint32_t index, uint32_t serial) noexcept -> Handle {
            return Handle {kTag | (static_cast<uint64_t>(serial) << kSerialShift) | (static_cast<uint64_t>(index) & kIndexMask)};
        }

        /// The one way back from a raw 64-bit value: std::nullopt for anything
        /// this registry did not mint, and for a retired slot's zeroed serial.
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

        /// The same, from the public type a handle travels as.
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

        /// The serial field's own modulus: a counter that outruns it wraps
        /// inside the field instead of bleeding into the index beside it.
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

    /// One resolvable destination: the image a pass binds, plus the identity
    /// that decides whether the handle naming it is still the right one.
    struct Record {
        Handle handle {};

        /// Incarnation stamped into `handle`; 0 while the slot is retired.
        uint32_t serial        = 0;
        uint32_t bindlessIndex = 0; ///< globalTextures[] slot; 0 = not sampleable

        VkImage     image       = VK_NULL_HANDLE;
        VkImageView view        = VK_NULL_HANDLE;
        VkExtent3D  extent {};
        VkFormat    format      = VK_FORMAT_UNDEFINED;
        bool        presentable = false; ///< swapchain-backed: the presenter transitions it to PRESENT_SRC_KHR

        bool writtenThisFrame = false;
        /// The frame's command stream cleared this record's image because no
        /// pass wrote it. The image therefore holds the background colour, not
        /// the frame: anything reading it back (a capture, a test metric) must
        /// say so rather than report a black scene.
        bool backgroundFilled = false;

        uint64_t generation = 0;
        /// Layout the last writer left the image in, in the vocabulary a pass
        /// is allowed to speak (see Vk::AttachmentLayout for what it may not
        /// claim). From Undefined the first touch of a swapchain image this
        /// frame means "contents are don't-care" (a clear or a DONT_CARE load
        /// is legal).
        Vk::AttachmentLayout trackedLayout = Vk::AttachmentLayout::Undefined;

        /// Non-owning key of the window that owns the swapchain image, if any.
        Window* window = nullptr;
    };

    /// One caller-owned window's presentation resources. Window* is a
    /// non-owning key; the primary window's session is the render context's and
    /// is therefore borrowed rather than owned here.
    struct WindowEntry {
        Window*                               window  = nullptr;
        Vk::SwapchainSession*                 session = nullptr;
        std::unique_ptr<Vk::SwapchainSession>  ownedSession;
        uint32_t imageIndex    = 0;
        bool     imageAcquired = false;
        /// Render-target record index + 1 per swapchain image, 0 when the image
        /// has not been vended yet this swapchain generation.
        ZHLN::Array<uint32_t> recordSlots;
        /// The presentation resource generation those records were built
        /// against. A rebuild (resize, suboptimal, out-of-date) hands out new
        /// VkImages and offscreen targets, so a record cached across one
        /// addresses destroyed memory -- on the GPU, with no CPU-side symptom
        /// until the driver walks a dead VkImageView.
        uint64_t cachedGeneration = 0;

        /// Command buffer opened when the destination's image was vended and
        /// still in the recording state; closed and submitted by EndFrame.
        VkCommandBuffer openCmd     = VK_NULL_HANDLE;
        bool            commandOpen = false;

        [[nodiscard]] auto IsPrimary() const noexcept -> bool {
            return ownedSession == nullptr;
        }
        [[nodiscard]] auto Session() const noexcept -> Vk::SwapchainSession& {
            return ownedSession != nullptr ? *ownedSession : *session;
        }
    };

    /// Bound so the registry stays a fixed, obviously-sized table. Presented
    /// windows are waited one frame in flight, so the cost is per-window sync
    /// objects rather than per-window memory.
    static constexpr size_t kMaxWindows = 8;

    DestinationRegistry() noexcept = default;
    ~DestinationRegistry() noexcept;
    DestinationRegistry(DestinationRegistry&&) noexcept;
    auto operator=(DestinationRegistry&&) noexcept -> DestinationRegistry&;
    DestinationRegistry(const DestinationRegistry&)                    = delete;
    auto operator=(const DestinationRegistry&) -> DestinationRegistry& = delete;

    // --- Window table -------------------------------------------------------

    [[nodiscard]] auto Find(const Window& window) noexcept -> WindowEntry*;
    [[nodiscard]] auto Windows() noexcept -> std::span<WindowEntry>;
    [[nodiscard]] auto Full() const noexcept -> bool;
    /// Appends an entry and logs which session it will present with.
    void Attach(WindowEntry entry) noexcept;
    /// Drops an entry without touching its records: the caller retires those,
    /// because retiring needs to know *why* the window went away.
    void Detach(const Window& window) noexcept;
    void Clear() noexcept;

    /// Presentation resource generation a window's destination is currently on,
    /// or 0 when the window has no destination (and therefore no record worth
    /// trusting). Used to reject stale attachments instead of binding them.
    [[nodiscard]] auto LiveGeneration(const Window& window) noexcept -> uint64_t;

    // --- Records ------------------------------------------------------------

    /// Registers a record and mints its handle. A retired slot is reused when
    /// one is free, so a record index already handed to a caller (a
    /// RenderAttachment) never shifts under it when another destination is
    /// released.
    [[nodiscard]] auto Register(Record record) noexcept -> Handle;

    /// The record a handle names, or std::nullopt when the handle was not
    /// minted here, names a slot that is out of range, or belongs to an earlier
    /// incarnation of it.
    ///
    /// Returned by value on purpose: Register can grow the vector, so a pointer
    /// handed out here could dangle while the caller is still using it.
    [[nodiscard]] auto Resolve(const RenderAttachment& attachment) noexcept -> std::optional<Record>;

    /// Mutable access for the two callers that own a record's per-frame state:
    /// image acquisition (which arms it for the frame) and the unwritten-
    /// destination fill (which writes it off). Everything else resolves.
    [[nodiscard]] auto Records() noexcept -> std::span<Record>;

    /// Marks the subresource as written by the current frame's command stream
    /// and moves its tracked layout forward.
    ///
    /// The layout parameter is Vk::AttachmentLayout, and a raw VkImageLayout is
    /// deleted rather than accepted: this is where a pass reports what it left
    /// behind, and the one layout it must not be able to report is the present
    /// one, which only the presenter can establish.
    void NoteWritten(const RenderAttachment& attachment, Vk::AttachmentLayout layout) noexcept;
    void NoteWritten(const RenderAttachment& attachment, VkImageLayout layout) = delete;

    /// Drops a window's cached records. Reasons to call it: the swapchain was
    /// rebuilt (new VkImages), or the window went away. Slots are retired in
    /// place rather than erased -- see Register.
    void Retire(const Window* owner) noexcept;

    // --- The frame's active destination -------------------------------------

    /// Starts a frame: no destination is active, and no window is marked as
    /// having been rendered into.
    void BeginFrame() noexcept;

    /// The window whose swapchain the frame is rendering into, used for the
    /// depth target and the scene's presentation decision. Null while no window
    /// attachment has been vended this frame.
    void               SetActive(Window* window) noexcept;
    [[nodiscard]] auto ActiveWindow() const noexcept -> Window*;

    /// The record behind this frame's vended destination, when a destination
    /// was vended and its record is still live. By value for the same reason
    /// Resolve is: registration can grow the registry.
    [[nodiscard]] auto ActiveRecord() noexcept -> std::optional<Record>;

    // --- Unwritten-destination warning --------------------------------------

    /// The warning is latched, not per-record: a frame that wrote nothing is
    /// one line, not one line per destination. NoteWritten re-arms it.
    [[nodiscard]] auto UnwrittenWarned() const noexcept -> bool;
    void               NoteUnwrittenWarned() noexcept;

  private:
    Window* activeWindow = nullptr;

    std::vector<WindowEntry> windows;
    std::vector<Record>      records;

    /// Mints the serial half of a vended handle. Never 0: that value is the
    /// retired-slot marker, so the counter steps over it.
    uint64_t nextSerial = 1;

    bool unwrittenWarned = false;
};

static_assert(sizeof(DestinationRegistry::Handle) == sizeof(uint64_t));

} // namespace ZHLN
