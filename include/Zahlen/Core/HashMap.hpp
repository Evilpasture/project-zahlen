// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace ZHLN {

template <typename Key, typename Value, size_t InitialCapacity = 32>
class HashMap {
    static_assert((InitialCapacity & (InitialCapacity - 1)) == 0, "InitialCapacity must be a power of two!");

  public:
    HashMap(): _capacity(InitialCapacity) {
        AllocateStorage();
    }

    ~HashMap() {
        ClearAndFree();
    }

    HashMap(const HashMap&)                    = delete;
    auto operator=(const HashMap&) -> HashMap& = delete;

    HashMap(HashMap&& other) noexcept:
        _states(other._states), _keys(other._keys), _values(other._values), _capacity(other._capacity), _size(other._size), _tombstones(other._tombstones) {
        other._states     = nullptr;
        other._keys       = nullptr;
        other._values     = nullptr;
        other._capacity   = 0;
        other._size       = 0;
        other._tombstones = 0;
    }

    auto operator=(HashMap&& other) noexcept -> HashMap& {
        if (this != &other) {
            ClearAndFree();

            _states     = other._states;
            _keys       = other._keys;
            _values     = other._values;
            _capacity   = other._capacity;
            _size       = other._size;
            _tombstones = other._tombstones;

            other._states     = nullptr;
            other._keys       = nullptr;
            other._values     = nullptr;
            other._capacity   = 0;
            other._size       = 0;
            other._tombstones = 0;
        }
        return *this;
    }

    void Insert(const Key& key, const Value& value) {
        // Factor both active entries and tombstones into load factor calculation
        if ((_size + _tombstones) * 2 >= _capacity) {
            // If table has many tombstones but few active items, rehash in-place to purge tombstones
            size_t new_capacity = (_size * 4 <= _capacity && _capacity >= InitialCapacity) ? _capacity : _capacity * 2;
            Resize(new_capacity);
        }

        const size_t mask      = _capacity - 1;
        size_t       idx       = Hash(key) & mask;
        auto         firstTomb = static_cast<size_t>(-1);

        while (_states[idx] != 0) {
            if (_states[idx] == 1) {
                if (_keys[idx] == key) {
                    _values[idx] = value;
                    return;
                }
            } else if (_states[idx] == 2 && firstTomb == static_cast<size_t>(-1)) {
                firstTomb = idx;
            }
            idx = (idx + 1) & mask;
        }

        // Reuse first tombstone slot if encountered, otherwise use empty slot
        size_t insertIdx = (firstTomb != static_cast<size_t>(-1)) ? firstTomb : idx;

        if (_states[insertIdx] == 2) {
            _tombstones--;
        }
        _states[insertIdx] = 1;
        ::new (static_cast<void*>(&_keys[insertIdx])) Key(key);
        ::new (static_cast<void*>(&_values[insertIdx])) Value(value);
        _size++;
    }

    [[nodiscard]] auto Find(const Key& key) const noexcept -> const Value* {
        if (_capacity == 0 || _size == 0) {
            return nullptr;
        }

        const size_t mask = _capacity - 1;
        size_t       idx  = Hash(key) & mask;

        while (_states[idx] != 0) {
            if (_states[idx] == 1 && _keys[idx] == key) {
                return &_values[idx];
            }
            idx = (idx + 1) & mask;
        }
        return nullptr;
    }

    [[nodiscard]] auto Find(const Key& key) noexcept -> Value* {
        return const_cast<Value*>(std::as_const(*this).Find(key));
    }

    /**
     * @brief O(1) Tombstone Erasure.
     */
    auto Erase(const Key& key) noexcept -> bool {
        if (_capacity == 0 || _size == 0) {
            return false;
        }

        const size_t mask = _capacity - 1;
        size_t       idx  = Hash(key) & mask;

        while (_states[idx] != 0) {
            if (_states[idx] == 1 && _keys[idx] == key) {
                _keys[idx].~Key();
                _values[idx].~Value();
                _states[idx] = 2; // Tombstone / Deleted
                _size--;
                _tombstones++;
                return true;
            }
            idx = (idx + 1) & mask;
        }
        return false;
    }

