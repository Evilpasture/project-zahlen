// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/DestinationRegistry.hpp
//
// The registry behind `RenderAttachment`: it vends the handles an attachment
// carries, resolves one back to the image/view/extent a pass binds, and tracks
// the presentation generation and frame-writer of that image.
//
//   * No Vulkan calls and no logging -- acquiring, presenting and clearing stay
//     with the render context; lookups answer with `Miss` instead of a bare
//     nullopt, because only the registry knows which way a lookup failed.
//   * A record holds a `Vk::ImageSlice`, not a `Vk::RenderTarget`: nothing is
//     owned and no layout is assumed -- the recording pass declares it.
//   * Invariants: a record index handed to a caller never shifts (retired slots
//     are reused in place, never erased); a handle is only valid for the
//     incarnation it was minted for, so a stale handle is rejected rather than
//     resolved onto the slot's new occupant.

#pragma once

#include "Rendering.hpp"
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/Description.hpp> // ZHLN_ANNOTATION: a miss says which of the ways it missed
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/FrameResult.hpp> // FrameOutcome: what a record answers in
#include <Zahlen/Types.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ZHLN {

class PresentationTarget;

// Why a window could not become a destination: the failures decided here
// (capacity, presentation mode, device, surface, present format) rather than by
// Vulkan -- RHI-level failures keep travelling as `Vk::PresentationError`
// inside ErrorCode. No zero enumerator (0 means success engine-wide).
enum class DestinationError : uint8_t {
    RegistryFull ZHLN_ANNOTATION(ZHLN::Description<"No room for another window: the destination table is full">{}) = 1,
    NativeSwapchainRequired ZHLN_ANNOTATION(ZHLN::Description<"A second window needs native swapchain presentation">{}),
    DeviceUnavailable ZHLN_ANNOTATION(ZHLN::Description<"There is no device to build a destination presenter on">{}),
    SurfaceUnusable ZHLN_ANNOTATION(ZHLN::Description<"The window's surface is null, or its extent is empty">{}),
    PresentFormatMismatch ZHLN_ANNOTATION(ZHLN::Description<"The window's present format does not match the primary swapchain">{}),
    NoActiveFrame ZHLN_ANNOTATION(ZHLN::Description<"A window attachment was asked for outside BeginFrame/EndFrame">{}),
    SlotRetired ZHLN_ANNOTATION(ZHLN::Description<"The record asked about was retired; the image it named is gone">{}),
};

// One window's presentation resources, or one render texture. The registry owns
// the table; the images belong to the swapchain or texture heap, and the primary
// window's presenter belongs to the render context.
class DestinationRegistry {
  public:
    // The handle this registry mints for a record; callers hold it as
    // `RenderAttachment::texture`.
    //
    // Layout: [tag:16][serial:24][index:24]. The index resolves the handle to a
    // record (it is not the mint counter -- recycled slots broke that), the
    // serial makes a handle vended for a since-retired record resolve to a
    // rejection instead of the slot's new occupant, and the tag keeps the space
    // disjoint from the hashed asset ids TextureHandle also carries. The layout
    // is private: `Make`/`FromRaw` are the ways in, `Index`/`Serial` the ways out.
    class Handle {
      public:
        // The retired-slot marker, not a handle: the registry never mints serial
        // 0, which is what makes a stale handle distinguishable from a live one
        // naming slot 0. `FromRaw` refuses it.
        constexpr Handle() noexcept = default;

        [[nodiscard]] static constexpr auto Make(uint32_t index, uint32_t serial) noexcept -> Handle {
            return Handle {kTag | (static_cast<uint64_t>(serial) << kSerialShift) | (static_cast<uint64_t>(index) & kIndexMask)};
        }

        // The one way back from a raw 64-bit value: nullopt for anything this
        // registry did not mint, and for a retired slot's zeroed serial.
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

        // The same, from the public type a handle travels as.
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

        // The serial field's modulus, so an overflowing counter wraps inside the
        // field instead of bleeding into the index.
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

