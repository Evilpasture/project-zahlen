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
// No Vulkan calls and no logging: acquiring, presenting and clearing stay with
// the render context, which owns the device, and anything worth saying about a
// destination is said by the caller that asked for it. What lives here is
// bookkeeping -- and the facts it is asked for are reported rather than thrown
// away: Resolve and ActiveRecord answer with `Miss`, which names the way a
// lookup failed and carries the record that took the slot, because no caller
// can recover that from a blank `nullopt` without re-deriving what this object
// already knew. The invariants below are why it needs to be one object:
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
#include <Zahlen/Core/Description.hpp> // ZHLN_ANNOTATION: a miss says which of the ways it missed
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

/// Why a window could not become a destination: the vocabulary of the layer that
/// decides which windows may be one.
///
/// It exists because those decisions are this layer's -- the table's capacity,
/// the presentation mode, the device, the window's surface, the format it
/// presents in -- and saying them in the RHI's `Vk::PresentationError` made the
/// presenter look like the one that had refused. What the RHI does report (a
/// surface that could not be created, a presenter that could not be
/// initialized) keeps travelling as that layer's own code inside ErrorCode; this
/// enum is for the failures Vulkan has nothing to say about, because they were
/// decided here.
///
/// No zero enumerator: Error's enum constructor refuses a type whose zero value
/// names a real enumerator, and these travel the same way every other code in
/// the engine does.
enum class DestinationError : uint8_t {
    RegistryFull ZHLN_ANNOTATION(ZHLN::Description<"No room for another window: the destination table is full">{}) = 1,
    NativeSwapchainRequired ZHLN_ANNOTATION(ZHLN::Description<"A second window needs native swapchain presentation">{}),
    DeviceUnavailable ZHLN_ANNOTATION(ZHLN::Description<"There is no device to build a destination presenter on">{}),
    SurfaceUnusable ZHLN_ANNOTATION(ZHLN::Description<"The window's surface is null, or its extent is empty">{}),
    PresentFormatMismatch ZHLN_ANNOTATION(ZHLN::Description<"The window's present format does not match the primary swapchain">{}),
    NoActiveFrame ZHLN_ANNOTATION(ZHLN::Description<"A window attachment was asked for outside BeginFrame/EndFrame">{}),
};

