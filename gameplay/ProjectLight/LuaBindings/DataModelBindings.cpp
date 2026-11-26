// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// ProjectLight's DataModel <-> ReflectLua::Vm adapter.
//
// Zero cross-heap GC co-ownership:
//   * Every Lua userdata payload (`LuaInstanceRef`, `LuaVector3`, `LuaColor3`,
//     `LuaSignalRef`, `LuaConnectionRef`) satisfies `ReflectLua::LuaPodUserdata`
//     (trivially copyable + trivially destructible, no `__gc` destructors).
//   * Live C++ `Instance`s are resolved on each boundary crossing through
//     `DataModel::FindById` (with permanent tombstones guaranteeing destroyed
//     IDs never alias new allocations).
//   * C++ `DataModelSignal::Connection` tokens live on the C++ heap inside
//     `DataModelLuaSession::Impl`; Lua closures live on the Lua heap inside
//     `ReflectLua::Vm`'s listener table and execute through `Vm::InvokeListener`
//     under the VM's instruction and memory budgets.

#include <DataModel/DataModel.hpp>
#include <LuaBindings/DataModelBindings.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <lua/LuaInterop.hpp>
#include <lua/ReflectLua.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ZHLN::ProjectLight {

namespace {

    using ZHLN::ReflectLua::CallFrame;
    using ZHLN::ReflectLua::ListenerID;
    using ZHLN::ReflectLua::MetatableSpec;
    using ZHLN::ReflectLua::MethodEntry;
    using ZHLN::ReflectLua::Vm;

    // ------------------------------------------------------------------------
    // Pure POD userdata payloads (`ReflectLua::LuaPodUserdata`)
    // ------------------------------------------------------------------------

    struct LuaInstanceRef {
        InstanceId id     = 0;
        bool       isRoot = false;
    };

    struct LuaVector3 {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct LuaColor3 {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
    };

    enum class SignalKind : uint8_t {
        ChildAdded,
        ChildRemoved,
        DescendantAdded,
        DescendantRemoved,
        Destroying,
        AncestryChanged,
        PropertyChanged,
        HumanoidStateChanged,
        SoundEnded,
    };

    struct LuaSignalRef {
        InstanceId ownerId     = 0;
        bool       ownerIsRoot = false;
        SignalKind kind        = SignalKind::ChildAdded;
    };

    struct LuaConnectionRef {
        uint64_t connectionId = 0;
    };

    static_assert(ZHLN::ReflectLua::LuaPodUserdata<LuaInstanceRef>);
    static_assert(ZHLN::ReflectLua::LuaPodUserdata<LuaVector3>);
    static_assert(ZHLN::ReflectLua::LuaPodUserdata<LuaColor3>);
    static_assert(ZHLN::ReflectLua::LuaPodUserdata<LuaSignalRef>);
    static_assert(ZHLN::ReflectLua::LuaPodUserdata<LuaConnectionRef>);

    // ------------------------------------------------------------------------
    // C++-owned session & connection state
    // ------------------------------------------------------------------------

    struct ConnectionSlot {
        uint64_t              id         = 0;
        ListenerID            listenerId = ZHLN::ReflectLua::kInvalidListenerID;
        bool                  connected  = true;
        std::function<void()> disconnectCppSignal;
    };

    struct BindingHostState {
        Vm*                         vm        = nullptr;
        DataModel*                  dataModel = nullptr;
        bool                        alive     = true;
        uint64_t                    nextConnId = 1;
        std::string                 lastCallbackError;
        std::vector<ConnectionSlot> connections;
        // Keeps orphan Instances created via `Instance.new("Model")` (before
        // being parented under `DataModel::Root()` or `Destroy()`ed) alive on
        // the C++ heap while preserving zero smart pointers inside Lua userdata.
        std::vector<InstancePtr>    unparentedPool;

        void TrackIfUnparented(const InstancePtr& instance) {
            if (!instance || instance->IsDestroyed() || instance->IsDataModelRoot() || instance->Parent()) {
                return;
            }
            for (const InstancePtr& existing: unparentedPool) {
                if (existing == instance) {
                    return;
                }
            }
            unparentedPool.push_back(instance);
        }

        void PruneUnparentedPool() noexcept {
            std::erase_if(unparentedPool, [](const InstancePtr& item) {
                return !item || item->IsDestroyed() || static_cast<bool>(item->Parent());
            });
        }

        [[nodiscard]] auto FindConnection(uint64_t connId) noexcept -> ConnectionSlot* {
            for (ConnectionSlot& slot: connections) {
                if (slot.id == connId) {
                    return &slot;
                }
            }
            return nullptr;
        }

        void DisconnectSlot(uint64_t connId) noexcept {
            ConnectionSlot* slot = FindConnection(connId);
            if (slot == nullptr || !slot->connected) {
                return;
            }
            slot->connected = false;
            if (slot->disconnectCppSignal) {
                auto fn = std::move(slot->disconnectCppSignal);
                slot->disconnectCppSignal = {};
                fn();
            }
            if (vm != nullptr && slot->listenerId != ZHLN::ReflectLua::kInvalidListenerID) {
                vm->UnregisterListener(slot->listenerId);
            }
            slot->listenerId = ZHLN::ReflectLua::kInvalidListenerID;
        }

        void DisconnectAll() noexcept {
            for (ConnectionSlot& slot: connections) {
                if (!slot.connected) {
                    continue;
                }
                slot.connected = false;
                if (slot.disconnectCppSignal) {
                    auto fn = std::move(slot.disconnectCppSignal);
                    slot.disconnectCppSignal = {};
                    fn();
                }
                if (vm != nullptr && slot.listenerId != ZHLN::ReflectLua::kInvalidListenerID) {
                    vm->UnregisterListener(slot.listenerId);
                }
                slot.listenerId = ZHLN::ReflectLua::kInvalidListenerID;
            }
            connections.clear();
        }
    };

    // Reserve `UINT64_MAX` in the intern table for the local DataModel root when
    // its protocol `Id()` is still `0`; ordinary `InstanceId`s allocate up from 1.
    constexpr uint64_t kRootZeroCacheKey = UINT64_MAX;

    auto CacheKeyFor(const LuaInstanceRef& ref) noexcept -> uint64_t {
        return (ref.isRoot && ref.id == 0u) ? kRootZeroCacheKey : ref.id;
    }

    auto MakeInstanceRef(const Instance& instance) noexcept -> LuaInstanceRef {
        return LuaInstanceRef {
            .id     = instance.Id(),
            .isRoot = instance.IsDataModelRoot(),
        };
    }

    auto ResolveInstance(const BindingHostState* host, const LuaInstanceRef& ref) noexcept -> InstancePtr {
        if (host == nullptr || !host->alive || host->dataModel == nullptr) {
            return {};
        }
        if (ref.isRoot) {
            InstancePtr root = host->dataModel->Root();
            if (root && !root->IsDestroyed()) {
                return root;
            }
            return {};
        }
        if (ref.id == 0u) {
            return {};
        }
        return host->dataModel->FindById(ref.id);
    }

    auto ResolveLiveArgInstance(const CallFrame& frame, int index) noexcept -> InstancePtr {
        const auto* ref = frame.TestUserdata<LuaInstanceRef>(index);
        if (ref == nullptr) {
            return {};
        }
        return ResolveInstance(frame.HostContext<BindingHostState>(), *ref);
    }

    void PushInstance(CallFrame& frame, const InstancePtr& instance) noexcept {
        if (!instance || instance->IsDestroyed()) {
            frame.PushNil();
            return;
        }
        const LuaInstanceRef ref = MakeInstanceRef(*instance);
        frame.PushInternedUserdata(CacheKeyFor(ref), ref);
    }

    template <typename T>
    void PushInstance(CallFrame& frame, const Ref<T>& instance) noexcept {
        PushInstance(frame, StaticRefCast<Instance>(instance));
    }

    void PushInstanceArray(CallFrame& frame, std::span<const InstancePtr> instances) noexcept {
        frame.BeginTable(static_cast<int>(instances.size()), 0);
        int index = 1;
        for (const InstancePtr& instance: instances) {
            PushInstance(frame, instance);
            frame.SetArrayElementTop(index++);
        }
    }