    // What the frame put into a destination's image: the receipt a read-back asks
    // for, in one value.
    struct Rendered {
        // Who wrote it. The frame's fallback fill counts as a writer, because a
        // filled image holds the background colour while one nothing touched holds
        // whatever the driver left in it -- reading the latter back would be
        // undefined memory.
        enum class By : uint8_t {
            Scene,     // the deferred scene pass
            UI,        // the UI pass; the last writer when the scene ran too
            FrameFill, // the frame's own clear, recorded as it closes a destination no pass wrote
        };

        By by = By::FrameFill;

        // A pass drew this image, not the fill alone -- what a reader that wants
        // the frame (a capture, a metric) asks before reading pixels.
        [[nodiscard]] constexpr auto Drawn() const noexcept -> bool {
            return by != By::FrameFill;
        }
    };

    // One resolvable destination: the image a pass binds plus the identity that
    // decides whether the handle naming it is still the right one.
    struct Record {
        Handle handle {};

        // Incarnation stamped into `handle`; 0 while the slot is retired.
        uint32_t serial        = 0;
        uint32_t bindlessIndex = 0; // globalTextures[] slot; 0 = not sampleable

        // The image itself, as a slice: every consumer (graph, pass, capture,
        // presenter) wants image+view+extent+format together, and nothing is owned
        // -- swapchain images and render textures live elsewhere.
        Vk::ImageSlice image {};
        // Swapchain-backed: the presenter transitions it to PRESENT_SRC_KHR.
        bool presentable = false;

        // What the frame put into this image: engaged once anything wrote it,
        // empty while nothing has -- the one state in which its contents are
        // undefined. See `GetRenderedContent`.
        std::optional<Rendered> content {};

        uint64_t generation = 0;
        // Layout the last writer left the image in, in the vocabulary a pass may
        // speak (see Vk::AttachmentLayout). Undefined on a swapchain image's first
        // touch this frame means contents are don't-care.
        Vk::AttachmentLayout trackedLayout = Vk::AttachmentLayout::Undefined;

        // Non-owning key of the presentation target that owns the swapchain image,
        // if any. The interface, not the concrete window: the registry only ever
        // compares the pointer and asks it for an extent.
        const PresentationTarget* target = nullptr;

        // What this frame put into the image, as `FrameOutcome`:
        //
        //   * std::unexpected -- the slot was retired or re-vended since the
        //     handle naming it was minted, so the record holds no image;
        //   * std::nullopt -- nothing touched it this frame: contents undefined;
        //   * Rendered -- something wrote it; `by` says what, Drawn() whether it
        //     was a pass.
        //
        // Defined out of line with the rest of the registry's decisions.
        [[nodiscard]] auto GetRenderedContent() const noexcept -> FrameOutcome<Rendered>;
    };

    // One destination's command stream for the current frame, as a scope. This is
    // what stands in for a "current command buffer": a pass is aimed at a target,
    // the target resolves to a destination, and the destination's recording is what
    // the pass records into.
    //
    // Whoever opened the stream owns closing it, because a buffer left recording
    // outlives its frame: the presentation path ends it after the present
    // transition, the frame's guard ends it on an early return, the destructor ends
    // it if nothing else did, and `Discard` forgets one whose pool is already gone.
    // Unlike `Vk::CommandBufferGuard` it is frame state, opened at most once per
    // frame however many passes ask for it.
    class DestinationRecording {
      public:
        DestinationRecording() noexcept = default;
        // Ends an open recording. Last resort: Close is what the frame boundary
        // calls, so this only fires when a caller skipped it.
        ~DestinationRecording() noexcept;
        DestinationRecording(DestinationRecording&& other) noexcept;
        auto operator=(DestinationRecording&& other) noexcept -> DestinationRecording&;
        DestinationRecording(const DestinationRecording&)                    = delete;
        auto operator=(const DestinationRecording&) -> DestinationRecording& = delete;

