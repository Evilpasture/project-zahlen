module;

// System & STL Headers in Global Module Fragment
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

// Zahlen Core Headers
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Core/ControlFlow.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/Loop.hpp>
#include <Zahlen/Core/MemoryPool.hpp>
#include <Zahlen/Core/Pair.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/Prefetch.hpp>
#include <Zahlen/Core/Print.hpp>
#include <Zahlen/Core/Queue.hpp>
#include <Zahlen/Core/RadixSort.hpp>
#include <Zahlen/Core/Ranges.hpp>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Core/SkipList.hpp>
#include <Zahlen/Core/Span.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>

export module zahlen:core;

export namespace ZHLN {
using ZHLN::Array;
using ZHLN::Atomic;
using ZHLN::DefaultAllocator;
using ZHLN::HashMap;
using ZHLN::ObjectPool;
using ZHLN::SkipList;

using ZHLN::FixedString;
using ZHLN::RestrictSpan;
using ZHLN::String128;
using ZHLN::String256;
using ZHLN::String32;
using ZHLN::String64;

using ZHLN::Assert;
using ZHLN::Dump;
using ZHLN::Error;
using ZHLN::ErrorCategory;
using ZHLN::Format;
using ZHLN::Log;
using ZHLN::Panic;
using ZHLN::Print;
using ZHLN::Println;
using ZHLN::Trace;

namespace Reflect {
using ZHLN::Reflect::EnumToString;
using ZHLN::Reflect::FieldCount;
using ZHLN::Reflect::ForEachField;
using ZHLN::Reflect::ForEachFieldWithName;
using ZHLN::Reflect::StringToEnum;
using ZHLN::Reflect::TypeName;
} // namespace Reflect
} // namespace ZHLN
