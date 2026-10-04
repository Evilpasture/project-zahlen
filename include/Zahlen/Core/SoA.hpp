// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Math.hpp>
#include <Zahlen/Core/Reflection/Core.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/Log.hpp>
#include <cstddef>
#include <string_view>
#include <type_traits>

namespace ZHLN {

#if ZHLN_REFLECTION_AVAILABLE

// Structure-of-arrays storage over a borrowed block of memory.
//
// Where a `std::vector<T>` keeps one element's fields together, an SoA block
// keeps one *field* together: `GetStreams()` is a synthesized analogue of T whose
// members are pointers to T's fields' arrays, so a pass that only touches
// positions (integration, culling, a broadphase upload) walks one dense array
// instead of striding over every field of every element. It is the same layout
// the GPU side already wants, which is why building it from reflection rather
// than by hand matters: a component that grows a field gets a stream for it
// without an edit here.
//
// The block does not own the memory it binds to. That separation is what lets the
// same layout sit in a frame-scratch arena (see SoAScratch) or in persistent
// storage without either one needing a different type.
//
// One consequence of the streams being *defined* by instantiation: C++ forbids
// defining a class in a function parameter or return type, so a member lookup
// through this type in a function signature -- `SoABlock<T>::ComputeByteSize(n)`
// written in a parameter list, which is also how a vexing parse reads -- is
// ill-formed. Lookups belong in a body (or a braced initializer there), where
// the definition has a scope that may hold one.
template <typename T>
class SoABlock {
    static_assert(Reflect::IsStreamableStruct<T>(),
                  "SoABlock needs an aggregate whose data members all carry identifiers and none of them is a reference; a member that "
                  "cannot name a stream cannot be laid out as one");

  public:
    // The two shapes of T this layout needs. `Streams` is the storage struct a
    // block binds its arrays into; `ProxyRef` is what operator[] splices out of
    // it -- the same fields in the same order, as references into the streams.
    // A third shape (const pointers, say) is a `Reflect::TransformedStruct<T,
    // ...>` away at any use site; nothing here depends on there being only two.
    using Streams  = Reflect::TransformedStruct<T, std::add_pointer_t>;
    using ProxyRef = Reflect::TransformedStruct<T, std::add_lvalue_reference_t>;

    // Every stream starts on a cache line, so a field written by one thread
    // cannot share a line with a field written by another. It costs up to one
    // line of padding per field; for a component of a few floats that is the
    // difference between streams that scale across workers and streams that
    // false-share, which is the trade this layout is for.
    static constexpr size_t StreamAlignment = CacheLineSize;

    SoABlock() = default;

    // Bytes Bind() consumes for `capacity` elements: each stream aligned up to a
    // line, then `capacity` elements of the field's type. This is the number an
    // arena allocation is sized with, so it is the authority on the layout --
    // Bind() walks the same loop with the same rule rather than recomputing it.
    static constexpr auto ComputeByteSize(size_t capacity) noexcept -> size_t {
        size_t total = 0;
        Reflect::ForEachFieldInfo<T>([&]<typename FieldType>(std::string_view, size_t) {
            total = Math::AlignUp(total, StreamAlignment);
            total += sizeof(FieldType) * capacity;
        });
        return total;
    }

    // Binds `capacity` elements' worth of parallel streams into `memory`, which
    // must be at least ComputeByteSize(capacity) bytes and stay alive as long as
    // this block does. The memory is not adopted: whatever owns it keeps owning
    // it, and the block is a view onto it.
    static auto Bind(void* memory, size_t capacity) noexcept -> SoABlock<T> {
        Assert(memory != nullptr, "SoABlock cannot bind to null memory");

        SoABlock<T> block;
        block._capacity = capacity;

        auto*  raw    = static_cast<std::byte*>(memory);
        size_t offset = 0;
        Reflect::ForEachFieldInfo<T>([&]<typename FieldType>(std::string_view name, size_t) {
            offset        = Math::AlignUp(offset, StreamAlignment);
            auto*  stream = reinterpret_cast<FieldType*>(raw + offset);
            // VisitFieldByName instantiates its callback for every member of the
            // stream struct, so the assignment has to be guarded on the member's
            // type rather than assumed: the transform of T and T itself are two
            // different types, and only the member whose type is `FieldType*` is
            // the stream this call is about.
            const bool bound = Reflect::VisitFieldByName(block._streams, name, [stream](auto& field) {
                using StreamType = std::remove_reference_t<decltype(field)>;
                if constexpr (std::is_same_v<StreamType, FieldType*>) {
                    field = stream;
                }
            });
            Assert(bound, "SoABlock could not bind a reflected field to its stream");
            offset += sizeof(FieldType) * capacity;
        });
        return block;
    }

    // One element, as a struct of references into the streams. The proxy is a
    // value with reference members: writing through it writes the streams,
    // copying it copies the references, and taking one costs one load per field.
    [[nodiscard]] auto operator[](size_t index) noexcept -> ProxyRef {
        Assert(index < _capacity, "SoABlock index out of bounds");
        return Reflect::MapConstruct<ProxyRef>(_streams, [index](auto* stream) -> auto& { return stream[index]; });
    }

    // One element copied out as a T. The proxy above is for touching a field or
    // two; this is for treating an element as the component it came from.
    [[nodiscard]] auto Get(size_t index) const noexcept -> T {
        Assert(index < _capacity, "SoABlock index out of bounds");
        return Reflect::MapConstruct<T>(_streams, [index](auto* stream) -> const auto& { return stream[index]; });
    }

    // One element written from a T, field by field into the stream that carries
    // each name. This is the write operator[] cannot express: a proxy whose
    // members are references has no assignment from T, so a block that could
    // only hand out proxies could be sprouted but not seeded.
    void Set(size_t index, const T& value) noexcept {
        Assert(index < _capacity, "SoABlock index out of bounds");
        Reflect::ForEachFieldWithName(value, [&](std::string_view name, const auto& field) {
            using FieldType = std::remove_cvref_t<decltype(field)>;
            // Same guard as Bind, from the other side: the callback is
            // instantiated for every stream, and only the one whose element type
            // is this field's takes the write.
            const bool stored = Reflect::VisitFieldByName(_streams, name, [&](auto& stream) {
                using StreamType = std::remove_reference_t<decltype(stream)>;
                if constexpr (std::is_same_v<StreamType, FieldType*>) {
                    stream[index] = field;
                }
            });
            Assert(stored, "SoABlock could not find the stream for a reflected field");
        });
    }

    // The alias above owns the name `Streams`, so the accessor cannot be spelled
    // the same way: a member alias and a member function of one name are a
    // redeclaration, not an overload.
    [[nodiscard]] auto GetStreams() noexcept -> Streams& {
        return _streams;
    }

    [[nodiscard]] auto GetStreams() const noexcept -> const Streams& {
        return _streams;
    }

    [[nodiscard]] auto Capacity() const noexcept -> size_t {
        return _capacity;
    }

  private:
    Streams _streams {};
    size_t  _capacity = 0;
};

#else

// Without P2996 the stream structs cannot be synthesized at all: there is no
// member list to transform, and the transpiler fallback only rewrites the named
// field-walking helpers, not aggregate definition. Rather than publish a block
// that silently moves nothing, this stands in and fails where a caller
// instantiates it -- the same shape SystemGraph's resolver table uses for a
// parameter it does not recognize.
template <typename T>
class SoABlock {
    static_assert(!std::is_same_v<T, T>, "SoABlock requires C++26 static reflection (-freflection); this build has none");
};

#endif

} // namespace ZHLN