        // Opens `slot` for recording, once; returns the stream either way, so a
        // caller need not know whether it is first this frame.
        auto Open(VkCommandBuffer slot) noexcept -> VkCommandBuffer;
        // Ends the recording, if one is open.
        void Close() noexcept;
        // Forgets the buffer without ending it: its pool is gone, and ending a
        // buffer from a destroyed pool is worse than forgetting it. Also how the
        // presentation path retires a stream the presenter ended and submitted.
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

    // One window's presentation resources. The target pointer is a non-owning key; the
    // primary window's presenter is the render context's, so it is borrowed.
    struct WindowEntry {
        const PresentationTarget*               target    = nullptr;
        Vk::SwapchainPresenter*                 presenter = nullptr;
        std::unique_ptr<Vk::SwapchainPresenter> ownedPresenter;
        uint32_t imageIndex    = 0;
        bool     imageAcquired = false;
        // The handle this window's image was vended as, per swapchain image; blank
        // for one not vended this generation. Handles rather than indices so
        // `Valid()` is the whole test and no index arithmetic is needed.
        ZHLN::Array<Handle> recordHandles;
        // The presentation generation those records were built against. A rebuild
        // (resize, suboptimal, out-of-date) hands out new VkImages, so a record
        // cached across one addresses destroyed memory -- GPU-side, with no CPU
        // symptom until the driver walks a dead VkImageView.
        uint64_t cachedGeneration = 0;

        // This destination's stream for the current frame: opened at image
        // acquisition, ended by the present or by the frame's guard.
        DestinationRecording recording;

        [[nodiscard]] auto IsPrimary() const noexcept -> bool {
            return ownedPresenter == nullptr;
        }
        [[nodiscard]] auto Presenter() const noexcept -> Vk::SwapchainPresenter& {
            return ownedPresenter != nullptr ? *ownedPresenter : *presenter;
        }
    };

    // Why a lookup did not answer, and what the registry knows about the slot the
    // handle named -- a bare nullopt would leave the caller re-deriving which of
    // seven situations it hit, and only the registry can tell them apart.
    struct Miss {
        // The ways a handle can fail to name a live record. Starts at 1 like every
        // error enum in the engine (0 means success).
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

        // The handle decoded out of the attachment; the retired marker when there
        // was none.
        Handle asked {};

        // The record holding `asked`'s slot now, when one does. Only
        // `SlotReVendedThisFrame` may be drawn into (Adoptable()), but the occupant
        // is reported either way.
        std::optional<Record> live;

        // For `StaleGeneration`, the target whose rebuild invalidated the record:
        // the handle is right, the images are not.
        const PresentationTarget* target = nullptr;

        // True for the one recoverable miss: the caller holds an attachment a
        // previous generation vended and this frame already re-vended that slot, so
        // `live` is the destination it meant. Every other miss stays a skip --
        // drawing into an image the caller never asked for would be a different lie.
        [[nodiscard]] constexpr auto Adoptable() const noexcept -> bool {
            return reason == Reason::SlotReVendedThisFrame && live.has_value();
        }
    };

    // Bound so the registry stays a fixed, obviously-sized table; the cost is
    // per-window sync objects (one frame in flight), not per-window memory.
    static constexpr size_t kMaxWindows = 8;

    DestinationRegistry() noexcept = default;
    ~DestinationRegistry() noexcept;
    DestinationRegistry(DestinationRegistry&&) noexcept;
    auto operator=(DestinationRegistry&&) noexcept -> DestinationRegistry&;
    DestinationRegistry(const DestinationRegistry&)                    = delete;
    auto operator=(const DestinationRegistry&) -> DestinationRegistry& = delete;

    // --- Window table

    [[nodiscard]] auto Find(const PresentationTarget& target) noexcept -> WindowEntry*;
    // The same lookup for readers: the frame's stream is read from a destination
    // without changing anything about it.
    [[nodiscard]] auto Find(const PresentationTarget& target) const noexcept -> const WindowEntry*;
    [[nodiscard]] auto Windows() noexcept -> std::span<WindowEntry>;
    [[nodiscard]] auto Full() const noexcept -> bool;
    // Appends an entry and returns it; nullptr when the table is full. The
    // registry does not log on the caller's behalf -- which presenter a destination
    // borrows or owns is a line for the code that built it. The pointer is the
    // table's, so a later Attach may move it.
    auto Attach(WindowEntry entry) noexcept -> WindowEntry*;
    // Drops an entry without touching its records; the caller retires those,
    // because retiring needs to know why the window went away.
    void Detach(const PresentationTarget& target) noexcept;
    void Clear() noexcept;