    void PushVector3(CallFrame& frame, float x, float y, float z) noexcept {
        frame.PushUserdata(LuaVector3 {.x = x, .y = y, .z = z});
    }

    void PushVector3(CallFrame& frame, const FVec3& value) noexcept {
        PushVector3(frame, value.x, value.y, value.z);
    }

    void PushColor3(CallFrame& frame, float r, float g, float b) noexcept {
        frame.PushUserdata(LuaColor3 {.r = r, .g = g, .b = b});
    }

    void PushSignal(CallFrame& frame, const Instance& owner, SignalKind kind) noexcept {
        frame.PushUserdata(LuaSignalRef {
            .ownerId     = owner.Id(),
            .ownerIsRoot = owner.IsDataModelRoot(),
            .kind        = kind,
        });
    }

    auto ReadVector3(const CallFrame& frame, int index) noexcept -> std::optional<FVec3> {
        if (const auto* vec = frame.TestUserdata<LuaVector3>(index)) {
            return FVec3 {vec->x, vec->y, vec->z};
        }
        if (!frame.IsTable(index)) {
            return std::nullopt;
        }
        const auto x = frame.ReadTableFloatField(index, "X");
        const auto y = frame.ReadTableFloatField(index, "Y");
        const auto z = frame.ReadTableFloatField(index, "Z");
        if (!x || !y || !z) {
            return std::nullopt;
        }
        return FVec3 {*x, *y, *z};
    }

    auto ReadColor3(const CallFrame& frame, int index) noexcept -> std::optional<FVec3> {
        if (const auto* color = frame.TestUserdata<LuaColor3>(index)) {
            return FVec3 {color->r, color->g, color->b};
        }
        return ReadVector3(frame, index);
    }

    // ------------------------------------------------------------------------
    // Vector3 & Color3 metamethods / constructors
    // ------------------------------------------------------------------------

    auto Vector3Dot(CallFrame& frame) -> int {
        const auto* lhs = frame.TestUserdata<LuaVector3>(1);
        const auto* rhs = frame.TestUserdata<LuaVector3>(2);
        if (lhs == nullptr || rhs == nullptr) {
            frame.PushDouble(0.0);
        } else {
            frame.PushFloat(lhs->x * rhs->x + lhs->y * rhs->y + lhs->z * rhs->z);
        }
        return 1;
    }

    auto Vector3Cross(CallFrame& frame) -> int {
        const auto* lhs = frame.TestUserdata<LuaVector3>(1);
        const auto* rhs = frame.TestUserdata<LuaVector3>(2);
        if (lhs == nullptr || rhs == nullptr) {
            frame.PushNil();
            return 1;
        }
        PushVector3(
            frame,
            lhs->y * rhs->z - lhs->z * rhs->y,
            lhs->z * rhs->x - lhs->x * rhs->z,
            lhs->x * rhs->y - lhs->y * rhs->x
        );
        return 1;
    }

    auto Vector3Lerp(CallFrame& frame) -> int {
        const auto* lhs   = frame.TestUserdata<LuaVector3>(1);
        const auto* rhs   = frame.TestUserdata<LuaVector3>(2);
        const float alpha = frame.ReadFloat(3).value_or(0.0f);
        if (lhs == nullptr || rhs == nullptr) {
            frame.PushNil();
            return 1;
        }
        PushVector3(
            frame,
            lhs->x + (rhs->x - lhs->x) * alpha,
            lhs->y + (rhs->y - lhs->y) * alpha,
            lhs->z + (rhs->z - lhs->z) * alpha
        );
        return 1;
    }

    auto Vector3Index(CallFrame& frame) -> int {
        const auto* value = frame.TestUserdata<LuaVector3>(1);
        const auto  prop  = frame.ReadStringView(2);
        if (value == nullptr || !prop) {
            frame.PushNil();
            return 1;
        }
        const std::string_view key = *prop;
        if (key == "X" || key == "x") {
            frame.PushFloat(value->x);
        } else if (key == "Y" || key == "y") {
            frame.PushFloat(value->y);
        } else if (key == "Z" || key == "z") {
            frame.PushFloat(value->z);
        } else if (key == "Magnitude") {
            frame.PushFloat(std::sqrt(value->x * value->x + value->y * value->y + value->z * value->z));
        } else if (key == "Unit") {
            const float mag = std::sqrt(value->x * value->x + value->y * value->y + value->z * value->z);
            if (mag > 0.0f) {
                PushVector3(frame, value->x / mag, value->y / mag, value->z / mag);
            } else {
                PushVector3(frame, 0.0f, 0.0f, 0.0f);
            }
        } else {
            frame.PushNil();
        }
        return 1;
    }

    auto Vector3New(CallFrame& frame) -> int {
        PushVector3(
            frame,
            frame.ReadFloat(1).value_or(0.0f),
            frame.ReadFloat(2).value_or(0.0f),
            frame.ReadFloat(3).value_or(0.0f)
        );
        return 1;
    }

    auto Vector3Add(CallFrame& frame) -> int {
        const auto* lhs = frame.TestUserdata<LuaVector3>(1);
        const auto* rhs = frame.TestUserdata<LuaVector3>(2);
        if (lhs == nullptr || rhs == nullptr) {
            frame.PushNil();
            return 1;
        }
        PushVector3(frame, lhs->x + rhs->x, lhs->y + rhs->y, lhs->z + rhs->z);
        return 1;
    }

    auto Vector3Sub(CallFrame& frame) -> int {
        const auto* lhs = frame.TestUserdata<LuaVector3>(1);
        const auto* rhs = frame.TestUserdata<LuaVector3>(2);
        if (lhs == nullptr || rhs == nullptr) {
            frame.PushNil();
            return 1;
        }
        PushVector3(frame, lhs->x - rhs->x, lhs->y - rhs->y, lhs->z - rhs->z);
        return 1;
    }

    auto Vector3Unm(CallFrame& frame) -> int {
        const auto* value = frame.TestUserdata<LuaVector3>(1);
        if (value == nullptr) {
            frame.PushNil();
            return 1;
        }
        PushVector3(frame, -value->x, -value->y, -value->z);
        return 1;
    }

    auto Vector3Mul(CallFrame& frame) -> int {
        const auto* lhs = frame.TestUserdata<LuaVector3>(1);
        const auto* rhs = frame.TestUserdata<LuaVector3>(2);
        if (lhs != nullptr) {
            if (const auto scale = frame.ReadFloat(2)) {
                PushVector3(frame, lhs->x * *scale, lhs->y * *scale, lhs->z * *scale);
                return 1;
            }
        }
        if (rhs != nullptr) {
            if (const auto scale = frame.ReadFloat(1)) {
                PushVector3(frame, rhs->x * *scale, rhs->y * *scale, rhs->z * *scale);
                return 1;
            }
        }
        frame.PushNil();
        return 1;
    }

    auto Vector3Div(CallFrame& frame) -> int {
        const auto* lhs     = frame.TestUserdata<LuaVector3>(1);
        const auto  divisor = frame.ReadFloat(2);
        if (lhs == nullptr || !divisor || *divisor == 0.0f) {
            frame.PushNil();
            return 1;
        }
        PushVector3(frame, lhs->x / *divisor, lhs->y / *divisor, lhs->z / *divisor);
        return 1;
    }

    auto Vector3Equal(CallFrame& frame) -> int {
        const auto* lhs = frame.TestUserdata<LuaVector3>(1);
        const auto* rhs = frame.TestUserdata<LuaVector3>(2);
        frame.PushBool(lhs != nullptr && rhs != nullptr && lhs->x == rhs->x && lhs->y == rhs->y && lhs->z == rhs->z);
        return 1;
    }

    auto IgnoreReadOnlyNewIndex(CallFrame& /*frame*/) -> int {
        return 0;
    }

    auto Color3Lerp(CallFrame& frame) -> int {
        const auto* lhs   = frame.TestUserdata<LuaColor3>(1);
        const auto* rhs   = frame.TestUserdata<LuaColor3>(2);
        const float alpha = frame.ReadFloat(3).value_or(0.0f);
        if (lhs == nullptr || rhs == nullptr) {
            frame.PushNil();
            return 1;
        }
        PushColor3(
            frame,
            lhs->r + (rhs->r - lhs->r) * alpha,
            lhs->g + (rhs->g - lhs->g) * alpha,
            lhs->b + (rhs->b - lhs->b) * alpha
        );
        return 1;
    }

