// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/Scripting/Lua/DataModelBindings.cpp
//
// The DataModel-facing Roblox-style surface sits on the existing LuaJIT state.
// It is deliberately an adapter: Instance userdata retain the canonical C++
// objects, while value userdata and event connections are Lua-boundary details.

#include <Scripting/Lua/DataModelBindings.hpp>

#include <DataModel/DataModel.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Log.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace ZHLN::ProjectLight {
namespace {

constexpr char kInstanceMetatable[] = "ZHLN.ProjectLight.Instance";
constexpr char kVector3Metatable[] = "ZHLN.ProjectLight.Vector3";
constexpr char kColor3Metatable[] = "ZHLN.ProjectLight.Color3";
constexpr char kSignalMetatable[] = "ZHLN.ProjectLight.Signal";
constexpr char kConnectionMetatable[] = "ZHLN.ProjectLight.Connection";
constexpr char kContextMetatable[] = "ZHLN.ProjectLight.BindingContext";

char kBindingContextRegistryKey;
char kInstanceCacheRegistryKey;

struct LuaInstance {
    InstancePtr instance;
};

struct LuaVector3 {
    float x;
    float y;
    float z;
};

struct LuaColor3 {
    float r;
    float g;
    float b;
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

struct LuaSignal {
    InstancePtr owner;
    SignalKind kind;
};

struct LuaConnectionRecord {
    lua_State* state = nullptr;
    int callbackRef = LUA_NOREF;
    bool connected = true;
    std::function<void()> disconnect;

    LuaConnectionRecord() = default;
    LuaConnectionRecord(const LuaConnectionRecord&) = delete;
    auto operator=(const LuaConnectionRecord&) -> LuaConnectionRecord& = delete;

    ~LuaConnectionRecord() {
        Disconnect();
    }

    void Disconnect() {
        if (!connected) return;
        connected = false;
        if (disconnect) {
            disconnect();
            disconnect = {};
        }
        if (state != nullptr && callbackRef != LUA_NOREF && callbackRef != LUA_REFNIL) {
            luaL_unref(state, LUA_REGISTRYINDEX, callbackRef);
        }
        callbackRef = LUA_NOREF;
    }
};

struct LuaBindingContext {
    lua_State* state = nullptr;
    DataModel* dataModel = nullptr;
    bool alive = true;
    std::vector<std::shared_ptr<LuaConnectionRecord>> connections;