    // Presentation generation a window's destination is on, or 0 when it has none.
    // Used to reject stale attachments instead of binding them.
    [[nodiscard]] auto LiveGeneration(const PresentationTarget& target) noexcept -> uint64_t;

    // --- Records

    // Registers a record and mints its handle. A free retired slot is reused, so
    // an index already handed to a caller never shifts under it.
    [[nodiscard]] auto Register(Record record) noexcept -> Handle;

    // The record a handle names, or the reason it names none. By value on purpose:
    // Register can grow the vector, so a pointer handed out here could dangle.
    [[nodiscard]] auto Resolve(const RenderAttachment& attachment) noexcept -> std::expected<Record, Miss>;

    // Mutable access for the two owners of a record's per-frame state: image
    // acquisition and the unwritten-destination fill. Everything else resolves.
    [[nodiscard]] auto Records() noexcept -> std::span<Record>;

    // Marks the subresource written by this frame's command stream and moves its
    // tracked layout forward. The raw-VkImageLayout overload is deleted: the one
    // layout a pass must not claim is PRESENT_SRC, which only the presenter
    // establishes. Last writer wins, because that is what a reader sees on top.
    void NoteWritten(const RenderAttachment& attachment, Rendered::By by, Vk::AttachmentLayout layout) noexcept;
    void NoteWritten(const RenderAttachment& attachment, Rendered::By by, VkImageLayout layout) = delete;

    // Drops a window's cached records: the swapchain was rebuilt, or the window
    // went away. Slots are retired in place, not erased (see Register).
    void Retire(const PresentationTarget* owner) noexcept;

    // --- The frame's active destination

    // Starts a frame: no destination active, no window marked as rendered into.
    void BeginFrame() noexcept;

    // The window whose swapchain the frame renders into (depth target and the
    // scene's presentation decision); null until a window attachment is vended.
    void               SetActive(const PresentationTarget* target) noexcept;
    [[nodiscard]] auto ActiveTarget() const noexcept -> const PresentationTarget*;

    // The destination the frame is drawing into, or nullptr. The frame's stream is
    // this destination's: a pass aimed at a render texture records here, because a
    // texture has no submission of its own.
    [[nodiscard]] auto ActiveDestination() const noexcept -> const WindowEntry*;

    // The image index the active destination acquired, 0 when none is active.
    [[nodiscard]] auto ActiveImageIndex() const noexcept -> uint32_t;

    // The destination a record's commands belong to: the window owning the
    // swapchain image, or the frame's active destination for a windowless record.
    [[nodiscard]] auto DestinationOf(const Record& record) const noexcept -> const WindowEntry*;

    // Ends every recording still open, so a frame that returned early cannot leave
    // a command buffer recording.
    void CloseRecordings() noexcept;

    // The record behind this frame's vended destination, or why there is none. By
    // value for the same reason Resolve is.
    [[nodiscard]] auto ActiveRecord() noexcept -> std::expected<Record, Miss>;

    // --- Unwritten-destination warning

    // Latched, not per-record: a frame that wrote nothing is one line, not one per
    // destination. NoteWritten re-arms it.
    [[nodiscard]] auto UnwrittenWarned() const noexcept -> bool;
    void               NoteUnwrittenWarned() noexcept;

  private:
    const PresentationTarget* activeTarget = nullptr;

    std::vector<WindowEntry> windows;
    std::vector<Record>      records;

    // Mints the serial half of a vended handle; never 0, which is the retired-slot
    // marker.
    uint64_t nextSerial = 1;

    bool unwrittenWarned = false;
};

static_assert(sizeof(DestinationRegistry::Handle) == sizeof(uint64_t));

} // namespace ZHLN
