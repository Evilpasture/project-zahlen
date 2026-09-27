// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <Zahlen/Core/Atomic.hpp>
#include <new>
#include <utility>

#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace ZHLN {

template <typename T, size_t BlockCount = 1024>
class ObjectPool {
    static_assert(BlockCount > 0, "BlockCount must be greater than 0");

    static constexpr size_t ObjectSize = std::max(sizeof(T), sizeof(void*));
    static constexpr size_t Alignment  = std::max(alignof(T), alignof(void*));

  public:
    struct Spinlock {
        ZHLN::Atomic<bool> locked {false};

        void lock() noexcept {
            bool expected = false;
            while (!locked.compare_exchange_weak(expected, true, std::memory_order::acquire, std::memory_order::relaxed)) {
                expected = false;
#if defined(__x86_64__) || defined(_M_X64)
                _mm_pause();
#elif defined(__aarch64__)
                __asm__ __volatile__("yield" ::: "memory");
#else
                std::this_thread::yield();
#endif
            }
        }

        void unlock() noexcept {
            locked.store(false, std::memory_order::release);
        }
    };

  private:
    struct Node {
        Node* next;
    };

    struct Chunk {
        alignas(Alignment) std::array<std::byte, ObjectSize * BlockCount> storage;
        Chunk* next = nullptr;
    };

  public:
    ObjectPool() = default;

    ~ObjectPool() noexcept {
        Chunk* curr = _chunks;
        while (curr != nullptr) {
            Chunk* next = curr->next;
            curr->~Chunk();
            ::operator delete(curr, std::align_val_t {alignof(Chunk)});
            curr = next;
        }
    }

    ObjectPool(const ObjectPool&)                    = delete;
    auto operator=(const ObjectPool&) -> ObjectPool& = delete;

    ObjectPool(ObjectPool&& other) noexcept: _freeList(std::exchange(other._freeList, nullptr)), _chunks(std::exchange(other._chunks, nullptr)) {
    }

    auto operator=(ObjectPool&& other) noexcept -> ObjectPool& {
        if (this != &other) {
            this->~ObjectPool();
            _freeList = std::exchange(other._freeList, nullptr);
            _chunks   = std::exchange(other._chunks, nullptr);
        }
        return *this;
    }

    template <typename... Args>
    [[nodiscard]] T* Create(Args&&... args) {
        void* mem = Allocate();
        return ::new (mem) T(std::forward<Args>(args)...);
    }

    void Destroy(T* ptr) noexcept {
        if (ptr == nullptr) {
            return;
        }
        ptr->~T();
        Deallocate(ptr);
    }

    [[nodiscard]] void* Allocate() {
        _lock.lock();
        if (_freeList == nullptr) [[unlikely]] {
            AllocateChunk();
        }

        Node* node = _freeList;
        _freeList  = _freeList->next;
        _lock.unlock();

        return std::bit_cast<void*>(node);
    }

    void Deallocate(void* ptr) noexcept {
        if (ptr == nullptr) {
            return;
        }

        auto* node = std::bit_cast<Node*>(ptr);
        _lock.lock();
        node->next = _freeList;
        _freeList  = node;
        _lock.unlock();
    }

  private:
    void AllocateChunk() {
        void* mem   = ::operator new(sizeof(Chunk), std::align_val_t {alignof(Chunk)});
        auto* chunk = ::new (mem) Chunk();

        chunk->next = _chunks;
        _chunks     = chunk;

        std::byte* start = chunk->storage.data();
        for (size_t i = 0; i < BlockCount; ++i) {
            auto* node = std::bit_cast<Node*>(start + (i * ObjectSize));
            node->next = _freeList;
            _freeList  = node;
        }
    }

    Spinlock _lock;
    Node*    _freeList = nullptr;
    Chunk*   _chunks   = nullptr;
};

}