    ~LuaBindingContext() {
        alive = false;
        for (const auto& connection: connections) {
            if (connection) connection->Disconnect();
        }
        connections.clear();
    }
};

struct LuaContextUserdata {
    LuaBindingContext* context = nullptr;
};

struct LuaConnectionUserdata {
    std::shared_ptr<LuaConnectionRecord> connection;
};

template <typename T>
auto TestUserdata(lua_State* state, int index, const char* metatable) -> T* {
    if (lua_type(state, index) != LUA_TUSERDATA || !lua_getmetatable(state, index)) return nullptr;
    luaL_getmetatable(state, metatable);
    const bool matches = lua_rawequal(state, -1, -2) != 0;
    lua_pop(state, 2);
    return matches ? static_cast<T*>(lua_touserdata(state, index)) : nullptr;
}

auto GetBindingContext(lua_State* state) -> LuaBindingContext* {
    lua_pushlightuserdata(state, &kBindingContextRegistryKey);
    lua_rawget(state, LUA_REGISTRYINDEX);
    auto* holder = static_cast<LuaContextUserdata*>(lua_touserdata(state, -1));
    LuaBindingContext* context = holder != nullptr ? holder->context : nullptr;
    lua_pop(state, 1);
    return context;
}

auto TestInstance(lua_State* state, int index) -> LuaInstance* {
    return TestUserdata<LuaInstance>(state, index, kInstanceMetatable);
}

auto TestVector3(lua_State* state, int index) -> LuaVector3* {
    return TestUserdata<LuaVector3>(state, index, kVector3Metatable);
}

auto TestColor3(lua_State* state, int index) -> LuaColor3* {
    return TestUserdata<LuaColor3>(state, index, kColor3Metatable);
}

void PushVector3(lua_State* state, float x, float y, float z) {
    auto* value = static_cast<LuaVector3*>(lua_newuserdata(state, sizeof(LuaVector3)));
    new (value) LuaVector3 {.x = x, .y = y, .z = z};
    luaL_getmetatable(state, kVector3Metatable);
    lua_setmetatable(state, -2);
}

void PushVector3(lua_State* state, const JPH::Vec3& value) {
    PushVector3(state, value.GetX(), value.GetY(), value.GetZ());
}

void PushColor3(lua_State* state, float r, float g, float b) {
    auto* value = static_cast<LuaColor3*>(lua_newuserdata(state, sizeof(LuaColor3)));
    new (value) LuaColor3 {.r = r, .g = g, .b = b};
    luaL_getmetatable(state, kColor3Metatable);
    lua_setmetatable(state, -2);
}

void PushInstance(lua_State* state, const InstancePtr& instance) {
    if (!instance) {
        lua_pushnil(state);
        return;
    }

    // A weak-value registry makes each live C++ Instance map to one Lua
    // userdata identity. Besides matching Instance equality, this matters for
    // scripts that use Instances as table keys (including weak-key tables).
    lua_pushlightuserdata(state, &kInstanceCacheRegistryKey);
    lua_rawget(state, LUA_REGISTRYINDEX);
    if (lua_istable(state, -1)) {
        lua_pushlightuserdata(state, instance.get());
        lua_rawget(state, -2);
        if (!lua_isnil(state, -1)) {
            lua_remove(state, -2);
            return;
        }
        lua_pop(state, 1);

        auto* value = static_cast<LuaInstance*>(lua_newuserdata(state, sizeof(LuaInstance)));
        new (value) LuaInstance {.instance = instance};
        luaL_getmetatable(state, kInstanceMetatable);
        lua_setmetatable(state, -2);

        lua_pushlightuserdata(state, instance.get());
        lua_pushvalue(state, -2);
        lua_rawset(state, -4);
        lua_remove(state, -2);
        return;
    }

    lua_pop(state, 1);
    auto* value = static_cast<LuaInstance*>(lua_newuserdata(state, sizeof(LuaInstance)));
    new (value) LuaInstance {.instance = instance};
    luaL_getmetatable(state, kInstanceMetatable);
    lua_setmetatable(state, -2);
}

void PushSignal(lua_State* state, const InstancePtr& owner, SignalKind kind) {
    auto* signal = static_cast<LuaSignal*>(lua_newuserdata(state, sizeof(LuaSignal)));
    new (signal) LuaSignal {.owner = owner, .kind = kind};
    luaL_getmetatable(state, kSignalMetatable);
    lua_setmetatable(state, -2);
}

void PushConnection(lua_State* state, std::shared_ptr<LuaConnectionRecord> connection) {
    auto* value = static_cast<LuaConnectionUserdata*>(lua_newuserdata(state, sizeof(LuaConnectionUserdata)));
    new (value) LuaConnectionUserdata {.connection = std::move(connection)};
    luaL_getmetatable(state, kConnectionMetatable);
    lua_setmetatable(state, -2);
}

auto ReadVector3(lua_State* state, int index, JPH::Vec3& out) -> bool {
    if (const auto* value = TestVector3(state, index)) {
        out = JPH::Vec3(value->x, value->y, value->z);
        return true;
    }
    if (lua_type(state, index) != LUA_TTABLE) return false;

    const int absoluteIndex = index > 0 ? index : lua_gettop(state) + index + 1;
    lua_getfield(state, absoluteIndex, "X");
    const bool hasX = lua_isnumber(state, -1) != 0;
    const float x = static_cast<float>(lua_tonumber(state, -1));
    lua_pop(state, 1);
    lua_getfield(state, absoluteIndex, "Y");
    const bool hasY = lua_isnumber(state, -1) != 0;
    const float y = static_cast<float>(lua_tonumber(state, -1));
    lua_pop(state, 1);
    lua_getfield(state, absoluteIndex, "Z");
    const bool hasZ = lua_isnumber(state, -1) != 0;
    const float z = static_cast<float>(lua_tonumber(state, -1));
    lua_pop(state, 1);
    if (!hasX || !hasY || !hasZ) return false;
    out = JPH::Vec3(x, y, z);
    return true;
}

auto ReadColor3(lua_State* state, int index, JPH::Vec3& out) -> bool {
    if (const auto* value = TestColor3(state, index)) {
        out = JPH::Vec3(value->r, value->g, value->b);
        return true;
    }
    return ReadVector3(state, index, out);
}

auto ReadNumber(lua_State* state, int index, float& out) -> bool {
    if (!lua_isnumber(state, index)) return false;
    out = static_cast<float>(lua_tonumber(state, index));
    return true;
}

auto ReadString(lua_State* state, int index, std::string& out) -> bool {
    if (lua_type(state, index) != LUA_TSTRING) return false;
    size_t length = 0;
    const char* value = lua_tolstring(state, index, &length);
    out.assign(value != nullptr ? value : "", length);
    return true;
}

auto ReadInstance(lua_State* state, int index, InstancePtr& out) -> bool {
    if (const auto* value = TestInstance(state, index); value != nullptr && value->instance) {
        out = value->instance;
        return true;
    }
    return false;
}

auto ReadEnumName(lua_State* state, int index) -> std::string_view {
    if (lua_type(state, index) != LUA_TSTRING) return {};
    size_t length = 0;
    const char* value = lua_tolstring(state, index, &length);
    return value != nullptr ? std::string_view(value, length) : std::string_view {};
}

auto EqualAsciiCaseInsensitive(std::string_view lhs, std::string_view rhs) noexcept -> bool {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(lhs[i]);
        const unsigned char b = static_cast<unsigned char>(rhs[i]);
        const char lowerA = static_cast<char>(a >= 'A' && a <= 'Z' ? a + ('a' - 'A') : a);
        const char lowerB = static_cast<char>(b >= 'A' && b <= 'Z' ? b + ('a' - 'A') : b);
        if (lowerA != lowerB) return false;
    }
    return true;
}

template <typename E>
struct ReflectedEnumValues {
    std::array<E, ZHLN::Reflect::EnumCount<E>()> values {};
    size_t count = 0;
};

template <typename E>
consteval auto GetReflectedEnumValues() -> ReflectedEnumValues<E> {
    ReflectedEnumValues<E> result;
    constexpr auto names = ZHLN::Reflect::EnumNames<E>();
    for (const std::string_view name: names) {
        const auto value = ZHLN::Reflect::StringToEnum<E>(name);
        if (!value) continue;
        bool alreadyAdded = false;
        for (size_t i = 0; i < result.count; ++i) {
            if (result.values[i] == *value) {
                alreadyAdded = true;
                break;
            }
        }
        if (!alreadyAdded) result.values[result.count++] = *value;
    }
    return result;
}

template <typename E>
auto ReadEnum(lua_State* state, int index, E& out) -> bool {
    const std::string_view name = ReadEnumName(state, index);
    if (!name.empty()) {
        constexpr auto names = ZHLN::Reflect::EnumNames<E>();
        for (const std::string_view candidateName: names) {
            if (!EqualAsciiCaseInsensitive(name, candidateName)) continue;
            const auto value = ZHLN::Reflect::StringToEnum<E>(candidateName);
            if (!value) return false;
            out = *value;
            return true;
        }
        return false;
    }
    if (!lua_isnumber(state, index)) return false;
    const lua_Number number = lua_tonumber(state, index);
    constexpr auto values = GetReflectedEnumValues<E>();
    if (number < 0 || number >= static_cast<lua_Number>(values.count) || std::floor(number) != number) return false;
    out = values.values[static_cast<size_t>(number)];
    return true;
}

template <typename E>
auto CanonicalEnumName(E value) -> std::string_view {
    constexpr auto names = ZHLN::Reflect::EnumNames<E>();
    for (const std::string_view name: names) {
        const auto candidate = ZHLN::Reflect::StringToEnum<E>(name);
        if (candidate && *candidate == value) return name;
    }
    return ZHLN::Reflect::EnumToString(value);
}

template <typename E>
void PushEnum(lua_State* state, E value) {
    const std::string_view name = CanonicalEnumName(value);
    lua_pushlstring(state, name.data(), name.size());
}

auto ReadPartShape(lua_State* state, int index, PartShape& out) -> bool {
    return ReadEnum(state, index, out);
}

auto ReadPartSurface(lua_State* state, int index, PartSurface& out) -> bool {
    return ReadEnum(state, index, out);
}

auto ReadDecalFace(lua_State* state, int index, DecalFace& out) -> bool {
    return ReadEnum(state, index, out);
}

auto ReadDecalWrapMode(lua_State* state, int index, DecalWrapMode& out) -> bool {
    return ReadEnum(state, index, out);
}

auto ReadHumanoidState(lua_State* state, int index, HumanoidState& out) -> bool {
    return ReadEnum(state, index, out);
}

void PushPartShape(lua_State* state, PartShape value) {
    PushEnum(state, value);
}

void PushPartSurface(lua_State* state, PartSurface value) {
    PushEnum(state, value);
}

void PushDecalFace(lua_State* state, DecalFace value) {
    PushEnum(state, value);
}

void PushDecalWrapMode(lua_State* state, DecalWrapMode value) {
    PushEnum(state, value);
}

void PushHumanoidState(lua_State* state, HumanoidState value) {
    PushEnum(state, value);
}

void PushInstanceArray(lua_State* state, const std::vector<InstancePtr>& instances) {
    lua_createtable(state, static_cast<int>(instances.size()), 0);
    int index = 1;
    for (const InstancePtr& instance: instances) {
        PushInstance(state, instance);
        lua_rawseti(state, -2, index++);
    }
}

void PushSignalProperty(lua_State* state, const InstancePtr& instance, std::string_view property) {
    if (property == "ChildAdded") PushSignal(state, instance, SignalKind::ChildAdded);
    else if (property == "ChildRemoved") PushSignal(state, instance, SignalKind::ChildRemoved);
    else if (property == "DescendantAdded") PushSignal(state, instance, SignalKind::DescendantAdded);
    else if (property == "DescendantRemoved") PushSignal(state, instance, SignalKind::DescendantRemoved);
    else if (property == "Destroying") PushSignal(state, instance, SignalKind::Destroying);
    else if (property == "AncestryChanged") PushSignal(state, instance, SignalKind::AncestryChanged);
    else if (property == "PropertyChanged") PushSignal(state, instance, SignalKind::PropertyChanged);
    else if (property == "StateChanged" && instance->IsA("Humanoid")) PushSignal(state, instance, SignalKind::HumanoidStateChanged);
    else if (property == "Ended" && instance->IsA("Sound")) PushSignal(state, instance, SignalKind::SoundEnded);
    else lua_pushnil(state);
}

int InstanceGc(lua_State* state) {
    if (auto* value = TestInstance(state, 1)) value->~LuaInstance();
    return 0;
}

int Vector3Gc(lua_State* state) {
    if (auto* value = TestVector3(state, 1)) value->~LuaVector3();
    return 0;
}

int Color3Gc(lua_State* state) {
    if (auto* value = TestColor3(state, 1)) value->~LuaColor3();
    return 0;
}

int SignalGc(lua_State* state) {
    if (auto* value = TestUserdata<LuaSignal>(state, 1, kSignalMetatable)) value->~LuaSignal();
    return 0;
}

int ConnectionGc(lua_State* state) {
    if (auto* value = TestUserdata<LuaConnectionUserdata>(state, 1, kConnectionMetatable)) value->~LuaConnectionUserdata();
    return 0;
}

int ContextGc(lua_State* state) {
    auto* value = TestUserdata<LuaContextUserdata>(state, 1, kContextMetatable);
    if (value != nullptr && value->context != nullptr) {
        delete value->context;
        value->context = nullptr;
    }
    return 0;
}

int InstanceToString(lua_State* state) {
    const auto* value = TestInstance(state, 1);
    if (value == nullptr || !value->instance) {
        lua_pushliteral(state, "Instance<expired>");
        return 1;
    }
    const std::string fullName = value->instance->GetFullName();
    lua_pushlstring(state, fullName.data(), fullName.size());
    return 1;
}

int InstanceEqual(lua_State* state) {
    const auto* lhs = TestInstance(state, 1);
    const auto* rhs = TestInstance(state, 2);
    lua_pushboolean(state, lhs != nullptr && rhs != nullptr && lhs->instance.get() == rhs->instance.get());
    return 1;
}

int Vector3Dot(lua_State* state);
int Vector3Cross(lua_State* state);
int Vector3Lerp(lua_State* state);
int Color3Lerp(lua_State* state);

int Vector3Index(lua_State* state) {
    const auto* value = TestVector3(state, 1);
    if (value == nullptr || lua_type(state, 2) != LUA_TSTRING) {
        lua_pushnil(state);
        return 1;
    }
    size_t length = 0;
    const char* key = lua_tolstring(state, 2, &length);
    const std::string_view property(key != nullptr ? key : "", length);
    if (property == "X" || property == "x") lua_pushnumber(state, value->x);
    else if (property == "Y" || property == "y") lua_pushnumber(state, value->y);
    else if (property == "Z" || property == "z") lua_pushnumber(state, value->z);
    else if (property == "Magnitude") lua_pushnumber(state, std::sqrt(value->x * value->x + value->y * value->y + value->z * value->z));
    else if (property == "Unit") {
        const float magnitude = std::sqrt(value->x * value->x + value->y * value->y + value->z * value->z);
        if (magnitude > 0.0f) PushVector3(state, value->x / magnitude, value->y / magnitude, value->z / magnitude);
        else PushVector3(state, 0.0f, 0.0f, 0.0f);
    } else if (property == "Dot") lua_pushcfunction(state, Vector3Dot);
    else if (property == "Cross") lua_pushcfunction(state, Vector3Cross);
    else if (property == "Lerp") lua_pushcfunction(state, Vector3Lerp);
    else lua_pushnil(state);
    return 1;
}

int Vector3New(lua_State* state) {
    const float x = static_cast<float>(lua_tonumber(state, 1));
    const float y = static_cast<float>(lua_tonumber(state, 2));
    const float z = static_cast<float>(lua_tonumber(state, 3));
    PushVector3(state, x, y, z);
    return 1;
}

int Vector3Add(lua_State* state) {
    const auto* lhs = TestVector3(state, 1);
    const auto* rhs = TestVector3(state, 2);
    if (lhs == nullptr || rhs == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    PushVector3(state, lhs->x + rhs->x, lhs->y + rhs->y, lhs->z + rhs->z);
    return 1;
}

int Vector3Sub(lua_State* state) {
    const auto* lhs = TestVector3(state, 1);
    const auto* rhs = TestVector3(state, 2);
    if (lhs == nullptr || rhs == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    PushVector3(state, lhs->x - rhs->x, lhs->y - rhs->y, lhs->z - rhs->z);
    return 1;
}

int Vector3Unm(lua_State* state) {
    const auto* value = TestVector3(state, 1);
    if (value == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    PushVector3(state, -value->x, -value->y, -value->z);
    return 1;
}

int Vector3Mul(lua_State* state) {
    const auto* lhs = TestVector3(state, 1);
    const auto* rhs = TestVector3(state, 2);
    if (lhs != nullptr && lua_isnumber(state, 2)) {
        const float scale = static_cast<float>(lua_tonumber(state, 2));
        PushVector3(state, lhs->x * scale, lhs->y * scale, lhs->z * scale);
    } else if (rhs != nullptr && lua_isnumber(state, 1)) {
        const float scale = static_cast<float>(lua_tonumber(state, 1));
        PushVector3(state, rhs->x * scale, rhs->y * scale, rhs->z * scale);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

int Vector3Div(lua_State* state) {
    const auto* lhs = TestVector3(state, 1);
    if (lhs == nullptr || !lua_isnumber(state, 2)) {
        lua_pushnil(state);
        return 1;
    }
    const float divisor = static_cast<float>(lua_tonumber(state, 2));
    if (divisor == 0.0f) {
        lua_pushnil(state);
        return 1;
    }
    PushVector3(state, lhs->x / divisor, lhs->y / divisor, lhs->z / divisor);
    return 1;
}

int Vector3Dot(lua_State* state) {
    const auto* lhs = TestVector3(state, 1);
    const auto* rhs = TestVector3(state, 2);
    if (lhs == nullptr || rhs == nullptr) lua_pushnumber(state, 0.0);
    else lua_pushnumber(state, lhs->x * rhs->x + lhs->y * rhs->y + lhs->z * rhs->z);
    return 1;
}

int Vector3Cross(lua_State* state) {
    const auto* lhs = TestVector3(state, 1);
    const auto* rhs = TestVector3(state, 2);
    if (lhs == nullptr || rhs == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    PushVector3(state,
        lhs->y * rhs->z - lhs->z * rhs->y,
        lhs->z * rhs->x - lhs->x * rhs->z,
        lhs->x * rhs->y - lhs->y * rhs->x);
    return 1;
}

int Vector3Lerp(lua_State* state) {
    const auto* lhs = TestVector3(state, 1);
    const auto* rhs = TestVector3(state, 2);
    const float alpha = static_cast<float>(lua_tonumber(state, 3));
    if (lhs == nullptr || rhs == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    PushVector3(state,
        lhs->x + (rhs->x - lhs->x) * alpha,
        lhs->y + (rhs->y - lhs->y) * alpha,
        lhs->z + (rhs->z - lhs->z) * alpha);
    return 1;
}

int Vector3Equal(lua_State* state) {
    const auto* lhs = TestVector3(state, 1);
    const auto* rhs = TestVector3(state, 2);
    lua_pushboolean(state, lhs != nullptr && rhs != nullptr && lhs->x == rhs->x && lhs->y == rhs->y && lhs->z == rhs->z);
    return 1;
}

int Color3Index(lua_State* state) {
    const auto* value = TestColor3(state, 1);
    if (value == nullptr || lua_type(state, 2) != LUA_TSTRING) {
        lua_pushnil(state);
        return 1;
    }
    size_t length = 0;
    const char* key = lua_tolstring(state, 2, &length);
    const std::string_view property(key != nullptr ? key : "", length);
    if (property == "R" || property == "r") lua_pushnumber(state, value->r);
    else if (property == "G" || property == "g") lua_pushnumber(state, value->g);
    else if (property == "B" || property == "b") lua_pushnumber(state, value->b);
    else if (property == "Lerp") lua_pushcfunction(state, Color3Lerp);
    else lua_pushnil(state);
    return 1;
}

int Color3New(lua_State* state) {
    PushColor3(state,
        static_cast<float>(lua_tonumber(state, 1)),
        static_cast<float>(lua_tonumber(state, 2)),
        static_cast<float>(lua_tonumber(state, 3)));
    return 1;
}

int Color3FromRGB(lua_State* state) {
    constexpr float scale = 1.0f / 255.0f;
    PushColor3(state,
        static_cast<float>(lua_tonumber(state, 1)) * scale,
        static_cast<float>(lua_tonumber(state, 2)) * scale,
        static_cast<float>(lua_tonumber(state, 3)) * scale);
    return 1;
}

int Color3Lerp(lua_State* state) {
    const auto* lhs = TestColor3(state, 1);
    const auto* rhs = TestColor3(state, 2);
    const float alpha = static_cast<float>(lua_tonumber(state, 3));
    if (lhs == nullptr || rhs == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    PushColor3(state,
        lhs->r + (rhs->r - lhs->r) * alpha,
        lhs->g + (rhs->g - lhs->g) * alpha,
        lhs->b + (rhs->b - lhs->b) * alpha);
    return 1;
}

int Color3Equal(lua_State* state) {
    const auto* lhs = TestColor3(state, 1);
    const auto* rhs = TestColor3(state, 2);
    lua_pushboolean(state, lhs != nullptr && rhs != nullptr && lhs->r == rhs->r && lhs->g == rhs->g && lhs->b == rhs->b);
    return 1;
}

int MethodGetService(lua_State* state) {
    const auto* self = TestInstance(state, 1);
    if (self == nullptr || !self->instance || !self->instance->IsDataModelRoot()) {
        lua_pushnil(state);
        return 1;
    }
    LuaBindingContext* context = GetBindingContext(state);
    if (context == nullptr || context->dataModel == nullptr || lua_type(state, 2) != LUA_TSTRING) {
        lua_pushnil(state);
        return 1;
    }
    size_t length = 0;
    const char* name = lua_tolstring(state, 2, &length);
    PushInstance(state, context->dataModel->GetService(std::string_view(name != nullptr ? name : "", length)));
    return 1;
}

int MethodGetChildren(lua_State* state) {
    const auto* self = TestInstance(state, 1);
    if (self == nullptr || !self->instance) {
        lua_newtable(state);
        return 1;
    }
    PushInstanceArray(state, self->instance->GetChildren());
    return 1;
}

int MethodGetDescendants(lua_State* state) {
    const auto* self = TestInstance(state, 1);
    if (self == nullptr || !self->instance) {
        lua_newtable(state);
        return 1;
    }
    PushInstanceArray(state, self->instance->GetDescendants());
    return 1;
}

int MethodGetAncestors(lua_State* state) {
    const auto* self = TestInstance(state, 1);
    if (self == nullptr || !self->instance) {
        lua_newtable(state);
        return 1;
    }
    PushInstanceArray(state, self->instance->GetAncestors());
    return 1;
}

int MethodFindFirstChild(lua_State* state) {
    const auto* self = TestInstance(state, 1);
    if (self == nullptr || !self->instance || lua_type(state, 2) != LUA_TSTRING) {
        lua_pushnil(state);
        return 1;
    }
    size_t length = 0;
    const char* name = lua_tolstring(state, 2, &length);
    const bool recursive = lua_toboolean(state, 3) != 0;
    PushInstance(state, self->instance->FindFirstChild(std::string_view(name != nullptr ? name : "", length), recursive));
    return 1;
}

int MethodFindFirstChildOfClass(lua_State* state) {
    const auto* self = TestInstance(state, 1);
    if (self == nullptr || !self->instance || lua_type(state, 2) != LUA_TSTRING) {
        lua_pushnil(state);
        return 1;
    }
    size_t length = 0;
    const char* name = lua_tolstring(state, 2, &length);
    const bool recursive = lua_toboolean(state, 3) != 0;
    PushInstance(state, self->instance->FindFirstChildOfClass(std::string_view(name != nullptr ? name : "", length), recursive));
    return 1;
}

int MethodFindFirstChildWhichIsA(lua_State* state) {
    const auto* self = TestInstance(state, 1);
    if (self == nullptr || !self->instance || lua_type(state, 2) != LUA_TSTRING) {
        lua_pushnil(state);
        return 1;
    }
    size_t length = 0;
    const char* name = lua_tolstring(state, 2, &length);
    const bool recursive = lua_toboolean(state, 3) != 0;
    PushInstance(state, self->instance->FindFirstChildWhichIsA(std::string_view(name != nullptr ? name : "", length), recursive));
    return 1;
}

int MethodIsA(lua_State* state) {
    const auto* self = TestInstance(state, 1);
    if (self == nullptr || !self->instance || lua_type(state, 2) != LUA_TSTRING) {
        lua_pushboolean(state, false);
        return 1;
    }
    size_t length = 0;
    const char* name = lua_tolstring(state, 2, &length);
    lua_pushboolean(state, self->instance->IsA(std::string_view(name != nullptr ? name : "", length)));
    return 1;
}

int MethodIsDescendantOf(lua_State* state) {
    const auto* self = TestInstance(state, 1);
    const auto* ancestor = TestInstance(state, 2);
    lua_pushboolean(state,
        self != nullptr && self->instance && ancestor != nullptr && ancestor->instance &&
        self->instance->IsDescendantOf(ancestor->instance));
    return 1;
}

int MethodGetFullName(lua_State* state) {
    const auto* self = TestInstance(state, 1);
    if (self == nullptr || !self->instance) {
        lua_pushliteral(state, "");
        return 1;
    }
    const std::string fullName = self->instance->GetFullName();
    lua_pushlstring(state, fullName.data(), fullName.size());
    return 1;
}

int MethodDestroy(lua_State* state) {
    const auto* self = TestInstance(state, 1);
    if (self != nullptr && self->instance) self->instance->Destroy();
    return 0;
}

int MethodPlay(lua_State* state) {
    const auto* self = TestInstance(state, 1);
    if (self != nullptr && self->instance && self->instance->IsA("Sound")) {
        static_cast<Sound*>(self->instance.get())->Play();
    }
    return 0;
}

int MethodStop(lua_State* state) {
    const auto* self = TestInstance(state, 1);
    if (self != nullptr && self->instance && self->instance->IsA("Sound")) {
        static_cast<Sound*>(self->instance.get())->Stop();
    }
    return 0;
}

int MethodGetPlayers(lua_State* state) {
    const auto* self = TestInstance(state, 1);
    if (self == nullptr || !self->instance || !self->instance->IsA("PlayersService")) {
        lua_newtable(state);
        return 1;
    }
    const auto players = static_cast<PlayersService*>(self->instance.get())->GetPlayers();
    lua_createtable(state, static_cast<int>(players.size()), 0);
    int index = 1;
    for (const auto& player: players) {
        PushInstance(state, player);
        lua_rawseti(state, -2, index++);
    }
    return 1;
}

int MethodFindPlayerByUserId(lua_State* state) {
    const auto* self = TestInstance(state, 1);
    if (self == nullptr || !self->instance || !self->instance->IsA("PlayersService") || !lua_isnumber(state, 2)) {
        lua_pushnil(state);
        return 1;
    }
    const double number = lua_tonumber(state, 2);
    if (number < 0.0) {
        lua_pushnil(state);
        return 1;
    }
    PushInstance(state, static_cast<PlayersService*>(self->instance.get())->FindPlayerByUserId(static_cast<uint64_t>(number)));
    return 1;
}

void PushMethod(lua_State* state, std::string_view name) {
    if (name == "GetService") lua_pushcfunction(state, MethodGetService);
    else if (name == "GetChildren") lua_pushcfunction(state, MethodGetChildren);
    else if (name == "GetDescendants") lua_pushcfunction(state, MethodGetDescendants);
    else if (name == "GetAncestors") lua_pushcfunction(state, MethodGetAncestors);
    else if (name == "FindFirstChild") lua_pushcfunction(state, MethodFindFirstChild);
    else if (name == "FindFirstChildOfClass") lua_pushcfunction(state, MethodFindFirstChildOfClass);
    else if (name == "FindFirstChildWhichIsA") lua_pushcfunction(state, MethodFindFirstChildWhichIsA);
    else if (name == "IsA") lua_pushcfunction(state, MethodIsA);
    else if (name == "IsDescendantOf") lua_pushcfunction(state, MethodIsDescendantOf);
    else if (name == "GetFullName") lua_pushcfunction(state, MethodGetFullName);
    else if (name == "Destroy") lua_pushcfunction(state, MethodDestroy);
    else if (name == "Play") lua_pushcfunction(state, MethodPlay);
    else if (name == "Stop") lua_pushcfunction(state, MethodStop);
    else if (name == "GetPlayers") lua_pushcfunction(state, MethodGetPlayers);
    else if (name == "FindPlayerByUserId") lua_pushcfunction(state, MethodFindPlayerByUserId);
    else lua_pushnil(state);
}

int InstanceIndex(lua_State* state) {
    const auto* value = TestInstance(state, 1);
    if (value == nullptr || !value->instance || lua_type(state, 2) != LUA_TSTRING) {
        lua_pushnil(state);
        return 1;
    }
    size_t length = 0;
    const char* key = lua_tolstring(state, 2, &length);
    const std::string_view property(key != nullptr ? key : "", length);
    const InstancePtr& instance = value->instance;

    if (property == "Name") lua_pushlstring(state, instance->Name().data(), instance->Name().size());
    else if (property == "ClassName") lua_pushlstring(state, instance->ClassName().data(), instance->ClassName().size());
    else if (property == "Parent") PushInstance(state, instance->Parent());
    else if (property == "InstanceId") {
        const std::string id = std::to_string(instance->Id());
        lua_pushlstring(state, id.data(), id.size());
    } else if (property == "IsDestroyed") lua_pushboolean(state, instance->IsDestroyed());
    else if (property == "ChildAdded" || property == "ChildRemoved" || property == "DescendantAdded" ||
             property == "DescendantRemoved" || property == "Destroying" || property == "AncestryChanged" ||
             property == "PropertyChanged" || property == "StateChanged" || property == "Ended") {
        PushSignalProperty(state, instance, property);
    } else if (instance->IsA("BasePart") && property == "Position") PushVector3(state, static_cast<BasePart*>(instance.get())->Position());
    else if (instance->IsA("BasePart") && property == "Rotation") PushVector3(state, static_cast<BasePart*>(instance.get())->Rotation());
    else if (instance->IsA("BasePart") && property == "Size") PushVector3(state, static_cast<BasePart*>(instance.get())->Size());
    else if (instance->IsA("BasePart") && property == "Color") {
        const JPH::Vec3& color = static_cast<BasePart*>(instance.get())->Color();
        PushColor3(state, color.GetX(), color.GetY(), color.GetZ());
    } else if (instance->IsA("BasePart") && property == "Anchored") lua_pushboolean(state, static_cast<BasePart*>(instance.get())->Anchored());
    else if (instance->IsA("BasePart") && property == "CanCollide") lua_pushboolean(state, static_cast<BasePart*>(instance.get())->CanCollide());
    else if (instance->IsA("BasePart") && property == "Transparency") lua_pushnumber(state, static_cast<BasePart*>(instance.get())->Transparency());
    else if (instance->IsA("BasePart") && property == "NetworkOwner") PushInstance(state, static_cast<BasePart*>(instance.get())->NetworkOwner());
    else if (instance->IsA("Part") && property == "Shape") PushPartShape(state, static_cast<Part*>(instance.get())->Shape());
    else if (instance->IsA("Part") && property == "FrontSurface") PushPartSurface(state, static_cast<Part*>(instance.get())->FrontSurface());
    else if (instance->IsA("Part") && property == "BackSurface") PushPartSurface(state, static_cast<Part*>(instance.get())->BackSurface());
    else if (instance->IsA("Part") && property == "TopSurface") PushPartSurface(state, static_cast<Part*>(instance.get())->TopSurface());
    else if (instance->IsA("Part") && property == "BottomSurface") PushPartSurface(state, static_cast<Part*>(instance.get())->BottomSurface());
    else if (instance->IsA("Part") && property == "LeftSurface") PushPartSurface(state, static_cast<Part*>(instance.get())->LeftSurface());
    else if (instance->IsA("Part") && property == "RightSurface") PushPartSurface(state, static_cast<Part*>(instance.get())->RightSurface());
    else if (instance->IsA("MeshPart") && property == "MeshId") {
        const std::string& meshId = static_cast<MeshPart*>(instance.get())->MeshId();
        lua_pushlstring(state, meshId.data(), meshId.size());
    } else if (instance->IsA("SpawnPoint") && property == "Position") PushVector3(state, static_cast<SpawnPoint*>(instance.get())->Position());
    else if (instance->IsA("SpawnPoint") && property == "Rotation") PushVector3(state, static_cast<SpawnPoint*>(instance.get())->Rotation());
    else if (instance->IsA("Model") && property == "PrimaryPart") PushInstance(state, static_cast<Model*>(instance.get())->PrimaryPart());
    else if (instance->IsA("Humanoid") && property == "WalkSpeed") lua_pushnumber(state, static_cast<Humanoid*>(instance.get())->WalkSpeed());
    else if (instance->IsA("Humanoid") && property == "JumpPower") lua_pushnumber(state, static_cast<Humanoid*>(instance.get())->JumpPower());
    else if (instance->IsA("Humanoid") && property == "Health") lua_pushnumber(state, static_cast<Humanoid*>(instance.get())->Health());
    else if (instance->IsA("Humanoid") && property == "MaxHealth") lua_pushnumber(state, static_cast<Humanoid*>(instance.get())->MaxHealth());
    else if (instance->IsA("Humanoid") && property == "RootPart") PushInstance(state, static_cast<Humanoid*>(instance.get())->RootPart());
    else if (instance->IsA("Humanoid") && property == "State") PushHumanoidState(state, static_cast<Humanoid*>(instance.get())->State());
    else if (instance->IsA("Player") && property == "UserId") lua_pushnumber(state, static_cast<lua_Number>(static_cast<Player*>(instance.get())->UserId()));
    else if (instance->IsA("Player") && property == "Character") PushInstance(state, static_cast<Player*>(instance.get())->Character());
    else if (instance->IsA("PlayersService") && property == "LocalPlayer") PushInstance(state, static_cast<PlayersService*>(instance.get())->LocalPlayer());
    else if (instance->IsA("WorkspaceService") && property == "CurrentCamera") PushInstance(state, static_cast<WorkspaceService*>(instance.get())->CurrentCamera());
    else if (instance->IsA("PhysicsService") && property == "ServerAuthority") lua_pushboolean(state, static_cast<PhysicsService*>(instance.get())->ServerAuthority());
    else if (instance->IsA("Sound") && property == "SoundId") {
        const std::string& soundId = static_cast<Sound*>(instance.get())->SoundId();
        lua_pushlstring(state, soundId.data(), soundId.size());
    } else if (instance->IsA("Sound") && property == "Volume") lua_pushnumber(state, static_cast<Sound*>(instance.get())->Volume());
    else if (instance->IsA("Sound") && property == "Loops") lua_pushboolean(state, static_cast<Sound*>(instance.get())->Loops());
    else if (instance->IsA("Sound") && property == "Playing") lua_pushboolean(state, static_cast<Sound*>(instance.get())->Playing());
    else if (instance->IsA("Decal") && property == "TextureId") {
        const std::string& textureId = static_cast<Decal*>(instance.get())->TextureId();
        lua_pushlstring(state, textureId.data(), textureId.size());
    } else if (instance->IsA("Decal") && property == "Color") {
        const JPH::Vec3& color = static_cast<Decal*>(instance.get())->Color();
        PushColor3(state, color.GetX(), color.GetY(), color.GetZ());
    } else if (instance->IsA("Decal") && property == "Face") PushDecalFace(state, static_cast<Decal*>(instance.get())->Face());
    else if (instance->IsA("Decal") && property == "WrapMode") PushDecalWrapMode(state, static_cast<Decal*>(instance.get())->WrapMode());
    else if (instance->IsA("Decal") && property == "Scale") lua_pushnumber(state, static_cast<Decal*>(instance.get())->Scale());
    else if (instance->IsA("Decal") && property == "Transparency") lua_pushnumber(state, static_cast<Decal*>(instance.get())->Transparency());
    else if (instance->IsA("Motor") && property == "Part1") PushInstance(state, static_cast<Motor*>(instance.get())->Part1());
    else if (instance->IsA("Motor") && property == "Part2") PushInstance(state, static_cast<Motor*>(instance.get())->Part2());
    else if (instance->IsA("Motor") && property == "Offset1") PushVector3(state, static_cast<Motor*>(instance.get())->Offset1());
    else if (instance->IsA("Motor") && property == "Offset2") PushVector3(state, static_cast<Motor*>(instance.get())->Offset2());
    else if (instance->IsA("Motor") && property == "CurrentAngle") lua_pushnumber(state, static_cast<Motor*>(instance.get())->CurrentAngle());
    else if (instance->IsA("Motor") && property == "DesiredAngle") lua_pushnumber(state, static_cast<Motor*>(instance.get())->DesiredAngle());
    else if (instance->IsA("Motor") && property == "MaxVelocity") lua_pushnumber(state, static_cast<Motor*>(instance.get())->MaxVelocity());
    else {
        PushMethod(state, property);
        if (!lua_isnil(state, -1)) return 1;
        lua_pop(state, 1);

        if (instance->IsDataModelRoot()) {
            if (LuaBindingContext* context = GetBindingContext(state); context != nullptr && context->dataModel != nullptr) {
                if (const auto service = context->dataModel->GetService(property)) {
                    PushInstance(state, service);
                    return 1;
                }
            }
        }
        PushInstance(state, instance->FindFirstChild(property));
    }
    return 1;
}

int InstanceNewIndex(lua_State* state) {
    const auto* self = TestInstance(state, 1);
    if (self == nullptr || !self->instance || lua_type(state, 2) != LUA_TSTRING) return 0;
    size_t length = 0;
    const char* key = lua_tolstring(state, 2, &length);
    const std::string_view property(key != nullptr ? key : "", length);
    Instance& instance = *self->instance;

    if (property == "Name") {
        std::string name;
        if (ReadString(state, 3, name)) instance.SetName(std::move(name));
    } else if (property == "Parent") {
        if (lua_isnil(state, 3)) {
            [[maybe_unused]] const auto result = instance.SetParent({});
        } else {
            InstancePtr parent;
            if (ReadInstance(state, 3, parent)) [[maybe_unused]] const auto result = instance.SetParent(parent);
        }
    } else if (instance.IsA("BasePart") && property == "Position") {
        JPH::Vec3 vector;
        if (ReadVector3(state, 3, vector)) static_cast<BasePart&>(instance).SetPosition(vector);
    } else if (instance.IsA("BasePart") && property == "Rotation") {
        JPH::Vec3 vector;
        if (ReadVector3(state, 3, vector)) static_cast<BasePart&>(instance).SetRotation(vector);
    } else if (instance.IsA("BasePart") && property == "Size") {
        JPH::Vec3 vector;
        if (ReadVector3(state, 3, vector)) static_cast<BasePart&>(instance).SetSize(vector);
    } else if (instance.IsA("BasePart") && property == "Color") {
        JPH::Vec3 color;
        if (ReadColor3(state, 3, color)) static_cast<BasePart&>(instance).SetColor(color);
    } else if (instance.IsA("BasePart") && property == "Anchored") static_cast<BasePart&>(instance).SetAnchored(lua_toboolean(state, 3) != 0);
    else if (instance.IsA("BasePart") && property == "CanCollide") static_cast<BasePart&>(instance).SetCanCollide(lua_toboolean(state, 3) != 0);
    else if (instance.IsA("BasePart") && property == "Transparency") {
        float number = 0.0f;
        if (ReadNumber(state, 3, number)) static_cast<BasePart&>(instance).SetTransparency(number);
    } else if (instance.IsA("BasePart") && property == "NetworkOwner") {
        InstancePtr owner;
        if (lua_isnil(state, 3)) [[maybe_unused]] const auto result = static_cast<BasePart&>(instance).SetNetworkOwner({});
        else if (ReadInstance(state, 3, owner) && owner->IsA("Player")) {
            [[maybe_unused]] const auto result = static_cast<BasePart&>(instance).SetNetworkOwner(std::static_pointer_cast<Player>(owner));
        }
    } else if (instance.IsA("Part") && property == "Shape") {
        PartShape shape {};
        if (ReadPartShape(state, 3, shape)) static_cast<Part&>(instance).SetShape(shape);
    } else if (instance.IsA("Part") && property == "FrontSurface") {
        PartSurface surface {};
        if (ReadPartSurface(state, 3, surface)) static_cast<Part&>(instance).SetFrontSurface(surface);
    } else if (instance.IsA("Part") && property == "BackSurface") {
        PartSurface surface {};
        if (ReadPartSurface(state, 3, surface)) static_cast<Part&>(instance).SetBackSurface(surface);
    } else if (instance.IsA("Part") && property == "TopSurface") {
        PartSurface surface {};
        if (ReadPartSurface(state, 3, surface)) static_cast<Part&>(instance).SetTopSurface(surface);
    } else if (instance.IsA("Part") && property == "BottomSurface") {
        PartSurface surface {};
        if (ReadPartSurface(state, 3, surface)) static_cast<Part&>(instance).SetBottomSurface(surface);
    } else if (instance.IsA("Part") && property == "LeftSurface") {
        PartSurface surface {};
        if (ReadPartSurface(state, 3, surface)) static_cast<Part&>(instance).SetLeftSurface(surface);
    } else if (instance.IsA("Part") && property == "RightSurface") {
        PartSurface surface {};
        if (ReadPartSurface(state, 3, surface)) static_cast<Part&>(instance).SetRightSurface(surface);
    } else if (instance.IsA("MeshPart") && property == "MeshId") {
        std::string meshId;
        if (ReadString(state, 3, meshId)) static_cast<MeshPart&>(instance).SetMeshId(std::move(meshId));
    } else if (instance.IsA("SpawnPoint") && property == "Position") {
        JPH::Vec3 vector;
        if (ReadVector3(state, 3, vector)) static_cast<SpawnPoint&>(instance).SetPosition(vector);
    } else if (instance.IsA("SpawnPoint") && property == "Rotation") {
        JPH::Vec3 vector;
        if (ReadVector3(state, 3, vector)) static_cast<SpawnPoint&>(instance).SetRotation(vector);
    } else if (instance.IsA("Model") && property == "PrimaryPart") {
        InstancePtr part;
        if (lua_isnil(state, 3)) [[maybe_unused]] const auto result = static_cast<Model&>(instance).SetPrimaryPart({});
        else if (ReadInstance(state, 3, part) && part->IsA("BasePart")) {
            [[maybe_unused]] const auto result = static_cast<Model&>(instance).SetPrimaryPart(std::static_pointer_cast<BasePart>(part));
        }
    } else if (instance.IsA("Humanoid") && property == "WalkSpeed") {
        float number = 0.0f;
        if (ReadNumber(state, 3, number)) static_cast<Humanoid&>(instance).SetWalkSpeed(number);
    } else if (instance.IsA("Humanoid") && property == "JumpPower") {
        float number = 0.0f;
        if (ReadNumber(state, 3, number)) static_cast<Humanoid&>(instance).SetJumpPower(number);
    } else if (instance.IsA("Humanoid") && property == "Health") {
        float number = 0.0f;
        if (ReadNumber(state, 3, number)) static_cast<Humanoid&>(instance).SetHealth(number);
    } else if (instance.IsA("Humanoid") && property == "MaxHealth") {
        float number = 0.0f;
        if (ReadNumber(state, 3, number)) static_cast<Humanoid&>(instance).SetMaxHealth(number);
    } else if (instance.IsA("Humanoid") && property == "RootPart") {
        InstancePtr part;
        if (lua_isnil(state, 3)) [[maybe_unused]] const auto result = static_cast<Humanoid&>(instance).SetRootPart({});
        else if (ReadInstance(state, 3, part) && part->IsA("BasePart")) {
            [[maybe_unused]] const auto result = static_cast<Humanoid&>(instance).SetRootPart(std::static_pointer_cast<BasePart>(part));
        }
    } else if (instance.IsA("Humanoid") && property == "State") {
        HumanoidState humanoidState {};
        if (ReadHumanoidState(state, 3, humanoidState)) static_cast<Humanoid&>(instance).SetState(humanoidState);
    } else if (instance.IsA("Player") && property == "UserId" && lua_isnumber(state, 3)) {
        const double number = lua_tonumber(state, 3);
        if (number >= 0.0) static_cast<Player&>(instance).SetUserId(static_cast<uint64_t>(number));
    } else if (instance.IsA("Player") && property == "Character") {
        InstancePtr character;
        if (lua_isnil(state, 3)) [[maybe_unused]] const auto result = static_cast<Player&>(instance).SetCharacter({});
        else if (ReadInstance(state, 3, character) && character->IsA("Model")) {
            [[maybe_unused]] const auto result = static_cast<Player&>(instance).SetCharacter(std::static_pointer_cast<Model>(character));
        }
    } else if (instance.IsA("PlayersService") && property == "LocalPlayer") {
        InstancePtr player;
        if (lua_isnil(state, 3)) [[maybe_unused]] const auto result = static_cast<PlayersService&>(instance).SetLocalPlayer({});
        else if (ReadInstance(state, 3, player) && player->IsA("Player")) {
            [[maybe_unused]] const auto result = static_cast<PlayersService&>(instance).SetLocalPlayer(std::static_pointer_cast<Player>(player));
        }
    } else if (instance.IsA("WorkspaceService") && property == "CurrentCamera") {
        InstancePtr camera;
        if (lua_isnil(state, 3)) [[maybe_unused]] const auto result = static_cast<WorkspaceService&>(instance).SetCurrentCamera({});
        else if (ReadInstance(state, 3, camera)) [[maybe_unused]] const auto result = static_cast<WorkspaceService&>(instance).SetCurrentCamera(camera);
    } else if (instance.IsA("PhysicsService") && property == "ServerAuthority") {
        static_cast<PhysicsService&>(instance).SetServerAuthority(lua_toboolean(state, 3) != 0);
    } else if (instance.IsA("Sound") && property == "SoundId") {
        std::string soundId;
        if (ReadString(state, 3, soundId)) static_cast<Sound&>(instance).SetSoundId(std::move(soundId));
    } else if (instance.IsA("Sound") && property == "Volume") {
        float number = 0.0f;
        if (ReadNumber(state, 3, number)) static_cast<Sound&>(instance).SetVolume(number);
    } else if (instance.IsA("Sound") && property == "Loops") static_cast<Sound&>(instance).SetLoops(lua_toboolean(state, 3) != 0);
    else if (instance.IsA("Sound") && property == "Playing") static_cast<Sound&>(instance).SetPlaying(lua_toboolean(state, 3) != 0);
    else if (instance.IsA("Decal") && property == "TextureId") {
        std::string textureId;
        if (ReadString(state, 3, textureId)) static_cast<Decal&>(instance).SetTextureId(std::move(textureId));
    } else if (instance.IsA("Decal") && property == "Color") {
        JPH::Vec3 color;
        if (ReadColor3(state, 3, color)) static_cast<Decal&>(instance).SetColor(color);
    } else if (instance.IsA("Decal") && property == "Face") {
        DecalFace face {};
        if (ReadDecalFace(state, 3, face)) static_cast<Decal&>(instance).SetFace(face);
    } else if (instance.IsA("Decal") && property == "WrapMode") {
        DecalWrapMode wrapMode {};
        if (ReadDecalWrapMode(state, 3, wrapMode)) static_cast<Decal&>(instance).SetWrapMode(wrapMode);
    } else if (instance.IsA("Decal") && property == "Scale") {
        float number = 0.0f;
        if (ReadNumber(state, 3, number)) static_cast<Decal&>(instance).SetScale(number);
    } else if (instance.IsA("Decal") && property == "Transparency") {
        float number = 0.0f;
        if (ReadNumber(state, 3, number)) static_cast<Decal&>(instance).SetTransparency(number);
    } else if (instance.IsA("Motor") && property == "Part1") {
        InstancePtr part;
        if (lua_isnil(state, 3)) [[maybe_unused]] const auto result = static_cast<Motor&>(instance).SetPart1({});
        else if (ReadInstance(state, 3, part) && part->IsA("BasePart")) {
            [[maybe_unused]] const auto result = static_cast<Motor&>(instance).SetPart1(std::static_pointer_cast<BasePart>(part));
        }
    } else if (instance.IsA("Motor") && property == "Part2") {
        InstancePtr part;
        if (lua_isnil(state, 3)) [[maybe_unused]] const auto result = static_cast<Motor&>(instance).SetPart2({});
        else if (ReadInstance(state, 3, part) && part->IsA("BasePart")) {
            [[maybe_unused]] const auto result = static_cast<Motor&>(instance).SetPart2(std::static_pointer_cast<BasePart>(part));
        }
    } else if (instance.IsA("Motor") && property == "Offset1") {
        JPH::Vec3 vector;
        if (ReadVector3(state, 3, vector)) static_cast<Motor&>(instance).SetOffset1(vector);
    } else if (instance.IsA("Motor") && property == "Offset2") {
        JPH::Vec3 vector;
        if (ReadVector3(state, 3, vector)) static_cast<Motor&>(instance).SetOffset2(vector);
    } else if (instance.IsA("Motor") && property == "CurrentAngle") {
        float number = 0.0f;
        if (ReadNumber(state, 3, number)) static_cast<Motor&>(instance).SetCurrentAngle(number);
    } else if (instance.IsA("Motor") && property == "DesiredAngle") {
        float number = 0.0f;
        if (ReadNumber(state, 3, number)) static_cast<Motor&>(instance).SetDesiredAngle(number);
    } else if (instance.IsA("Motor") && property == "MaxVelocity") {
        float number = 0.0f;
        if (ReadNumber(state, 3, number)) static_cast<Motor&>(instance).SetMaxVelocity(number);
    }
    return 0;
}

int InstanceNew(lua_State* state) {
    LuaBindingContext* context = GetBindingContext(state);
    if (context == nullptr || context->dataModel == nullptr || lua_type(state, 1) != LUA_TSTRING) {
        lua_pushnil(state);
        return 1;
    }

    size_t classLength = 0;
    const char* classNameData = lua_tolstring(state, 1, &classLength);
    const std::string_view className(classNameData != nullptr ? classNameData : "", classLength);
    if (className.empty() || className == "DataModel") {
        lua_pushnil(state);
        return 1;
    }

    std::string name;
    int parentIndex = 2;
    if (lua_type(state, 2) == LUA_TSTRING && (lua_isnoneornil(state, 3))) {
        if (!ReadString(state, 2, name)) {
            lua_pushnil(state);
            return 1;
        }
        parentIndex = 0;
    } else if (lua_type(state, 3) == LUA_TSTRING) {
        if (!ReadString(state, 3, name)) {
            lua_pushnil(state);
            return 1;
        }
    }

    auto created = context->dataModel->CreateInstance(className, std::move(name));
    if (!created) {
        lua_pushnil(state);
        return 1;
    }
    InstancePtr instance = std::move(created.value());

    if (parentIndex != 0 && !lua_isnoneornil(state, parentIndex)) {
        InstancePtr parent;
        if (!ReadInstance(state, parentIndex, parent) || !instance->SetParent(parent)) {
            instance->Destroy();
            lua_pushnil(state);
            return 1;
        }
    }

    PushInstance(state, instance);
    return 1;
}

template <typename Signal, typename PushArguments>
void ConnectSignal(lua_State* state, LuaBindingContext& context, Signal& signal, int callbackIndex, PushArguments pushArguments);

int SignalConnect(lua_State* state) {
    if (lua_type(state, 2) != LUA_TFUNCTION) {
        lua_pushnil(state);
        return 1;
    }
    const auto* signal = TestUserdata<LuaSignal>(state, 1, kSignalMetatable);
    LuaBindingContext* context = GetBindingContext(state);
    if (signal == nullptr || !signal->owner || context == nullptr || !context->alive) {
        lua_pushnil(state);
        return 1;
    }

    const auto owner = signal->owner;
    switch (signal->kind) {
    case SignalKind::ChildAdded:
        ConnectSignal(state, *context, owner->ChildAdded, 2,
            [](lua_State* target, const InstancePtr& child) { PushInstance(target, child); });
        return 1;
    case SignalKind::ChildRemoved:
        ConnectSignal(state, *context, owner->ChildRemoved, 2,
            [](lua_State* target, const InstancePtr& child) { PushInstance(target, child); });
        return 1;
    case SignalKind::DescendantAdded:
        ConnectSignal(state, *context, owner->DescendantAdded, 2,
            [](lua_State* target, const InstancePtr& child) { PushInstance(target, child); });
        return 1;
    case SignalKind::DescendantRemoved:
        ConnectSignal(state, *context, owner->DescendantRemoved, 2,
            [](lua_State* target, const InstancePtr& child) { PushInstance(target, child); });
        return 1;
    case SignalKind::Destroying:
        ConnectSignal(state, *context, owner->Destroying, 2,
            [](lua_State* target, const InstancePtr& item) { PushInstance(target, item); });
        return 1;
    case SignalKind::AncestryChanged:
        ConnectSignal(state, *context, owner->AncestryChanged, 2,
            [](lua_State* target, const InstancePtr& item, const InstancePtr& parent) {
                PushInstance(target, item);
                PushInstance(target, parent);
            });
        return 1;
    case SignalKind::PropertyChanged:
        ConnectSignal(state, *context, owner->PropertyChanged, 2,
            [](lua_State* target, const std::string& property) { lua_pushlstring(target, property.data(), property.size()); });
        return 1;
    case SignalKind::HumanoidStateChanged:
        if (owner->IsA("Humanoid")) {
            ConnectSignal(state, *context, static_cast<Humanoid*>(owner.get())->StateChanged, 2,
                [](lua_State* target, HumanoidState oldState, HumanoidState newState) {
                    PushHumanoidState(target, oldState);
                    PushHumanoidState(target, newState);
                });
            return 1;
        }
        break;
    case SignalKind::SoundEnded:
        if (owner->IsA("Sound")) {
            ConnectSignal(state, *context, static_cast<Sound*>(owner.get())->Ended, 2,
                [](lua_State*) {});
            return 1;
        }
        break;
    }
    lua_pushnil(state);
    return 1;
}

int SignalIndex(lua_State* state) {
    if (lua_type(state, 2) == LUA_TSTRING) {
        size_t length = 0;
        const char* key = lua_tolstring(state, 2, &length);
        const std::string_view property(key != nullptr ? key : "", length);
        if (property == "Connect") {
            lua_pushcfunction(state, SignalConnect);
            return 1;
        }
    }
    lua_pushnil(state);
    return 1;
}

int ConnectionDisconnect(lua_State* state) {
    auto* value = TestUserdata<LuaConnectionUserdata>(state, 1, kConnectionMetatable);
    if (value != nullptr && value->connection) value->connection->Disconnect();
    return 0;
}

int ConnectionIndex(lua_State* state) {
    const auto* value = TestUserdata<LuaConnectionUserdata>(state, 1, kConnectionMetatable);
    if (value == nullptr || !value->connection || lua_type(state, 2) != LUA_TSTRING) {
        lua_pushnil(state);
        return 1;
    }
    size_t length = 0;
    const char* key = lua_tolstring(state, 2, &length);
    const std::string_view property(key != nullptr ? key : "", length);
    if (property == "Connected") lua_pushboolean(state, value->connection->connected);
    else if (property == "Disconnect") lua_pushcfunction(state, ConnectionDisconnect);
    else lua_pushnil(state);
    return 1;
}

template <typename Connection>
void InvokeLuaConnection(const std::weak_ptr<LuaConnectionRecord>& weakConnection, auto& pushArguments, auto&&... arguments) {
    const auto connection = weakConnection.lock();
    if (!connection || !connection->connected || connection->state == nullptr || connection->callbackRef == LUA_NOREF) return;
    lua_State* state = connection->state;
    const int originalTop = lua_gettop(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, connection->callbackRef);
    pushArguments(state, std::forward<decltype(arguments)>(arguments)...);
    if (lua_pcall(state, static_cast<int>(sizeof...(arguments)), 0, 0) != LUA_OK) {
        const char* error = lua_tostring(state, -1);
        if (error != nullptr) LogWarning("[ProjectLight] Lua DataModel event callback failed: {}", error);
    }
    lua_settop(state, originalTop);
}

template <typename Signal, typename PushArguments>
void ConnectSignal(lua_State* state, LuaBindingContext& context, Signal& signal, int callbackIndex, PushArguments pushArguments) {
    lua_pushvalue(state, callbackIndex);
    const int callbackRef = luaL_ref(state, LUA_REGISTRYINDEX);

    auto connection = std::make_shared<LuaConnectionRecord>();
    connection->state = state;
    connection->callbackRef = callbackRef;

    using Token = typename Signal::Connection;
    auto token = std::make_shared<Token>();
    const std::weak_ptr<LuaConnectionRecord> weakConnection = connection;
    *token = signal.Connect([weakConnection, pushArguments = std::move(pushArguments)](auto&&... arguments) mutable {
        InvokeLuaConnection<Token>(weakConnection, pushArguments, std::forward<decltype(arguments)>(arguments)...);
    });
    connection->disconnect = [token] { token->Disconnect(); };

    std::erase_if(context.connections, [](const auto& current) { return !current || !current->connected; });
    context.connections.push_back(connection);
    PushConnection(state, std::move(connection));
}

int Vector3NewIndex(lua_State*) {
    return 0;
}

int Color3NewIndex(lua_State*) {
    return 0;
}

void RegisterMetatables(lua_State* state) {
    luaL_newmetatable(state, kInstanceMetatable);
    lua_pushcfunction(state, InstanceIndex);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, InstanceNewIndex);
    lua_setfield(state, -2, "__newindex");
    lua_pushcfunction(state, InstanceGc);
    lua_setfield(state, -2, "__gc");
    lua_pushcfunction(state, InstanceToString);
    lua_setfield(state, -2, "__tostring");
    lua_pushcfunction(state, InstanceEqual);
    lua_setfield(state, -2, "__eq");
    lua_pop(state, 1);

    luaL_newmetatable(state, kVector3Metatable);
    lua_pushcfunction(state, Vector3Index);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, Vector3NewIndex);
    lua_setfield(state, -2, "__newindex");
    lua_pushcfunction(state, Vector3Gc);
    lua_setfield(state, -2, "__gc");
    lua_pushcfunction(state, Vector3Add);
    lua_setfield(state, -2, "__add");
    lua_pushcfunction(state, Vector3Sub);
    lua_setfield(state, -2, "__sub");
    lua_pushcfunction(state, Vector3Unm);
    lua_setfield(state, -2, "__unm");
    lua_pushcfunction(state, Vector3Mul);
    lua_setfield(state, -2, "__mul");
    lua_pushcfunction(state, Vector3Div);
    lua_setfield(state, -2, "__div");
    lua_pushcfunction(state, Vector3Equal);
    lua_setfield(state, -2, "__eq");
    lua_pop(state, 1);

    luaL_newmetatable(state, kColor3Metatable);
    lua_pushcfunction(state, Color3Index);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, Color3NewIndex);
    lua_setfield(state, -2, "__newindex");
    lua_pushcfunction(state, Color3Gc);
    lua_setfield(state, -2, "__gc");
    lua_pushcfunction(state, Color3Equal);
    lua_setfield(state, -2, "__eq");
    lua_pop(state, 1);

    luaL_newmetatable(state, kSignalMetatable);
    lua_pushcfunction(state, SignalIndex);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, SignalGc);
    lua_setfield(state, -2, "__gc");
    lua_pop(state, 1);

    luaL_newmetatable(state, kConnectionMetatable);
    lua_pushcfunction(state, ConnectionIndex);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, ConnectionGc);
    lua_setfield(state, -2, "__gc");
    lua_pop(state, 1);

    luaL_newmetatable(state, kContextMetatable);
    lua_pushcfunction(state, ContextGc);
    lua_setfield(state, -2, "__gc");
    lua_pop(state, 1);
}

void RegisterVector3(lua_State* state) {
    lua_newtable(state);
    lua_pushcfunction(state, Vector3New);
    lua_setfield(state, -2, "new");
    PushVector3(state, 0.0f, 0.0f, 0.0f);
    lua_setfield(state, -2, "zero");
    PushVector3(state, 1.0f, 1.0f, 1.0f);
    lua_setfield(state, -2, "one");
    PushVector3(state, 1.0f, 0.0f, 0.0f);
    lua_setfield(state, -2, "xAxis");
    PushVector3(state, 0.0f, 1.0f, 0.0f);
    lua_setfield(state, -2, "yAxis");
    PushVector3(state, 0.0f, 0.0f, 1.0f);
    lua_setfield(state, -2, "zAxis");
    lua_setglobal(state, "Vector3");
}

void RegisterColor3(lua_State* state) {
    lua_newtable(state);
    lua_pushcfunction(state, Color3New);
    lua_setfield(state, -2, "new");
    lua_pushcfunction(state, Color3FromRGB);
    lua_setfield(state, -2, "fromRGB");
    lua_setglobal(state, "Color3");
}

template <typename E>
void PushEnumTable(lua_State* state, const char* enumName) {
    lua_newtable(state);
    constexpr auto names = ZHLN::Reflect::EnumNames<E>();
    for (const std::string_view name: names) {
        const auto value = ZHLN::Reflect::StringToEnum<E>(name);
        if (!value) continue;
        const std::string_view canonicalName = CanonicalEnumName(*value);
        lua_pushlstring(state, name.data(), name.size());
        lua_pushlstring(state, canonicalName.data(), canonicalName.size());
        lua_settable(state, -3);
    }
    lua_setfield(state, -2, enumName);
}

void RegisterEnums(lua_State* state) {
    lua_newtable(state);
    PushEnumTable<PartShape>(state, "PartShape");
    PushEnumTable<HumanoidState>(state, "HumanoidState");
    PushEnumTable<PartSurface>(state, "PartSurface");
    PushEnumTable<DecalFace>(state, "DecalFace");
    PushEnumTable<DecalWrapMode>(state, "DecalWrapMode");
    lua_setglobal(state, "Enum");
}

void RegisterInstanceLibrary(lua_State* state) {
    lua_newtable(state);
    lua_pushcfunction(state, InstanceNew);
    lua_setfield(state, -2, "new");
    lua_setglobal(state, "Instance");
}

} // namespace

void RegisterDataModelLuaBindings(lua_State* state, DataModel& dataModel) {
    if (state == nullptr) return;
    if (GetBindingContext(state) != nullptr) return;

    RegisterMetatables(state);

    lua_pushlightuserdata(state, &kInstanceCacheRegistryKey);
    lua_newtable(state);
    lua_newtable(state);
    lua_pushliteral(state, "v");
    lua_setfield(state, -2, "__mode");
    lua_setmetatable(state, -2);
    lua_rawset(state, LUA_REGISTRYINDEX);

    auto* contextBox = static_cast<LuaContextUserdata*>(lua_newuserdata(state, sizeof(LuaContextUserdata)));
    auto* context = new LuaBindingContext {};
    context->state = state;
    context->dataModel = &dataModel;
    new (contextBox) LuaContextUserdata {.context = context};
    luaL_getmetatable(state, kContextMetatable);
    lua_setmetatable(state, -2);
    lua_pushlightuserdata(state, &kBindingContextRegistryKey);
    lua_pushvalue(state, -2);
    lua_rawset(state, LUA_REGISTRYINDEX);
    lua_pop(state, 1);

    RegisterVector3(state);
    RegisterColor3(state);
    RegisterEnums(state);
    RegisterInstanceLibrary(state);

    PushInstance(state, dataModel.Root());
    lua_setglobal(state, "game");
    if (const auto workspace = dataModel.GetService("Workspace")) {
        PushInstance(state, workspace);
        lua_setglobal(state, "workspace");
    }
}

} // namespace ZHLN::ProjectLight
