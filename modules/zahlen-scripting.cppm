module;

#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/JSON.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/ScriptBinder.hpp>
#include <Zahlen/ScriptECSBridge.hpp>
#include <Zahlen/Scripting.h>
#include <Zahlen/Scripting.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <cmath>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

export module zahlen:scripting;

export import :core;
export import :math;
export import :ecs;

export namespace ZHLN {
using ZHLN::BoxedObject;
using ZHLN::JSONError;
using ZHLN::OwnedObject;
using ZHLN::ScriptArray;
using ZHLN::ScriptBinder;
using ZHLN::ScriptClassInfo;
using ZHLN::ScriptECSBridge;
using ZHLN::ScriptError;
using ZHLN::ScriptMethod;
using ZHLN::ScriptProperty;
using ZHLN::ScriptRunner;
using ZHLN::ScriptVal;

namespace ReflectJSON {
using ZHLN::ReflectJSON::Document;
using ZHLN::ReflectJSON::GetJSONValue;
using ZHLN::ReflectJSON::Parse;
using ZHLN::ReflectJSON::ParseObject;
using ZHLN::ReflectJSON::TryParse;
using ZHLN::ReflectJSON::ValueReader;
} // namespace ReflectJSON
} // namespace ZHLN