    auto Color3Index(CallFrame& frame) -> int {
        const auto* value = frame.TestUserdata<LuaColor3>(1);
        const auto  prop  = frame.ReadStringView(2);
        if (value == nullptr || !prop) {
            frame.PushNil();
            return 1;
        }
        const std::string_view key = *prop;
        if (key == "R" || key == "r") {
            frame.PushFloat(value->r);
        } else if (key == "G" || key == "g") {
            frame.PushFloat(value->g);
        } else if (key == "B" || key == "b") {
            frame.PushFloat(value->b);
        } else {
            frame.PushNil();
        }
        return 1;
    }

    auto Color3New(CallFrame& frame) -> int {
        PushColor3(
            frame,
            frame.ReadFloat(1).value_or(0.0f),
            frame.ReadFloat(2).value_or(0.0f),
            frame.ReadFloat(3).value_or(0.0f)
        );
        return 1;
    }

    auto Color3FromRGB(CallFrame& frame) -> int {
        constexpr float kScale = 1.0f / 255.0f;
        PushColor3(
            frame,
            frame.ReadFloat(1).value_or(0.0f) * kScale,
            frame.ReadFloat(2).value_or(0.0f) * kScale,
            frame.ReadFloat(3).value_or(0.0f) * kScale
        );
        return 1;
    }

    auto Color3Equal(CallFrame& frame) -> int {
        const auto* lhs = frame.TestUserdata<LuaColor3>(1);
        const auto* rhs = frame.TestUserdata<LuaColor3>(2);
        frame.PushBool(lhs != nullptr && rhs != nullptr && lhs->r == rhs->r && lhs->g == rhs->g && lhs->b == rhs->b);
        return 1;
    }

    // ------------------------------------------------------------------------
    // Instance methods & metamethods
    // ------------------------------------------------------------------------

    auto MethodGetService(CallFrame& frame) -> int {
        const InstancePtr self = ResolveLiveArgInstance(frame, 1);
        const auto*       host = frame.HostContext<BindingHostState>();
        const auto        name = frame.ReadStringView(2);
        if (!self || !self->IsDataModelRoot() || host == nullptr || host->dataModel == nullptr || !name) {
            frame.PushNil();
            return 1;
        }
        PushInstance(frame, host->dataModel->GetService(*name));
        return 1;
    }

    auto MethodGetChildren(CallFrame& frame) -> int {
        const InstancePtr self = ResolveLiveArgInstance(frame, 1);
        if (!self) {
            frame.BeginTable(0, 0);
            return 1;
        }
        PushInstanceArray(frame, self->GetChildren());
        return 1;
    }

    auto MethodGetDescendants(CallFrame& frame) -> int {
        const InstancePtr self = ResolveLiveArgInstance(frame, 1);
        if (!self) {
            frame.BeginTable(0, 0);
            return 1;
        }
        PushInstanceArray(frame, self->GetDescendants());
        return 1;
    }

    auto MethodGetAncestors(CallFrame& frame) -> int {
        const InstancePtr self = ResolveLiveArgInstance(frame, 1);
        if (!self) {
            frame.BeginTable(0, 0);
            return 1;
        }
        PushInstanceArray(frame, self->GetAncestors());
        return 1;
    }

    auto MethodFindFirstChild(CallFrame& frame) -> int {
        const InstancePtr self = ResolveLiveArgInstance(frame, 1);
        const auto        name = frame.ReadStringView(2);
        if (!self || !name) {
            frame.PushNil();
            return 1;
        }
        PushInstance(frame, self->FindFirstChild(*name, frame.ToBool(3)));
        return 1;
    }

    auto MethodFindFirstChildOfClass(CallFrame& frame) -> int {
        const InstancePtr self = ResolveLiveArgInstance(frame, 1);
        const auto        name = frame.ReadStringView(2);
        if (!self || !name) {
            frame.PushNil();
            return 1;
        }
        PushInstance(frame, self->FindFirstChildOfClass(*name, frame.ToBool(3)));
        return 1;
    }

    auto MethodFindFirstChildWhichIsA(CallFrame& frame) -> int {
        const InstancePtr self = ResolveLiveArgInstance(frame, 1);
        const auto        name = frame.ReadStringView(2);
        if (!self || !name) {
            frame.PushNil();
            return 1;
        }
        PushInstance(frame, self->FindFirstChildWhichIsA(*name, frame.ToBool(3)));
        return 1;
    }

    auto MethodIsA(CallFrame& frame) -> int {
        const InstancePtr self = ResolveLiveArgInstance(frame, 1);
        const auto        name = frame.ReadStringView(2);
        frame.PushBool(self && name && self->IsA(*name));
        return 1;
    }

    auto MethodIsDescendantOf(CallFrame& frame) -> int {
        const InstancePtr self     = ResolveLiveArgInstance(frame, 1);
        const InstancePtr ancestor = ResolveLiveArgInstance(frame, 2);
        frame.PushBool(self && ancestor && self->IsDescendantOf(ancestor));
        return 1;
    }

    auto MethodGetFullName(CallFrame& frame) -> int {
        const InstancePtr self = ResolveLiveArgInstance(frame, 1);
        if (!self) {
            frame.PushString("");
            return 1;
        }
        frame.PushString(self->GetFullName());
        return 1;
    }

    auto MethodDestroy(CallFrame& frame) -> int {
        if (const InstancePtr self = ResolveLiveArgInstance(frame, 1)) {
            self->Destroy();
        }
        return 0;
    }

    auto MethodPlay(CallFrame& frame) -> int {
        if (const InstancePtr self = ResolveLiveArgInstance(frame, 1); self && self->IsA("Sound")) {
            static_cast<Sound*>(self.GetPtr())->Play();
        }
        return 0;
    }

    auto MethodStop(CallFrame& frame) -> int {
        if (const InstancePtr self = ResolveLiveArgInstance(frame, 1); self && self->IsA("Sound")) {
            static_cast<Sound*>(self.GetPtr())->Stop();
        }
        return 0;
    }

    auto MethodGetPlayers(CallFrame& frame) -> int {
        const InstancePtr self = ResolveLiveArgInstance(frame, 1);
        if (!self || !self->IsA("PlayersService")) {
            frame.BeginTable(0, 0);
            return 1;
        }
        const auto players = static_cast<PlayersService*>(self.GetPtr())->GetPlayers();
        frame.BeginTable(static_cast<int>(players.size()), 0);
        int index = 1;
        for (const auto& player: players) {
            PushInstance(frame, player);
            frame.SetArrayElementTop(index++);
        }
        return 1;
    }

    auto MethodFindPlayerByUserId(CallFrame& frame) -> int {
        const InstancePtr self   = ResolveLiveArgInstance(frame, 1);
        const auto        userId = frame.ReadUInt64(2);
        if (!self || !self->IsA("PlayersService") || !userId) {
            frame.PushNil();
            return 1;
        }
        PushInstance(frame, static_cast<PlayersService*>(self.GetPtr())->FindPlayerByUserId(*userId));
        return 1;
    }

    auto PushMethodFunction(CallFrame& frame, std::string_view name) noexcept -> bool {
        constexpr std::array<MethodEntry, 15> kInstanceMethods = {{
            {"GetService", &MethodGetService},
            {"GetChildren", &MethodGetChildren},
            {"GetDescendants", &MethodGetDescendants},
            {"GetAncestors", &MethodGetAncestors},
            {"FindFirstChild", &MethodFindFirstChild},
            {"FindFirstChildOfClass", &MethodFindFirstChildOfClass},
            {"FindFirstChildWhichIsA", &MethodFindFirstChildWhichIsA},
            {"IsA", &MethodIsA},
            {"IsDescendantOf", &MethodIsDescendantOf},
            {"GetFullName", &MethodGetFullName},
            {"Destroy", &MethodDestroy},
            {"Play", &MethodPlay},
            {"Stop", &MethodStop},
            {"GetPlayers", &MethodGetPlayers},
            {"FindPlayerByUserId", &MethodFindPlayerByUserId},
        }};
        for (const MethodEntry& entry: kInstanceMethods) {
            if (entry.name == name) {
                frame.PushFunction(entry.callback);
                return true;
            }
        }
        return false;
    }

