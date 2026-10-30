// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Log.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <cstring>
#include <memory>

namespace ZHLN::ECS {

namespace {

ZHLN::Mutex                         s_FamilyMutex {};
uint32_t                            s_TypeCounter = 0;
HashMap<uint32_t, uint32_t>         s_HashToDense;
HashMap<std::string_view, uint32_t> s_NameToFamilyID;

// Keep the old allocation sizes as well as their resource: a memory_resource
// requires the exact size and alignment supplied at allocation on deallocation.
auto ReallocAligned(std::pmr::memory_resource* memory, void* oldPtr, size_t oldSize, size_t newSize, size_t alignment) -> void* {
    void* const newPtr = memory->allocate(newSize, alignment);
    if (oldPtr != nullptr) {
        std::memcpy(newPtr, oldPtr, oldSize);
        memory->deallocate(oldPtr, oldSize, alignment);
    }
    return newPtr;
}

}

auto ComponentFamily::ResolveDenseID(uint32_t typeHash) noexcept -> uint32_t {
    return ZHLN::Lock(s_FamilyMutex, [&] -> uint32_t {
        if (const auto existing = s_HashToDense.Find(typeHash)) {
            return *existing;
        }
        uint32_t id = s_TypeCounter++;
        s_HashToDense.Insert(typeHash, id);
        return id;
    });
}

void Registry::MapNameToFamilyID(std::string_view name, uint32_t id) noexcept {
    ZHLN::Lock(s_FamilyMutex, [&] -> void { s_NameToFamilyID.Insert(name, id); });
}

auto Registry::GetFamilyIDFromName(std::string_view name) noexcept -> uint32_t {
    return ZHLN::Lock(s_FamilyMutex, [&] -> uint32_t {
        return s_NameToFamilyID.Find(name).value_or(0xFFFFFFFFu);
    });
}


SparseSet::SparseSet(size_t elementSize, size_t alignment, BufferSync* syncPtr, DestructorFn destructor):
    SparseSet(elementSize, alignment, syncPtr, destructor, nullptr) {
}

SparseSet::SparseSet(size_t elementSize, size_t alignment, BufferSync* syncPtr, DestructorFn destructor,
                     std::pmr::memory_resource* memory):
    _memory(memory != nullptr ? memory : std::pmr::get_default_resource()),
    _elementSize(elementSize), _alignment(alignment), _sync(syncPtr), _destructor(destructor) {
}

SparseSet::~SparseSet() {
    if (_data != nullptr) {
        for (size_t i = 0; i < _count; ++i) {
            void* component = _data + (i * _elementSize);
            if (_destructor != nullptr) {
                _destructor(component);
            }
        }
    }
    if (_sparse != nullptr) {
        _memory->deallocate(_sparse, _sparseCapacity * sizeof(uint32_t), alignof(uint32_t));
    }
    if (_dense != nullptr) {
        _memory->deallocate(_dense, _denseCapacity * sizeof(Entity), alignof(Entity));
    }
    if (_data != nullptr) {
        _memory->deallocate(_data, _denseCapacity * _elementSize, _alignment);
    }
}

void SparseSet::ResizeSparse(uint32_t required) {
    if ((_sync != nullptr) && _sync->viewExportCount.load(std::memory_order::acquire) > 0) {
        ZHLN::Panic("FFI MEMORY VIOLATION: SparseSet Sparse Resize");
    }

    size_t oldCap   = _sparseCapacity;
    _sparseCapacity = oldCap == 0 ? 1024 : oldCap * 2;
    while (_sparseCapacity <= required) {
        _sparseCapacity *= 2;
    }

    _sparse = static_cast<uint32_t*>(ReallocAligned(_memory, _sparse, oldCap * sizeof(uint32_t), _sparseCapacity * sizeof(uint32_t), alignof(uint32_t)));

    for (size_t i = oldCap; i < _sparseCapacity; ++i) {
        _sparse[i] = INVALID_DENSE;
    }
}

void SparseSet::ResizeDense() {
    if ((_sync != nullptr) && _sync->viewExportCount.load(std::memory_order::acquire) > 0) {
        ZHLN::Panic("FFI MEMORY VIOLATION: SparseSet Dense Resize");
    }

    size_t oldCap  = _denseCapacity;
    _denseCapacity = oldCap == 0 ? 64 : oldCap * 2;

    _dense = static_cast<Entity*>(ReallocAligned(_memory, _dense, oldCap * sizeof(Entity), _denseCapacity * sizeof(Entity), alignof(Entity)));
    _data  = static_cast<std::byte*>(ReallocAligned(_memory, _data, oldCap * _elementSize, _denseCapacity * _elementSize, _alignment));
}

void SparseSet::Insert(Entity entity, const void* data) {
    if (entity.index >= _sparseCapacity) {
        ResizeSparse(entity.index);
    }

    uint32_t denseIdx = _sparse[entity.index];
    if (denseIdx == INVALID_DENSE) {
        if (_count >= _denseCapacity) {
            ResizeDense();
        }
        denseIdx              = static_cast<uint32_t>(_count++);
        _dense[denseIdx]      = entity;
        _sparse[entity.index] = denseIdx;
    } else {
        if (_dense[denseIdx] != entity) {
            ZHLN::Panic("ECS: cannot replace a component through a stale entity handle");
        }
    }
    std::memcpy(_data + (denseIdx * _elementSize), data, _elementSize);
}

auto SparseSet::InsertEmpty(Entity entity) -> void* {
    if (entity.index >= _sparseCapacity) {
        ResizeSparse(entity.index);
    }

    uint32_t denseIdx = _sparse[entity.index];
    if (denseIdx == INVALID_DENSE) {
        if (_count >= _denseCapacity) {
            ResizeDense();
        }
        denseIdx              = static_cast<uint32_t>(_count++);
        _dense[denseIdx]      = entity;
        _sparse[entity.index] = denseIdx;

        std::memset(_data + (denseIdx * _elementSize), 0, _elementSize);
    } else {
        if (_dense[denseIdx] != entity) {
            ZHLN::Panic("ECS: cannot replace a component through a stale entity handle");
        }
        void* component = _data + (denseIdx * _elementSize);
        if (_destructor != nullptr) {
            _destructor(component);
        }
    }
    return _data + (denseIdx * _elementSize);
}

void SparseSet::Remove(Entity entity) {
    if (!Contains(entity)) {
        return;
    }
    uint32_t denseIdx = _sparse[entity.index];
    void* component = _data + (denseIdx * _elementSize);
    if (_destructor != nullptr) {
        _destructor(component);
    }

    uint32_t lastIdx = static_cast<uint32_t>(_count) - 1;
    if (denseIdx != lastIdx) {
        Entity lastEntity = _dense[lastIdx];
        std::memcpy(_data + (denseIdx * _elementSize), _data + (lastIdx * _elementSize), _elementSize);
        _dense[denseIdx]          = lastEntity;
        _sparse[lastEntity.index] = denseIdx;
    }
    _sparse[entity.index] = INVALID_DENSE;
    _count--;
}

auto SparseSet::Contains(Entity entity) const noexcept -> bool {
    if (entity.index >= _sparseCapacity || _sparse[entity.index] == INVALID_DENSE) {
        return false;
    }
    return _dense[_sparse[entity.index]] == entity;
}

auto SparseSet::Get(Entity entity) const noexcept -> void* {
    if (!Contains(entity)) {
        return nullptr;
    }
    return _data + (_sparse[entity.index] * _elementSize);
}

void SparseSet::Clear() noexcept {
    if (_data != nullptr) {
        for (size_t i = 0; i < _count; ++i) {
            void* component = _data + (i * _elementSize);
            if (_destructor != nullptr) {
                _destructor(component);
            }
        }
    }
    if (_sparse != nullptr) {
        std::memset(_sparse, 0xFF, _sparseCapacity * sizeof(uint32_t));
    }
    _count = 0;
}

auto SparseSet::GetBufferView(const void* owner, const char* format) const noexcept -> BufferView {
    BufferView view = {};
    view.buf        = _data;
    view.obj        = const_cast<void*>(owner);
    view.itemsize   = static_cast<uint32_t>(_elementSize);
    std::strncpy(view.format, format, 7);
    view.readonly   = 0;
    view.ndim       = 1;
    view.shape[0]   = _count;
    view.strides[0] = _elementSize;
    view.flags      = ZHLN_BUFFER_CONTIGUOUS | ZHLN_BUFFER_WRITABLE;
    return view;
}

auto SparseSet::GetEntityView(const void* owner) const noexcept -> BufferView {
    BufferView view = {};
    view.buf        = _dense;
    view.obj        = const_cast<void*>(owner);
    view.itemsize   = sizeof(Entity);
    std::strncpy(view.format, "Q", 7);
    view.readonly   = 1;
    view.ndim       = 1;
    view.shape[0]   = _count;
    view.strides[0] = sizeof(Entity);
    view.flags      = ZHLN_BUFFER_CONTIGUOUS;
    return view;
}


Registry::Registry(): Registry(nullptr) {
}

Registry::Registry(std::pmr::memory_resource* memory):
    _memory(memory != nullptr ? memory : std::pmr::get_default_resource()), _typeInfo(_memory) {
    sync.viewExportCount.store(0, std::memory_order::relaxed);
    std::memset(&sync.shadowLock, 0, sizeof(sync.shadowLock));
}

// Component sets live in the same resource as all of their backing buffers.
auto Registry::CreateSparseSet(size_t elementSize, size_t alignment, SparseSet::DestructorFn destructor) -> SparseSet* {
    void* const storage = _memory->allocate(sizeof(SparseSet), alignof(SparseSet));
    return std::construct_at(static_cast<SparseSet*>(storage), elementSize, alignment, &sync, destructor, _memory);
}

Registry::~Registry() {
    // Storage destruction only runs component destructors. External handles
    // belong to the scene cleanup pass, not the registry.
    for (size_t i = 0; i < _compCapacity; ++i) {
        SparseSet* set = _components[i];
        _components[i] = nullptr;
        if (set != nullptr) {
            std::destroy_at(set);
            _memory->deallocate(set, sizeof(SparseSet), alignof(SparseSet));
        }
    }
    if (_components != nullptr) {
        _memory->deallocate(_components, _compCapacity * sizeof(SparseSet*), alignof(SparseSet*));
    }
    if (_generations != nullptr) {
        _memory->deallocate(_generations, _entityCapacity * sizeof(uint32_t), alignof(uint32_t));
    }
    if (_freeIndices != nullptr) {
        _memory->deallocate(_freeIndices, _entityCapacity * sizeof(uint32_t), alignof(uint32_t));
    }
}

void Registry::EnsureEntityCapacity(uint32_t index) {
    if (index < _entityCapacity) {
        return;
    }

    size_t oldCap   = _entityCapacity;
    _entityCapacity = oldCap == 0 ? 1024 : oldCap * 2;
    while (_entityCapacity <= index) {
        _entityCapacity *= 2;
    }

    _generations = static_cast<uint32_t*>(ReallocAligned(_memory, _generations, oldCap * sizeof(uint32_t),
                                                         _entityCapacity * sizeof(uint32_t), alignof(uint32_t)));
    _freeIndices = static_cast<uint32_t*>(ReallocAligned(_memory, _freeIndices, oldCap * sizeof(uint32_t),
                                                         _entityCapacity * sizeof(uint32_t), alignof(uint32_t)));

    for (size_t i = oldCap; i < _entityCapacity; ++i) {
        _generations[i]            = 1;
        _freeIndices[_freeCount++] = static_cast<uint32_t>(i);
    }
}

void Registry::EnsureComponentCapacity(uint32_t id) {
    if (id < _compCapacity) {
        return;
    }
    size_t oldCap = _compCapacity;
    _compCapacity = id + 8;
    _components   = static_cast<SparseSet**>(ReallocAligned(_memory, _components, oldCap * sizeof(SparseSet*),
                                                            _compCapacity * sizeof(SparseSet*), alignof(SparseSet*)));
    for (size_t i = oldCap; i < _compCapacity; ++i) {
        _components[i] = nullptr;
    }
    _typeInfo.resize(_compCapacity);
}

auto Registry::Create() -> Entity {
    return ZHLN::Lock(sync.shadowLock, [&] -> Entity {
        if (_freeCount == 0) {
            EnsureEntityCapacity(static_cast<uint32_t>(_entityCapacity));
        }
        uint32_t index = _freeIndices[--_freeCount];
        return Entity {.index = index, .generation = _generations[index]};
    });
}

void Registry::Destroy(Entity entity) {
    ZHLN::Lock(sync.shadowLock, [&] -> void {
        if (entity.index >= _entityCapacity || _generations[entity.index] != entity.generation) {
            return;
        }
        for (size_t i = 0; i < _compCapacity; ++i) {
            if (_components[i] != nullptr) {
                _components[i]->Remove(entity);
            }
        }
        _generations[entity.index]++;
        _freeIndices[_freeCount++] = entity.index;
    });
}

auto Registry::IsAlive(Entity entity) const noexcept -> bool {
    return entity.index < _entityCapacity && _generations[entity.index] == entity.generation;
}

void Registry::Clear() {
    ZHLN::Lock(sync.shadowLock, [&] -> void {
        for (size_t i = 0; i < _compCapacity; ++i) {
            if (_components[i] != nullptr) {
                _components[i]->Clear();
            }
        }
        _freeCount   = 0;
        _entityCount = 0;
        for (size_t i = 0; i < _entityCapacity; ++i) {
            _generations[i]++;
            _freeIndices[_freeCount++] = static_cast<uint32_t>(i);
        }
    });
}

auto Registry::RegisterComponentDynamic(std::string_view name, size_t size, size_t alignment) -> uint32_t {
    uint32_t id = GetFamilyIDFromName(name);
    if (id != 0xFFFFFFFF) {
        // A registered C++ family must never be converted into unconstructed
        // script storage. AddDynamic below also enforces this at insertion.
        if (id < _compCapacity && _components[id] != nullptr && !_typeInfo[id].isDynamic) {
            return 0xFFFFFFFF;
        }
        return id;
    }
    ZHLN::Lock(sync.shadowLock, [&] -> void {
        id = ComponentFamily::ResolveDenseID(HashTypeName(name));
        EnsureComponentCapacity(id);
        if (_components[id] != nullptr) {
            id = 0xFFFFFFFF;
            return;
        }
        MapNameToFamilyID(name, id);
        _components[id] = CreateSparseSet(size, alignment);
        _typeInfo[id] = {
            .name      = name,
            .size      = size,
            .alignment = alignment,
            .debugDump = [](const void*, std::string& out) -> void { out += "{}"; },
            .isDynamic = true,
        };
    });
    return id;
}

auto Registry::AddDynamic(Entity entity, uint32_t familyID) -> void* {
    return ZHLN::Lock(sync.shadowLock, [&] -> void* {
        // Do not grow the registry for unknown (or sentinel) family IDs.
        // InsertEmpty cannot construct typed components or release handles.
        if (!IsAlive(entity) || familyID >= _compCapacity || _components[familyID] == nullptr || !_typeInfo[familyID].isDynamic) {
            return nullptr;
        }
        return _components[familyID]->InsertEmpty(entity);
    });
}

}