/// One window's worth of presentation resources, or one render texture. The
/// registry owns the *table*; the images belong to the swapchain or the texture
/// heap, and the primary window's presenter belongs to the render context.
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

    /// One destination's command stream for the current frame, as a scope.
    ///
    /// There is no "current command buffer" in the renderer, and this is what
    /// takes its place. A pass is pointed at a target; the target resolves to a
    /// destination; the destination's recording is what the pass records into.
    /// Which stream receives a pass therefore cannot depend on what some earlier
    /// call left behind -- asking for another window's attachment cannot repoint
    /// a pass that is already aimed at this one.
    ///
    /// The begin/end pair is this object's rather than the caller's. A command
    /// buffer left in the recording state outlives the frame it belongs to, and
    /// the only way not to have one is for whoever opened it to own closing it:
    /// the presentation path ends it after recording the present transition (the
    /// one thing that must be in the submitted stream), the frame's own guard
    /// ends it if the frame returns early, and the destructor ends it if nothing
    /// else did. `Discard` is the third case -- the pool the buffer came from is
    /// gone (a rebuilt or released presenter), so there is nothing left to end.
    ///
    /// Not `Vk::CommandBufferGuard`, which begins and ends a buffer inside one
    /// scope: this one is frame state that outlives the call that opened it, it
    /// is opened at most once per frame however many passes ask for it, and it
    /// has an end-of-life the guard has no case for -- a recording whose pool is
    /// gone has to be forgotten, not ended.
    class DestinationRecording {
      public:
        DestinationRecording() noexcept = default;
        /// Ends an open recording. Last resort by design: Close is what the
        /// frame's own boundary calls, and the destructor only ever runs with
        /// something still open when a caller skipped it.
        ~DestinationRecording() noexcept;
        DestinationRecording(DestinationRecording&& other) noexcept;
        auto operator=(DestinationRecording&& other) noexcept -> DestinationRecording&;
        DestinationRecording(const DestinationRecording&)                    = delete;
        auto operator=(const DestinationRecording&) -> DestinationRecording& = delete;

        /// Opens `slot` for recording, once. Returns the stream either way, so
        /// a caller does not have to know whether it is the first this frame.
        auto Open(VkCommandBuffer slot) noexcept -> VkCommandBuffer;
        /// Ends the recording, if one is open.
        void Close() noexcept;
        /// Forgets the buffer without ending it: the pool it came from is gone,
        /// and ending a buffer from a destroyed pool is worse than forgetting
        /// it. Also the way the presentation path retires a stream the presenter
        /// has ended and submitted itself.
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

    /// One window's presentation resources. Window* is a non-owning key; the
    /// primary window's presenter is the render context's and is therefore
    /// borrowed rather than owned here.
    struct WindowEntry {
        Window*                                 window   = nullptr;
        Vk::SwapchainPresenter*                 presenter = nullptr;
        std::unique_ptr<Vk::SwapchainPresenter> ownedPresenter;
        uint32_t imageIndex    = 0;
        bool     imageAcquired = false;
        /// The handle this window's image was vended as, per swapchain image; a
        /// blank Handle for one that has not been vended this generation.
        /// Handles and not indices, so nothing here has to remember that a
        /// record index is otherwise stored plus one: `Valid()` is the test for
        /// "vended", the value is what a caller's attachment carries, and there
        /// is no arithmetic between the two.
        ZHLN::Array<Handle> recordHandles;
        /// The presentation resource generation those records were built
        /// against. A rebuild (resize, suboptimal, out-of-date) hands out new
        /// VkImages and offscreen targets, so a record cached across one
        /// addresses destroyed memory -- on the GPU, with no CPU-side symptom
        /// until the driver walks a dead VkImageView.
        uint64_t cachedGeneration = 0;

        /// This destination's stream for the current frame: opened when the
        /// image was acquired, ended by the present, and ended by the frame's
        /// guard if the frame ends before that. Belongs to the destination, so
        /// nothing else has to hold a pointer to it.
        DestinationRecording recording;

        [[nodiscard]] auto IsPrimary() const noexcept -> bool {
            return ownedPresenter == nullptr;
        }
        [[nodiscard]] auto Presenter() const noexcept -> Vk::SwapchainPresenter& {
            return ownedPresenter != nullptr ? *ownedPresenter : *presenter;
        }
    };

    /// Why a lookup did not answer, and what the registry knows about the slot
    /// the handle named.
    ///
    /// `Resolve` and `ActiveRecord` used to answer with a blank `std::nullopt`
    /// for seven different situations, which left the *caller* re-deriving the
    /// difference: RenderScene re-decoded the attachment's handle and
    /// cross-referenced `ActiveRecord()` to find out whether a miss was this
    /// frame's re-vend of the same slot -- recoverable, draw into the live
    /// record -- or a destination that is simply gone. The registry knows which
    /// of the two it is, and it is the only thing that does. It says so here.
    struct Miss {
        /// The ways a handle can fail to name a live record. Enumerators start
        /// at 1, like every error enum in the engine (see ErrorCode's
        /// static_assert: 0 means success, and a miss is not success).
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

        /// The handle the lookup decoded out of the attachment; the retired
        /// marker (a blank Handle) when there was no handle to decode.
        Handle asked {};

        /// The record holding `asked`'s slot now, when one does. Only
        /// `SlotReVendedThisFrame` may be drawn into -- see Adoptable() -- but
        /// the occupant is reported either way, because "who has it now" is the
        /// next question a reader asks.
        std::optional<Record> live;

        /// The window whose presentation rebuild invalidated the record, for
        /// `StaleGeneration`: the handle is right, the images are not.
        Window* window = nullptr;

        /// True when the miss is recoverable in the one way a frame-rebuild
        /// makes recoverable: the caller holds the attachment a *previous*
        /// generation vended, this frame has already re-vended that slot, and
        /// `live` is therefore the destination the caller means. Any other miss
        /// -- a render texture that has been destroyed, a slot that went to a
        /// different destination -- stays a skip; drawing into an image the
        /// caller never asked for would be a different lie.
        [[nodiscard]] constexpr auto Adoptable() const noexcept -> bool {
            return reason == Reason::SlotReVendedThisFrame && live.has_value();
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
    /// The same lookup, for readers: the frame's stream is read from a
    /// destination without anything being changed about it.
    [[nodiscard]] auto Find(const Window& window) const noexcept -> const WindowEntry*;
    [[nodiscard]] auto Windows() noexcept -> std::span<WindowEntry>;
    [[nodiscard]] auto Full() const noexcept -> bool;
    /// Appends an entry and returns it; nullptr when the table is full, which
    /// the caller has normally already checked with Full().
    ///
    /// Returned rather than void so the caller can say what it created, and
    /// simply discarded by callers with nothing to say: the registry does not
    /// log on anyone's behalf, and which presenter a destination borrows or
    /// owns is a line for the code that just built it. Returning the entry also
    /// saves the Find() a caller would otherwise do to get back what it
    /// attached. The pointer is the table's, so a later Attach may move it --
    /// the same lifetime the entry already had.
    auto Attach(WindowEntry entry) noexcept -> WindowEntry*;
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

    /// The record a handle names, or the reason it names none.
    ///
    /// Returned by value on purpose: Register can grow the vector, so a pointer
    /// handed out here could dangle while the caller is still using it. The
    /// miss is by value for the same reason, and carries the record that holds
    /// the slot now when one does.
    [[nodiscard]] auto Resolve(const RenderAttachment& attachment) noexcept -> std::expected<Record, Miss>;

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

    /// The destination the frame is drawing into, or nullptr when none is
    /// active. The frame's *stream* is this destination's: a pass whose target
    /// has no window of its own -- a render texture -- records here, because a
    /// texture is drawn as part of the frame that draws a window and has no
    /// submission of its own.
    [[nodiscard]] auto ActiveDestination() const noexcept -> const WindowEntry*;

    /// The image index the frame's active destination acquired, 0 when none is
    /// active. The frame's swapchain image, without the frame having to remember
    /// it beside the destination that owns it.
    [[nodiscard]] auto ActiveImageIndex() const noexcept -> uint32_t;

    /// The destination a record's commands belong to: the window that owns the
    /// swapchain image, or, for a record that names no window, the frame's
    /// active destination.
    [[nodiscard]] auto DestinationOf(const Record& record) const noexcept -> const WindowEntry*;

    /// Ends every recording still open. The frame calls this on its way out, so
    /// a frame that returned early cannot leave a command buffer recording.
    void CloseRecordings() noexcept;

    /// The record behind this frame's vended destination, or why there is
    /// none. By value for the same reason Resolve is: registration can grow the
    /// registry.
    [[nodiscard]] auto ActiveRecord() noexcept -> std::expected<Record, Miss>;

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