    void PushSignalProperty(CallFrame& frame, const Instance& instance, std::string_view property) noexcept {
        if (property == "ChildAdded") {
            PushSignal(frame, instance, SignalKind::ChildAdded);
        } else if (property == "ChildRemoved") {
            PushSignal(frame, instance, SignalKind::ChildRemoved);
        } else if (property == "DescendantAdded") {
            PushSignal(frame, instance, SignalKind::DescendantAdded);
        } else if (property == "DescendantRemoved") {
            PushSignal(frame, instance, SignalKind::DescendantRemoved);
        } else if (property == "Destroying") {
            PushSignal(frame, instance, SignalKind::Destroying);
        } else if (property == "AncestryChanged") {
            PushSignal(frame, instance, SignalKind::AncestryChanged);
        } else if (property == "PropertyChanged") {
            PushSignal(frame, instance, SignalKind::PropertyChanged);
        } else if (property == "StateChanged" && instance.IsA("Humanoid")) {
            PushSignal(frame, instance, SignalKind::HumanoidStateChanged);
        } else if (property == "Ended" && instance.IsA("Sound")) {
            PushSignal(frame, instance, SignalKind::SoundEnded);
        } else {
            frame.PushNil();
        }
    }

    auto InstanceToString(CallFrame& frame) -> int {
        const InstancePtr self = ResolveLiveArgInstance(frame, 1);
        if (!self) {
            frame.PushString("Instance<expired>");
            return 1;
        }
        frame.PushString(self->GetFullName());
        return 1;
    }

    auto InstanceEqual(CallFrame& frame) -> int {
        const auto* lhs = frame.TestUserdata<LuaInstanceRef>(1);
        const auto* rhs = frame.TestUserdata<LuaInstanceRef>(2);
        if (lhs == nullptr || rhs == nullptr) {
            frame.PushBool(false);
            return 1;
        }
        frame.PushBool(lhs->isRoot == rhs->isRoot && lhs->id == rhs->id);
        return 1;
    }

    auto InstanceIndex(CallFrame& frame) -> int {
        const auto* ref     = frame.TestUserdata<LuaInstanceRef>(1);
        const auto  propOpt = frame.ReadStringView(2);
        if (ref == nullptr || !propOpt) {
            frame.PushNil();
            return 1;
        }
        const std::string_view property = *propOpt;
        const InstancePtr      instance = ResolveInstance(frame.HostContext<BindingHostState>(), *ref);

        if (!instance) {
            if (property == "IsDestroyed") {
                frame.PushBool(true);
                return 1;
            }
            if (property == "InstanceId") {
                frame.PushString(std::to_string(ref->id));
                return 1;
            }
            // Allow harmless querying/destroying of tombstoned handles (`part:Destroy()`,
            // `part:GetChildren()`, `part:IsA(...)`, `part:GetFullName()`).
            if (PushMethodFunction(frame, property)) {
                return 1;
            }
            frame.PushNil();
            return 1;
        }

        if (property == "Name") {
            frame.PushString(instance->Name());
        } else if (property == "ClassName") {
            frame.PushString(instance->ClassName());
        } else if (property == "Parent") {
            PushInstance(frame, instance->Parent());
        } else if (property == "InstanceId") {
            frame.PushString(std::to_string(instance->Id()));
        } else if (property == "IsDestroyed") {
            frame.PushBool(instance->IsDestroyed());
        } else if (
            property == "ChildAdded" || property == "ChildRemoved" || property == "DescendantAdded" ||
            property == "DescendantRemoved" || property == "Destroying" || property == "AncestryChanged" ||
            property == "PropertyChanged" || property == "StateChanged" || property == "Ended"
        ) {
            PushSignalProperty(frame, *instance, property);
        } else if (instance->IsA("BasePart") && property == "Position") {
            PushVector3(frame, static_cast<BasePart*>(instance.GetPtr())->Position());
        } else if (instance->IsA("BasePart") && property == "Rotation") {
            PushVector3(frame, static_cast<BasePart*>(instance.GetPtr())->Rotation());
        } else if (instance->IsA("BasePart") && property == "Size") {
            PushVector3(frame, static_cast<BasePart*>(instance.GetPtr())->Size());
        } else if (instance->IsA("BasePart") && property == "Color") {
            const FVec3& color = static_cast<BasePart*>(instance.GetPtr())->Color();
            PushColor3(frame, color.x, color.y, color.z);
        } else if (instance->IsA("BasePart") && property == "Anchored") {
            frame.PushBool(static_cast<BasePart*>(instance.GetPtr())->Anchored());
        } else if (instance->IsA("BasePart") && property == "CanCollide") {
            frame.PushBool(static_cast<BasePart*>(instance.GetPtr())->CanCollide());
        } else if (instance->IsA("BasePart") && property == "Transparency") {
            frame.PushFloat(static_cast<BasePart*>(instance.GetPtr())->Transparency());
        } else if (instance->IsA("BasePart") && property == "NetworkOwner") {
            PushInstance(frame, static_cast<BasePart*>(instance.GetPtr())->NetworkOwner());
        } else if (instance->IsA("Part") && property == "Shape") {
            frame.PushEnum(static_cast<Part*>(instance.GetPtr())->Shape());
        } else if (instance->IsA("Part") && property == "FrontSurface") {
            frame.PushEnum(static_cast<Part*>(instance.GetPtr())->FrontSurface());
        } else if (instance->IsA("Part") && property == "BackSurface") {
            frame.PushEnum(static_cast<Part*>(instance.GetPtr())->BackSurface());
        } else if (instance->IsA("Part") && property == "TopSurface") {
            frame.PushEnum(static_cast<Part*>(instance.GetPtr())->TopSurface());
        } else if (instance->IsA("Part") && property == "BottomSurface") {
            frame.PushEnum(static_cast<Part*>(instance.GetPtr())->BottomSurface());
        } else if (instance->IsA("Part") && property == "LeftSurface") {
            frame.PushEnum(static_cast<Part*>(instance.GetPtr())->LeftSurface());
        } else if (instance->IsA("Part") && property == "RightSurface") {
            frame.PushEnum(static_cast<Part*>(instance.GetPtr())->RightSurface());
        } else if (instance->IsA("MeshPart") && property == "MeshId") {
            frame.PushString(static_cast<MeshPart*>(instance.GetPtr())->MeshId());
        } else if (instance->IsA("SpawnPoint") && property == "Position") {
            PushVector3(frame, static_cast<SpawnPoint*>(instance.GetPtr())->Position());
        } else if (instance->IsA("SpawnPoint") && property == "Rotation") {
            PushVector3(frame, static_cast<SpawnPoint*>(instance.GetPtr())->Rotation());
        } else if (instance->IsA("Model") && property == "PrimaryPart") {
            PushInstance(frame, static_cast<Model*>(instance.GetPtr())->PrimaryPart());
        } else if (instance->IsA("Humanoid") && property == "WalkSpeed") {
            frame.PushFloat(static_cast<Humanoid*>(instance.GetPtr())->WalkSpeed());
        } else if (instance->IsA("Humanoid") && property == "JumpPower") {
            frame.PushFloat(static_cast<Humanoid*>(instance.GetPtr())->JumpPower());
        } else if (instance->IsA("Humanoid") && property == "Health") {
            frame.PushFloat(static_cast<Humanoid*>(instance.GetPtr())->Health());
        } else if (instance->IsA("Humanoid") && property == "MaxHealth") {
            frame.PushFloat(static_cast<Humanoid*>(instance.GetPtr())->MaxHealth());
        } else if (instance->IsA("Humanoid") && property == "RootPart") {
            PushInstance(frame, static_cast<Humanoid*>(instance.GetPtr())->RootPart());
        } else if (instance->IsA("Humanoid") && property == "State") {
            frame.PushEnum(static_cast<Humanoid*>(instance.GetPtr())->State());
        } else if (instance->IsA("Player") && property == "UserId") {
            frame.PushUInt64(static_cast<Player*>(instance.GetPtr())->UserId());
        } else if (instance->IsA("Player") && property == "Character") {
            PushInstance(frame, static_cast<Player*>(instance.GetPtr())->Character());
        } else if (instance->IsA("PlayersService") && property == "LocalPlayer") {
            PushInstance(frame, static_cast<PlayersService*>(instance.GetPtr())->LocalPlayer());
        } else if (instance->IsA("WorkspaceService") && property == "CurrentCamera") {
            PushInstance(frame, static_cast<WorkspaceService*>(instance.GetPtr())->CurrentCamera());
        } else if (instance->IsA("PhysicsService") && property == "ServerAuthority") {
            frame.PushBool(static_cast<PhysicsService*>(instance.GetPtr())->ServerAuthority());
        } else if (instance->IsA("Sound") && property == "SoundId") {
            frame.PushString(static_cast<Sound*>(instance.GetPtr())->SoundId());
        } else if (instance->IsA("Sound") && property == "Volume") {
            frame.PushFloat(static_cast<Sound*>(instance.GetPtr())->Volume());
        } else if (instance->IsA("Sound") && property == "Loops") {
            frame.PushBool(static_cast<Sound*>(instance.GetPtr())->Loops());
        } else if (instance->IsA("Sound") && property == "Playing") {
            frame.PushBool(static_cast<Sound*>(instance.GetPtr())->Playing());
        } else if (instance->IsA("Decal") && property == "TextureId") {
            frame.PushString(static_cast<Decal*>(instance.GetPtr())->GetTextureID());
        } else if (instance->IsA("Decal") && property == "Color") {
            const FVec3& color = static_cast<Decal*>(instance.GetPtr())->Color();
            PushColor3(frame, color.x, color.y, color.z);
        } else if (instance->IsA("Decal") && property == "Face") {
            frame.PushEnum(static_cast<Decal*>(instance.GetPtr())->Face());
        } else if (instance->IsA("Decal") && property == "WrapMode") {
            frame.PushEnum(static_cast<Decal*>(instance.GetPtr())->WrapMode());
        } else if (instance->IsA("Decal") && property == "Scale") {
            frame.PushFloat(static_cast<Decal*>(instance.GetPtr())->Scale());
        } else if (instance->IsA("Decal") && property == "Transparency") {
            frame.PushFloat(static_cast<Decal*>(instance.GetPtr())->Transparency());
        } else if (instance->IsA("Motor") && property == "Part1") {
            PushInstance(frame, static_cast<Motor*>(instance.GetPtr())->Part1());
        } else if (instance->IsA("Motor") && property == "Part2") {
            PushInstance(frame, static_cast<Motor*>(instance.GetPtr())->Part2());
        } else if (instance->IsA("Motor") && property == "Offset1") {
            PushVector3(frame, static_cast<Motor*>(instance.GetPtr())->Offset1());
        } else if (instance->IsA("Motor") && property == "Offset2") {
            PushVector3(frame, static_cast<Motor*>(instance.GetPtr())->Offset2());
        } else if (instance->IsA("Motor") && property == "CurrentAngle") {
            frame.PushFloat(static_cast<Motor*>(instance.GetPtr())->CurrentAngle());
        } else if (instance->IsA("Motor") && property == "DesiredAngle") {
            frame.PushFloat(static_cast<Motor*>(instance.GetPtr())->DesiredAngle());
        } else if (instance->IsA("Motor") && property == "MaxVelocity") {
            frame.PushFloat(static_cast<Motor*>(instance.GetPtr())->MaxVelocity());
        } else {
            if (PushMethodFunction(frame, property)) {
                return 1;
            }
            if (instance->IsDataModelRoot()) {
                if (const auto* host = frame.HostContext<BindingHostState>(); host != nullptr && host->dataModel != nullptr) {
                    if (const auto service = host->dataModel->GetService(property)) {
                        PushInstance(frame, service);
                        return 1;
                    }
                }
            }
            PushInstance(frame, instance->FindFirstChild(property));
        }
        return 1;
    }