    void Clear() noexcept {
        if (_states == nullptr) {
            return;
        }

        for (size_t i = 0; i < _capacity; ++i) {
            if (_states[i] == 1) {
                _keys[i].~Key();
                _values[i].~Value();
            }
            _states[i] = 0;
        }
        _size       = 0;
        _tombstones = 0;
    }

    [[nodiscard]] auto Size() const noexcept -> size_t {
        return _size;
    }
    [[nodiscard]] auto Capacity() const noexcept -> size_t {
        return _capacity;
    }

    template <typename Func>
    void ForEach(Func&& func) const {
        if (_states == nullptr) {
            return;
        }
        for (size_t i = 0; i < _capacity; ++i) {
            if (_states[i] == 1) {
                std::forward<Func>(func)(_keys[i], _values[i]);
            }
        }
    }

    template <typename Func>
    void ForEach(Func&& func) {
        if (_states == nullptr) {
            return;
        }
        for (size_t i = 0; i < _capacity; ++i) {
            if (_states[i] == 1) {
                std::forward<Func>(func)(_keys[i], _values[i]);
            }
        }
    }

  private:
    void AllocateStorage() {
        _states = new uint8_t[_capacity](); // 0 = Empty, 1 = Active, 2 = Tombstone
        _keys   = static_cast<Key*>(::operator new[](_capacity * sizeof(Key)));
        _values = static_cast<Value*>(::operator new[](_capacity * sizeof(Value)));
    }

    void ClearAndFree() {
        if (_states != nullptr) {
            Clear();
            delete[] _states;
            ::operator delete[](_keys);
            ::operator delete[](_values);
        }
    }

    void Resize(size_t new_capacity) {
        uint8_t* old_states   = _states;
        Key*     old_keys     = _keys;
        Value*   old_values   = _values;
        size_t   old_capacity = _capacity;

        _capacity = new_capacity;
        AllocateStorage(); // Fresh zeroed status array automatically purges tombstones
        _size       = 0;
        _tombstones = 0;

        for (size_t i = 0; i < old_capacity; ++i) {
            if (old_states[i] == 1) {
                Insert(old_keys[i], old_values[i]);
                old_keys[i].~Key();
                old_values[i].~Value();
            }
        }

        delete[] old_states;
        ::operator delete[](old_keys);
        ::operator delete[](old_values);
    }

    [[nodiscard]] static constexpr auto HashRawBytes(const char* str, size_t length) noexcept -> size_t {
#if INTPTR_MAX == INT64_MAX
        constexpr size_t FNV_prime        = 1099511628211ULL;
        constexpr size_t FNV_offset_basis = 14695981039346656037ULL;
#else
        constexpr size_t FNV_prime        = 16777619U;
        constexpr size_t FNV_offset_basis = 2166136261U;
#endif
        size_t hash = FNV_offset_basis;
        for (size_t i = 0; i < length; ++i) {
            hash ^= static_cast<size_t>(static_cast<uint8_t>(str[i]));
            hash *= FNV_prime;
        }
        return hash;
    }

    [[nodiscard]] auto Hash(const Key& key) const noexcept -> size_t {
        if constexpr (sizeof(Key) <= 8 && (std::is_integral_v<Key> || std::is_enum_v<Key> || std::is_pointer_v<Key>) ) {
            uint64_t val = 0;
            if constexpr (std::is_pointer_v<Key>) {
                val = std::bit_cast<uint64_t>(key);
            } else {
                val = static_cast<uint64_t>(key);
            }
            uint64_t scrambled = val * 11400714819323198485ULL;
            return static_cast<size_t>(scrambled >> (64 - std::countr_zero(_capacity)));
        } else if constexpr (requires {
                                 key.data();
                                 key.length();
                             }) {
            const char* data_ptr = reinterpret_cast<const char*>(key.data());
            return HashRawBytes(data_ptr, key.length() * sizeof(*key.data()));
        } else {
            static_assert(sizeof(Key) == 0, "Unsupported Key type!");
            return 0;
        }
    }

    uint8_t* _states     = nullptr; // 0 = Empty, 1 = Active, 2 = Tombstone
    Key*     _keys       = nullptr;
    Value*   _values     = nullptr;
    size_t   _capacity   = 0;
    size_t   _size       = 0;
    size_t   _tombstones = 0;
};

} // namespace ZHLN