    auto InstanceNewIndex(CallFrame& frame) -> int {
        const InstancePtr instancePtr = ResolveLiveArgInstance(frame, 1);
        const auto        propOpt     = frame.ReadStringView(2);
        if (!instancePtr || !propOpt) {
            return 0;
        }
        const std::string_view property = *propOpt;
        Instance&              instance = *instancePtr;

        if (property == "Name") {
            if (auto name = frame.ReadString(3)) {
                instance.SetName(std::move(*name));
            }
        } else if (property == "Parent") {
            auto* host = frame.HostContext<BindingHostState>();
            if (frame.IsNilOrNone(3)) {
                if (host != nullptr) {
                    host->TrackIfUnparented(instancePtr);
                }
                [[maybe_unused]] const auto result = instance.SetParent({});
                if (host != nullptr) {
                    host->TrackIfUnparented(instancePtr);
                }
            } else if (const InstancePtr parent = ResolveLiveArgInstance(frame, 3)) {
                if (instance.SetParent(parent) && host != nullptr) {
                    host->PruneUnparentedPool();
                }
            }
        } else if (instance.IsA("BasePart") && property == "Position") {
            if (const auto vec = ReadVector3(frame, 3)) {
                static_cast<BasePart&>(instance).SetPosition(*vec);
            }
        } else if (instance.IsA("BasePart") && property == "Rotation") {
            if (const auto vec = ReadVector3(frame, 3)) {
                static_cast<BasePart&>(instance).SetRotation(*vec);
            }
        } else if (instance.IsA("BasePart") && property == "Size") {
            if (const auto vec = ReadVector3(frame, 3)) {
                static_cast<BasePart&>(instance).SetSize(*vec);
            }
        } else if (instance.IsA("BasePart") && property == "Color") {
            if (const auto color = ReadColor3(frame, 3)) {
                static_cast<BasePart&>(instance).SetColor(*color);
            }
        } else if (instance.IsA("BasePart") && property == "Anchored") {
            static_cast<BasePart&>(instance).SetAnchored(frame.ToBool(3));
        } else if (instance.IsA("BasePart") && property == "CanCollide") {
            static_cast<BasePart&>(instance).SetCanCollide(frame.ToBool(3));
        } else if (instance.IsA("BasePart") && property == "Transparency") {
            if (const auto num = frame.ReadFloat(3)) {
                static_cast<BasePart&>(instance).SetTransparency(*num);
            }
        } else if (instance.IsA("BasePart") && property == "NetworkOwner") {
            if (frame.IsNilOrNone(3)) {
                [[maybe_unused]] const auto result = static_cast<BasePart&>(instance).SetNetworkOwner({});
            } else if (const InstancePtr owner = ResolveLiveArgInstance(frame, 3); owner && owner->IsA("Player")) {
                [[maybe_unused]] const auto result = static_cast<BasePart&>(instance).SetNetworkOwner(StaticRefCast<Player>(owner));
            }
        } else if (instance.IsA("Part") && property == "Shape") {
            if (const auto shape = frame.ReadEnum<PartShape>(3)) {
                static_cast<Part&>(instance).SetShape(*shape);
            }
        } else if (instance.IsA("Part") && property == "FrontSurface") {
            if (const auto surface = frame.ReadEnum<PartSurface>(3)) {
                static_cast<Part&>(instance).SetFrontSurface(*surface);
            }
        } else if (instance.IsA("Part") && property == "BackSurface") {
            if (const auto surface = frame.ReadEnum<PartSurface>(3)) {
                static_cast<Part&>(instance).SetBackSurface(*surface);
            }
        } else if (instance.IsA("Part") && property == "TopSurface") {
            if (const auto surface = frame.ReadEnum<PartSurface>(3)) {
                static_cast<Part&>(instance).SetTopSurface(*surface);
            }
        } else if (instance.IsA("Part") && property == "BottomSurface") {
            if (const auto surface = frame.ReadEnum<PartSurface>(3)) {
                static_cast<Part&>(instance).SetBottomSurface(*surface);
            }
        } else if (instance.IsA("Part") && property == "LeftSurface") {
            if (const auto surface = frame.ReadEnum<PartSurface>(3)) {
                static_cast<Part&>(instance).SetLeftSurface(*surface);
            }
        } else if (instance.IsA("Part") && property == "RightSurface") {
            if (const auto surface = frame.ReadEnum<PartSurface>(3)) {
                static_cast<Part&>(instance).SetRightSurface(*surface);
            }
        } else if (instance.IsA("MeshPart") && property == "MeshId") {
            if (auto meshId = frame.ReadString(3)) {
                static_cast<MeshPart&>(instance).SetMeshId(std::move(*meshId));
            }
        } else if (instance.IsA("SpawnPoint") && property == "Position") {
            if (const auto vec = ReadVector3(frame, 3)) {
                static_cast<SpawnPoint&>(instance).SetPosition(*vec);
            }
        } else if (instance.IsA("SpawnPoint") && property == "Rotation") {
            if (const auto vec = ReadVector3(frame, 3)) {
                static_cast<SpawnPoint&>(instance).SetRotation(*vec);
            }
        } else if (instance.IsA("Model") && property == "PrimaryPart") {
            if (frame.IsNilOrNone(3)) {
                [[maybe_unused]] const auto result = static_cast<Model&>(instance).SetPrimaryPart({});
            } else if (const InstancePtr part = ResolveLiveArgInstance(frame, 3); part && part->IsA("BasePart")) {
                [[maybe_unused]] const auto result = static_cast<Model&>(instance).SetPrimaryPart(StaticRefCast<BasePart>(part));
            }
        } else if (instance.IsA("Humanoid") && property == "WalkSpeed") {
            if (const auto num = frame.ReadFloat(3)) {
                static_cast<Humanoid&>(instance).SetWalkSpeed(*num);
            }
        } else if (instance.IsA("Humanoid") && property == "JumpPower") {
            if (const auto num = frame.ReadFloat(3)) {
                static_cast<Humanoid&>(instance).SetJumpPower(*num);
            }
        } else if (instance.IsA("Humanoid") && property == "Health") {
            if (const auto num = frame.ReadFloat(3)) {
                static_cast<Humanoid&>(instance).SetHealth(*num);
            }
        } else if (instance.IsA("Humanoid") && property == "MaxHealth") {
            if (const auto num = frame.ReadFloat(3)) {
                static_cast<Humanoid&>(instance).SetMaxHealth(*num);
            }
        } else if (instance.IsA("Humanoid") && property == "RootPart") {
            if (frame.IsNilOrNone(3)) {
                [[maybe_unused]] const auto result = static_cast<Humanoid&>(instance).SetRootPart({});
            } else if (const InstancePtr part = ResolveLiveArgInstance(frame, 3); part && part->IsA("BasePart")) {
                [[maybe_unused]] const auto result = static_cast<Humanoid&>(instance).SetRootPart(StaticRefCast<BasePart>(part));
            }
        } else if (instance.IsA("Humanoid") && property == "State") {
            if (const auto state = frame.ReadEnum<HumanoidState>(3)) {
                static_cast<Humanoid&>(instance).SetState(*state);
            }
        } else if (instance.IsA("Player") && property == "UserId") {
            if (const auto userId = frame.ReadUInt64(3)) {
                static_cast<Player&>(instance).SetUserId(*userId);
            }
        } else if (instance.IsA("Player") && property == "Character") {
            if (frame.IsNilOrNone(3)) {
                [[maybe_unused]] const auto result = static_cast<Player&>(instance).SetCharacter({});
            } else if (const InstancePtr character = ResolveLiveArgInstance(frame, 3); character && character->IsA("Model")) {
                [[maybe_unused]] const auto result = static_cast<Player&>(instance).SetCharacter(StaticRefCast<Model>(character));
            }
        } else if (instance.IsA("PlayersService") && property == "LocalPlayer") {
            if (frame.IsNilOrNone(3)) {
                [[maybe_unused]] const auto result = static_cast<PlayersService&>(instance).SetLocalPlayer({});
            } else if (const InstancePtr player = ResolveLiveArgInstance(frame, 3); player && player->IsA("Player")) {
                [[maybe_unused]] const auto result = static_cast<PlayersService&>(instance).SetLocalPlayer(StaticRefCast<Player>(player));
            }
        } else if (instance.IsA("WorkspaceService") && property == "CurrentCamera") {
            if (frame.IsNilOrNone(3)) {
                [[maybe_unused]] const auto result = static_cast<WorkspaceService&>(instance).SetCurrentCamera({});
            } else if (const InstancePtr camera = ResolveLiveArgInstance(frame, 3)) {
                [[maybe_unused]] const auto result = static_cast<WorkspaceService&>(instance).SetCurrentCamera(camera);
            }
        } else if (instance.IsA("PhysicsService") && property == "ServerAuthority") {
            static_cast<PhysicsService&>(instance).SetServerAuthority(frame.ToBool(3));
        } else if (instance.IsA("Sound") && property == "SoundId") {
            if (auto soundId = frame.ReadString(3)) {
                static_cast<Sound&>(instance).SetSoundId(std::move(*soundId));
            }
        } else if (instance.IsA("Sound") && property == "Volume") {
            if (const auto num = frame.ReadFloat(3)) {
                static_cast<Sound&>(instance).SetVolume(*num);
            }
        } else if (instance.IsA("Sound") && property == "Loops") {
            static_cast<Sound&>(instance).SetLoops(frame.ToBool(3));
        } else if (instance.IsA("Sound") && property == "Playing") {
            static_cast<Sound&>(instance).SetPlaying(frame.ToBool(3));
        } else if (instance.IsA("Decal") && property == "TextureId") {
            if (auto textureID = frame.ReadString(3)) {
                static_cast<Decal&>(instance).SetTextureID(std::move(*textureID));
            }
        } else if (instance.IsA("Decal") && property == "Color") {
            if (const auto color = ReadColor3(frame, 3)) {
                static_cast<Decal&>(instance).SetColor(*color);
            }
        } else if (instance.IsA("Decal") && property == "Face") {
            if (const auto face = frame.ReadEnum<DecalFace>(3)) {
                static_cast<Decal&>(instance).SetFace(*face);
            }
        } else if (instance.IsA("Decal") && property == "WrapMode") {
            if (const auto wrapMode = frame.ReadEnum<DecalWrapMode>(3)) {
                static_cast<Decal&>(instance).SetWrapMode(*wrapMode);
            }
        } else if (instance.IsA("Decal") && property == "Scale") {
            if (const auto num = frame.ReadFloat(3)) {
                static_cast<Decal&>(instance).SetScale(*num);
            }
        } else if (instance.IsA("Decal") && property == "Transparency") {
            if (const auto num = frame.ReadFloat(3)) {
                static_cast<Decal&>(instance).SetTransparency(*num);
            }
        } else if (instance.IsA("Motor") && property == "Part1") {
            if (frame.IsNilOrNone(3)) {
                [[maybe_unused]] const auto result = static_cast<Motor&>(instance).SetPart1({});
            } else if (const InstancePtr part = ResolveLiveArgInstance(frame, 3); part && part->IsA("BasePart")) {
                [[maybe_unused]] const auto result = static_cast<Motor&>(instance).SetPart1(StaticRefCast<BasePart>(part));
            }
        } else if (instance.IsA("Motor") && property == "Part2") {
            if (frame.IsNilOrNone(3)) {
                [[maybe_unused]] const auto result = static_cast<Motor&>(instance).SetPart2({});
            } else if (const InstancePtr part = ResolveLiveArgInstance(frame, 3); part && part->IsA("BasePart")) {
                [[maybe_unused]] const auto result = static_cast<Motor&>(instance).SetPart2(StaticRefCast<BasePart>(part));
            }
        } else if (instance.IsA("Motor") && property == "Offset1") {
            if (const auto vec = ReadVector3(frame, 3)) {
                static_cast<Motor&>(instance).SetOffset1(*vec);
            }
        } else if (instance.IsA("Motor") && property == "Offset2") {
            if (const auto vec = ReadVector3(frame, 3)) {
                static_cast<Motor&>(instance).SetOffset2(*vec);
            }
        } else if (instance.IsA("Motor") && property == "CurrentAngle") {
            if (const auto num = frame.ReadFloat(3)) {
                static_cast<Motor&>(instance).SetCurrentAngle(*num);
            }
        } else if (instance.IsA("Motor") && property == "DesiredAngle") {
            if (const auto num = frame.ReadFloat(3)) {
                static_cast<Motor&>(instance).SetDesiredAngle(*num);
            }
        } else if (instance.IsA("Motor") && property == "MaxVelocity") {
            if (const auto num = frame.ReadFloat(3)) {
                static_cast<Motor&>(instance).SetMaxVelocity(*num);
            }
        }
        return 0;
    }

    auto InstanceNew(CallFrame& frame) -> int {
        auto*      host         = frame.HostContext<BindingHostState>();
        const auto classNameOpt = frame.ReadStringView(1);
        if (host == nullptr || !host->alive || host->dataModel == nullptr || !classNameOpt) {
            frame.PushNil();
            return 1;
        }
        const std::string_view className = *classNameOpt;
        if (className.empty() || className == "DataModel") {
            frame.PushNil();
            return 1;
        }

        std::string name;
        int         parentIndex = 2;
        if (frame.IsString(2) && frame.IsNilOrNone(3)) {
            auto parsed = frame.ReadString(2);
            if (!parsed) {
                frame.PushNil();
                return 1;
            }
            name        = std::move(*parsed);
            parentIndex = 0;
        } else if (frame.IsString(3)) {
            auto parsed = frame.ReadString(3);
            if (!parsed) {
                frame.PushNil();
                return 1;
            }
            name = std::move(*parsed);
        }

        auto created = host->dataModel->CreateInstance(className, std::move(name));
        if (!created) {
            frame.PushNil();
            return 1;
        }
        InstancePtr instance = std::move(*created);

        if (parentIndex != 0 && !frame.IsNilOrNone(parentIndex)) {
            const InstancePtr parent = ResolveLiveArgInstance(frame, parentIndex);
            if (!parent || !instance->SetParent(parent)) {
                instance->Destroy();
                frame.PushNil();
                return 1;
            }
        } else {
            host->TrackIfUnparented(instance);
        }

        PushInstance(frame, instance);
        return 1;
    }

    // ------------------------------------------------------------------------
    // Signal & Connection dispatch
    // ------------------------------------------------------------------------

    template <typename PushArguments>
    void DispatchSignalCallback(
        const std::weak_ptr<BindingHostState>& weakHost,
        uint64_t                               connId,
        std::string_view                       signalLabel,
        PushArguments&                         pushArguments,
        auto&&... arguments
    ) {
        const std::shared_ptr<BindingHostState> host = weakHost.lock();
        if (!host || !host->alive || host->vm == nullptr || !host->vm->IsValid()) {
            return;
        }
        const ConnectionSlot* slot = host->FindConnection(connId);
        if (slot == nullptr || !slot->connected || slot->listenerId == ZHLN::ReflectLua::kInvalidListenerID) {
            return;
        }
        const ListenerID listenerId = slot->listenerId;
        const auto       res        = host->vm->InvokeListener(
            listenerId,
            [&](CallFrame& cbFrame) {
                pushArguments(cbFrame, std::forward<decltype(arguments)>(arguments)...);
            },
            signalLabel
        );
        if (!res) {
            const std::string_view detail = host->vm->LastErrorDetail();
            host->lastCallbackError = detail.empty() ? "unknown Lua callback error" : std::string(detail);
            ZHLN::Log<ZHLN::LogSeverity::Error>(ZHLN::LogChannel {"Script"}, "Lua callback failed: {}", host->lastCallbackError);
        }
    }

    template <typename Signal, typename PushArguments>
    void ConnectSignalSlot(
        CallFrame&                             frame,
        const std::shared_ptr<BindingHostState>& host,
        Signal&                                signal,
        std::string_view                       signalLabel,
        int                                    callbackIndex,
        PushArguments                          pushArguments
    ) {
        const ListenerID listenerId = frame.RegisterListener(callbackIndex);
        if (listenerId == ZHLN::ReflectLua::kInvalidListenerID) {
            frame.PushNil();
            return;
        }

        const uint64_t connId = host->nextConnId++;
        using Token           = typename Signal::Connection;
        auto token            = std::make_shared<Token>();
        const std::weak_ptr<BindingHostState> weakHost = host;

        *token = signal.Connect([weakHost, connId, signalLabel, pushArguments = std::move(pushArguments)](auto&&... arguments) mutable {
            DispatchSignalCallback(weakHost, connId, signalLabel, pushArguments, std::forward<decltype(arguments)>(arguments)...);
        });

        std::erase_if(host->connections, [](const ConnectionSlot& existing) { return !existing.connected; });
        host->connections.push_back(ConnectionSlot {
            .id                  = connId,
            .listenerId          = listenerId,
            .connected           = true,
            .disconnectCppSignal = [token]() noexcept { token->Disconnect(); },
        });

        frame.PushUserdata(LuaConnectionRef {.connectionId = connId});
    }

    auto SignalConnect(CallFrame& frame) -> int {
        if (!frame.IsFunction(2)) {
            frame.PushNil();
            return 1;
        }
        const auto* signal = frame.TestUserdata<LuaSignalRef>(1);
        auto*       rawHost = frame.HostContext<std::shared_ptr<BindingHostState>>();
        if (signal == nullptr || rawHost == nullptr || !(*rawHost) || !(*rawHost)->alive) {
            frame.PushNil();
            return 1;
        }
        const std::shared_ptr<BindingHostState>& host = *rawHost;
        const InstancePtr owner = ResolveInstance(
            host.get(),
            LuaInstanceRef {.id = signal->ownerId, .isRoot = signal->ownerIsRoot}
        );
        if (!owner) {
            frame.PushNil();
            return 1;
        }

        switch (signal->kind) {
        case SignalKind::ChildAdded:
            ConnectSignalSlot(
                frame, host, owner->ChildAdded, "ChildAdded", 2,
                [](CallFrame& cb, const InstancePtr& child) { PushInstance(cb, child); }
            );
            return 1;
        case SignalKind::ChildRemoved:
            ConnectSignalSlot(
                frame, host, owner->ChildRemoved, "ChildRemoved", 2,
                [](CallFrame& cb, const InstancePtr& child) { PushInstance(cb, child); }
            );
            return 1;
        case SignalKind::DescendantAdded:
            ConnectSignalSlot(
                frame, host, owner->DescendantAdded, "DescendantAdded", 2,
                [](CallFrame& cb, const InstancePtr& child) { PushInstance(cb, child); }
            );
            return 1;
        case SignalKind::DescendantRemoved:
            ConnectSignalSlot(
                frame, host, owner->DescendantRemoved, "DescendantRemoved", 2,
                [](CallFrame& cb, const InstancePtr& child) { PushInstance(cb, child); }
            );
            return 1;
        case SignalKind::Destroying:
            ConnectSignalSlot(
                frame, host, owner->Destroying, "Destroying", 2,
                [](CallFrame& cb, const InstancePtr& item) { PushInstance(cb, item); }
            );
            return 1;
        case SignalKind::AncestryChanged:
            ConnectSignalSlot(
                frame, host, owner->AncestryChanged, "AncestryChanged", 2,
                [](CallFrame& cb, const InstancePtr& item, const InstancePtr& parent) {
                    PushInstance(cb, item);
                    PushInstance(cb, parent);
                }
            );
            return 1;
        case SignalKind::PropertyChanged:
            ConnectSignalSlot(
                frame, host, owner->PropertyChanged, "PropertyChanged", 2,
                [](CallFrame& cb, std::string_view prop) { cb.PushString(prop); }
            );
            return 1;
        case SignalKind::HumanoidStateChanged:
            if (owner->IsA("Humanoid")) {
                ConnectSignalSlot(
                    frame, host, static_cast<Humanoid*>(owner.GetPtr())->StateChanged, "StateChanged", 2,
                    [](CallFrame& cb, HumanoidState oldState, HumanoidState newState) {
                        cb.PushEnum(oldState);
                        cb.PushEnum(newState);
                    }
                );
                return 1;
            }
            break;
        case SignalKind::SoundEnded:
            if (owner->IsA("Sound")) {
                ConnectSignalSlot(
                    frame, host, static_cast<Sound*>(owner.GetPtr())->Ended, "Ended", 2,
                    [](CallFrame& /*cb*/) {}
                );
                return 1;
            }
            break;
        }
        frame.PushNil();
        return 1;
    }

    auto ConnectionDisconnect(CallFrame& frame) -> int {
        const auto* conn = frame.TestUserdata<LuaConnectionRef>(1);
        auto*       host = frame.HostContext<BindingHostState>();
        if (conn != nullptr && host != nullptr && host->alive) {
            host->DisconnectSlot(conn->connectionId);
        }
        return 0;
    }

    auto ConnectionIndex(CallFrame& frame) -> int {
        const auto* conn    = frame.TestUserdata<LuaConnectionRef>(1);
        const auto  propOpt = frame.ReadStringView(2);
        auto*       host    = frame.HostContext<BindingHostState>();
        if (conn == nullptr || !propOpt) {
            frame.PushNil();
            return 1;
        }
        if (*propOpt == "Connected") {
            const ConnectionSlot* slot = (host != nullptr && host->alive) ? host->FindConnection(conn->connectionId) : nullptr;
            frame.PushBool(slot != nullptr && slot->connected);
            return 1;
        }
        frame.PushNil();
        return 1;
    }

    template <typename E>
    void PopulateEnumSubtable(CallFrame& frame, std::string_view enumName) noexcept {
        frame.BeginTable(0, static_cast<int>(ZHLN::Reflect::EnumCount<E>()));
        constexpr auto names = ZHLN::Reflect::EnumNames<E>();
        for (const std::string_view name: names) {
            const auto value = ZHLN::Reflect::StringToEnum<E>(name);
            if (!value) {
                continue;
            }
            frame.PushEnum(*value);
            frame.SetFieldTop(name);
        }
        frame.SetFieldTop(enumName);
    }

} // namespace

struct DataModelLuaSession::Impl {
    std::shared_ptr<BindingHostState> host;

    ~Impl() {
        Teardown();
    }

    void Teardown() noexcept {
        if (!host) {
            return;
        }
        host->alive = false;
        host->DisconnectAll();
        if (host->vm != nullptr && host->vm->IsValid()) {
            host->vm->ClearHostContext<BindingHostState>();
            host->vm->ClearHostContext<std::shared_ptr<BindingHostState>>();
        }
        host->vm        = nullptr;
        host->dataModel = nullptr;
    }
};

DataModelLuaSession::DataModelLuaSession() noexcept = default;
DataModelLuaSession::~DataModelLuaSession()         = default;
DataModelLuaSession::DataModelLuaSession(DataModelLuaSession&& other) noexcept = default;
auto DataModelLuaSession::operator=(DataModelLuaSession&& other) noexcept -> DataModelLuaSession& = default;

void DataModelLuaSession::DisconnectAll() noexcept {
    if (m_impl && m_impl->host) {
        m_impl->host->DisconnectAll();
    }
}

auto DataModelLuaSession::LastCallbackError() const noexcept -> std::string_view {
    if (!m_impl || !m_impl->host) {
        return {};
    }
    return m_impl->host->lastCallbackError;
}

auto DataModelLuaSession::Bind(Vm& vm, DataModel& dataModel) -> std::expected<DataModelLuaSession, ZHLN::ErrorCode> {
    if (!vm.IsValid()) {
        return std::unexpected(ZHLN::LuaError::AllocationFailed);
    }

    static constexpr std::array<MethodEntry, 3> kVector3Methods = {{
        {"Dot", &Vector3Dot},
        {"Cross", &Vector3Cross},
        {"Lerp", &Vector3Lerp},
    }};
    static constexpr std::array<MethodEntry, 1> kColor3Methods = {{
        {"Lerp", &Color3Lerp},
    }};
    static constexpr std::array<MethodEntry, 1> kSignalMethods = {{
        {"Connect", &SignalConnect},
    }};
    static constexpr std::array<MethodEntry, 1> kConnectionMethods = {{
        {"Disconnect", &ConnectionDisconnect},
    }};

    if (auto res = vm.RegisterUserdataType<LuaInstanceRef>(MetatableSpec {
            .debugName = "ZHLN.ProjectLight.Instance",
            .index     = &InstanceIndex,
            .newIndex  = &InstanceNewIndex,
            .toString  = &InstanceToString,
            .eq        = &InstanceEqual,
        });
        !res) {
        return std::unexpected(res.error());
    }

    if (auto res = vm.RegisterUserdataType<LuaVector3>(MetatableSpec {
            .debugName = "ZHLN.ProjectLight.Vector3",
            .index     = &Vector3Index,
            .newIndex  = &IgnoreReadOnlyNewIndex,
            .eq        = &Vector3Equal,
            .add       = &Vector3Add,
            .sub       = &Vector3Sub,
            .mul       = &Vector3Mul,
            .div       = &Vector3Div,
            .unm       = &Vector3Unm,
            .methods   = kVector3Methods,
        });
        !res) {
        return std::unexpected(res.error());
    }

    if (auto res = vm.RegisterUserdataType<LuaColor3>(MetatableSpec {
            .debugName = "ZHLN.ProjectLight.Color3",
            .index     = &Color3Index,
            .newIndex  = &IgnoreReadOnlyNewIndex,
            .eq        = &Color3Equal,
            .methods   = kColor3Methods,
        });
        !res) {
        return std::unexpected(res.error());
    }

    if (auto res = vm.RegisterUserdataType<LuaSignalRef>(MetatableSpec {
            .debugName = "ZHLN.ProjectLight.Signal",
            .methods   = kSignalMethods,
        });
        !res) {
        return std::unexpected(res.error());
    }

    if (auto res = vm.RegisterUserdataType<LuaConnectionRef>(MetatableSpec {
            .debugName = "ZHLN.ProjectLight.Connection",
            .index     = &ConnectionIndex,
            .methods   = kConnectionMethods,
        });
        !res) {
        return std::unexpected(res.error());
    }

    auto impl               = std::make_unique<Impl>();
    impl->host              = std::make_shared<BindingHostState>();
    impl->host->vm          = &vm;
    impl->host->dataModel   = &dataModel;
    impl->host->alive       = true;

    vm.SetHostContext<BindingHostState>(impl->host.get());
    vm.SetHostContext<std::shared_ptr<BindingHostState>>(&impl->host);

    const auto globalsRes = vm.WithFrame([&](CallFrame& frame) {
        // Vector3 global table
        frame.BeginTable(0, 6);
        frame.PushFunction(&Vector3New);
        frame.SetFieldTop("new");
        PushVector3(frame, 0.0f, 0.0f, 0.0f);
        frame.SetFieldTop("zero");
        PushVector3(frame, 1.0f, 1.0f, 1.0f);
        frame.SetFieldTop("one");
        PushVector3(frame, 1.0f, 0.0f, 0.0f);
        frame.SetFieldTop("xAxis");
        PushVector3(frame, 0.0f, 1.0f, 0.0f);
        frame.SetFieldTop("yAxis");
        PushVector3(frame, 0.0f, 0.0f, 1.0f);
        frame.SetFieldTop("zAxis");
        frame.SetGlobalTop("Vector3");

        // Color3 global table
        frame.BeginTable(0, 2);
        frame.PushFunction(&Color3New);
        frame.SetFieldTop("new");
        frame.PushFunction(&Color3FromRGB);
        frame.SetFieldTop("fromRGB");
        frame.SetGlobalTop("Color3");

        // Enum global table
        frame.BeginTable(0, 5);
        PopulateEnumSubtable<PartShape>(frame, "PartShape");
        PopulateEnumSubtable<HumanoidState>(frame, "HumanoidState");
        PopulateEnumSubtable<PartSurface>(frame, "PartSurface");
        PopulateEnumSubtable<DecalFace>(frame, "DecalFace");
        PopulateEnumSubtable<DecalWrapMode>(frame, "DecalWrapMode");
        frame.SetGlobalTop("Enum");

        // Instance global table
        frame.BeginTable(0, 1);
        frame.PushFunction(&InstanceNew);
        frame.SetFieldTop("new");
        frame.SetGlobalTop("Instance");

        // Root `game` and optional `workspace`
        PushInstance(frame, dataModel.Root());
        frame.SetGlobalTop("game");
        if (const auto workspace = dataModel.GetService("Workspace")) {
            PushInstance(frame, workspace);
            frame.SetGlobalTop("workspace");
        }
    });

    if (!globalsRes) {
        impl->Teardown();
        return std::unexpected(globalsRes.error());
    }

    DataModelLuaSession session;
    session.m_impl = std::move(impl);
    return session;
}

} // namespace ZHLN::ProjectLight
