// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Log.hpp>
#include <cstring>
#include <ecs/ECS.hpp>
#include <memory>
#include <string>
#include <utility>

namespace ZHLN::ECS {

namespace {

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

[[nodiscard]] constexpr auto IsValidLayout(size_t size, size_t alignment) noexcept -> bool {
    return size > 0 && alignment > 0 && (alignment & (alignment - 1)) == 0 && (size % alignment) == 0;
}

// Single-component staging buffer used on the stack so onRetire hooks always
// execute after sync.shadowLock is released without heap traffic for small PODs.
struct PendingRetire {
    static constexpr size_t kInlineSize  = 64;
    static constexpr size_t kInlineAlign = 16;

    SparseSet::RetireFn     onRetire  = nullptr;
    void*                   hookCtx   = nullptr;
    SparseSet::DestructorFn destroy   = nullptr;
    void*                   storage   = nullptr;
    size_t                  size      = 0;
    size_t                  alignment = 1;
    bool                    heapAlloc = false;
    alignas(kInlineAlign) std::byte inlineBuf[kInlineSize] {};

    [[nodiscard]] auto Allocate(std::pmr::memory_resource* memory, size_t sz, size_t align) -> void* {
        size      = sz;
        alignment = align;
        if (sz <= kInlineSize && align <= kInlineAlign) {
            storage   = inlineBuf;
            heapAlloc = false;
        } else {
            storage   = memory->allocate(sz, align);
            heapAlloc = true;
        }
        return storage;
    }

    void InvokeAndRelease(std::pmr::memory_resource* memory, Entity entity) noexcept {
        if (onRetire != nullptr && storage != nullptr) {
            onRetire(hookCtx, entity, storage);
        }
        if (destroy != nullptr && storage != nullptr) {
            destroy(storage);
        }
        if (heapAlloc && storage != nullptr) {
            memory->deallocate(storage, size, alignment);
        }
        storage   = nullptr;
        heapAlloc = false;
    }
};

struct PendingHeapRetire {
    SparseSet::RetireFn     onRetire  = nullptr;
    void*                   hookCtx   = nullptr;
    SparseSet::DestructorFn destroy   = nullptr;
    void*                   storage   = nullptr;
    size_t                  size      = 0;
    size_t                  alignment = 1;
};

struct PendingRetireBatch {
    SparseSet::RetireFn       onRetire    = nullptr;
    void*                     hookCtx     = nullptr;
    SparseSet::DestructorFn   destroy     = nullptr;
    SparseSet::ExtractedBatch batch       = {};
    size_t                    elementSize = 0;
    size_t                    alignment   = 1;
};

} // namespace

SparseSet::SparseSet(size_t elementSize, size_t alignment, ZHLN::BufferSync* syncPtr, DestructorFn destructor, RelocateFn relocate):
    SparseSet(elementSize, alignment, syncPtr, destructor, relocate, nullptr) {
}

SparseSet::SparseSet(size_t elementSize, size_t alignment, ZHLN::BufferSync* syncPtr, DestructorFn destructor, RelocateFn relocate,
                     std::pmr::memory_resource* memory):
    _memory(memory != nullptr ? memory : std::pmr::get_default_resource()), _elementSize(elementSize), _alignment(alignment), _sync(syncPtr),
    _destructor(destructor), _relocate(relocate) {
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

    const size_t oldCap = _denseCapacity;
    _denseCapacity      = oldCap == 0 ? 64 : oldCap * 2;

    _dense        = static_cast<Entity*>(ReallocAligned(_memory, _dense, oldCap * sizeof(Entity), _denseCapacity * sizeof(Entity), alignof(Entity)));
    auto* newData = static_cast<std::byte*>(_memory->allocate(_denseCapacity * _elementSize, _alignment));
    if (_data != nullptr) {
        if (_count > 0) {
            if (_relocate != nullptr) {
                for (size_t i = 0; i < _count; ++i) {
                    _relocate(newData + (i * _elementSize), _data + (i * _elementSize));
                }
            } else {
                std::memcpy(newData, _data, _count * _elementSize);
            }
        }
        _memory->deallocate(_data, oldCap * _elementSize, _alignment);
    }
    _data = newData;
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

auto SparseSet::ReserveSlot(Entity entity) -> void* {
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
        void* component = _data + (denseIdx * _elementSize);
        if (_destructor != nullptr) {
            _destructor(component);
        }
    }
    return _data + (denseIdx * _elementSize);
}

auto SparseSet::InsertEmpty(Entity entity) -> void* {
    void* slot = ReserveSlot(entity);
    std::memset(slot, 0, _elementSize);
    return slot;
}

void SparseSet::Remove(Entity entity) {
    if (!Contains(entity)) {
        return;
    }
    uint32_t denseIdx  = _sparse[entity.index];
    void*    component = _data + (denseIdx * _elementSize);
    if (_destructor != nullptr) {
        _destructor(component);
    }

    uint32_t lastIdx = static_cast<uint32_t>(_count) - 1;
    if (denseIdx != lastIdx) {
        Entity lastEntity = _dense[lastIdx];
        void*  lastComp   = _data + (lastIdx * _elementSize);
        if (_relocate != nullptr) {
            _relocate(component, lastComp);
        } else {
            std::memcpy(component, lastComp, _elementSize);
        }
        _dense[denseIdx]          = lastEntity;
        _sparse[lastEntity.index] = denseIdx;
    }
    _sparse[entity.index] = INVALID_DENSE;
    _count--;
}

auto SparseSet::ExtractAndRemove(Entity entity, void* outBuffer) noexcept -> bool {
    if (!Contains(entity)) {
        return false;
    }
    uint32_t denseIdx  = _sparse[entity.index];
    void*    component = _data + (denseIdx * _elementSize);
    if (_relocate != nullptr) {
        _relocate(outBuffer, component);
    } else {
        std::memcpy(outBuffer, component, _elementSize);
    }

    uint32_t lastIdx = static_cast<uint32_t>(_count) - 1;
    if (denseIdx != lastIdx) {
        Entity lastEntity = _dense[lastIdx];
        void*  lastComp   = _data + (lastIdx * _elementSize);
        if (_relocate != nullptr) {
            _relocate(component, lastComp);
        } else {
            std::memcpy(component, lastComp, _elementSize);
        }
        _dense[denseIdx]          = lastEntity;
        _sparse[lastEntity.index] = denseIdx;
    }
    _sparse[entity.index] = INVALID_DENSE;
    _count--;
    return true;
}

auto SparseSet::ExtractAll() noexcept -> ExtractedBatch {
    if (_count == 0) {
        return {};
    }
    if ((_sync != nullptr) && _sync->viewExportCount.load(std::memory_order::acquire) > 0) {
        ZHLN::Panic("FFI MEMORY VIOLATION: SparseSet Clear during exported view");
    }
    ExtractedBatch batch {
        .entities      = _dense,
        .data          = _data,
        .count         = _count,
        .denseCapacity = _denseCapacity,
    };
    _dense         = nullptr;
    _data          = nullptr;
    _denseCapacity = 0;
    _count         = 0;
    if (_sparse != nullptr) {
        std::memset(_sparse, 0xFF, _sparseCapacity * sizeof(uint32_t));
    }
    return batch;
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
    _memory(memory != nullptr ? memory : std::pmr::get_default_resource()), _slots(_memory), _retiredTables(_memory) {
}

// Component sets live in the same resource as all of their backing buffers.
auto Registry::CreateSparseSet(size_t elementSize, size_t alignment, SparseSet::DestructorFn destructor, SparseSet::RelocateFn relocate) -> SparseSet* {
    void* const storage = _memory->allocate(sizeof(SparseSet), alignof(SparseSet));
    return std::construct_at(static_cast<SparseSet*>(storage), elementSize, alignment, &sync, destructor, relocate, _memory);
}

void Registry::DestroyKeyTable(KeyTable* table) noexcept {
    if (table == nullptr) {
        return;
    }
    if (table->entries != nullptr) {
        for (size_t i = 0; i < table->capacity; ++i) {
            std::destroy_at(&table->entries[i]);
        }
        _memory->deallocate(table->entries, table->capacity * sizeof(KeyEntry), alignof(KeyEntry));
    }
    std::destroy_at(table);
    _memory->deallocate(table, sizeof(KeyTable), alignof(KeyTable));
}

Registry::~Registry() {
    // Storage destruction only runs component destructors. External handles
    // belong to the scene cleanup pass, not the registry.
    for (auto& slot: _slots) {
        SparseSet* set = slot.set;
        slot.set       = nullptr;
        if (set != nullptr) {
            std::destroy_at(set);
            _memory->deallocate(set, sizeof(SparseSet), alignof(SparseSet));
        }
    }
    _slots.clear();

    KeyTable* active = _activeKeyTable.exchange(nullptr, std::memory_order::acq_rel);
    DestroyKeyTable(active);
    for (KeyTable* retired: _retiredTables) {
        DestroyKeyTable(retired);
    }
    _retiredTables.clear();

    if (_schemaTable.entries != nullptr) {
        _memory->deallocate(_schemaTable.entries, _schemaTable.capacity * sizeof(SchemaEntry), alignof(SchemaEntry));
        _schemaTable = {};
    }
    if (_nameTable.entries != nullptr) {
        _memory->deallocate(_nameTable.entries, _nameTable.capacity * sizeof(NameEntry), alignof(NameEntry));
        _nameTable = {};
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

    _generations =
        static_cast<uint32_t*>(ReallocAligned(_memory, _generations, oldCap * sizeof(uint32_t), _entityCapacity * sizeof(uint32_t), alignof(uint32_t)));
    _freeIndices =
        static_cast<uint32_t*>(ReallocAligned(_memory, _freeIndices, oldCap * sizeof(uint32_t), _entityCapacity * sizeof(uint32_t), alignof(uint32_t)));

    for (size_t i = oldCap; i < _entityCapacity; ++i) {
        _generations[i]            = 1;
        _freeIndices[_freeCount++] = static_cast<uint32_t>(i);
    }
}

auto Registry::FindSparseSet(uint64_t typeKey) const noexcept -> SparseSet* {
    const KeyTable* tbl = _activeKeyTable.load(std::memory_order::acquire);
    if (tbl == nullptr) {
        return nullptr;
    }
    const size_t mask = tbl->mask;
    size_t       idx  = Mix64(typeKey) & mask;
    for (;;) {
        SparseSet* set = tbl->entries[idx].set.load(std::memory_order::acquire);
        if (set == nullptr) {
            return nullptr;
        }
        if (tbl->entries[idx].key == typeKey) {
            return set;
        }
        idx = (idx + 1) & mask;
    }
}

auto Registry::FindSlotIndexByKeyLocked(uint64_t typeKey) const noexcept -> uint32_t {
    if (_schemaTable.entries == nullptr) {
        return InvalidSlot;
    }
    const size_t mask = _schemaTable.mask;
    size_t       idx  = Mix64(typeKey) & mask;
    for (;;) {
        const SchemaEntry& entry = _schemaTable.entries[idx];
        if (entry.slot == InvalidSlot) {
            return InvalidSlot;
        }
        if (entry.key == typeKey) {
            return entry.slot;
        }
        idx = (idx + 1) & mask;
    }
}

void Registry::InsertSchemaTableLocked(uint64_t typeKey, uint32_t slotIndex) {
    if (_schemaTable.entries == nullptr || ((_schemaTable.count + 1) * 10 >= _schemaTable.capacity * 7)) {
        const size_t newCap  = (_schemaTable.capacity == 0) ? 64 : (_schemaTable.capacity * 2);
        const size_t newMask = newCap - 1;
        auto*        newEnts = static_cast<SchemaEntry*>(_memory->allocate(newCap * sizeof(SchemaEntry), alignof(SchemaEntry)));
        for (size_t i = 0; i < newCap; ++i) {
            newEnts[i] = SchemaEntry {};
        }

        size_t newCount = 0;
        if (_schemaTable.entries != nullptr) {
            for (size_t i = 0; i < _schemaTable.capacity; ++i) {
                const SchemaEntry& old = _schemaTable.entries[i];
                if (old.slot == InvalidSlot) {
                    continue;
                }
                size_t pos = Mix64(old.key) & newMask;
                while (newEnts[pos].slot != InvalidSlot) {
                    pos = (pos + 1) & newMask;
                }
                newEnts[pos] = old;
                ++newCount;
            }
            _memory->deallocate(_schemaTable.entries, _schemaTable.capacity * sizeof(SchemaEntry), alignof(SchemaEntry));
        }
        _schemaTable = SchemaTable {.entries = newEnts, .capacity = newCap, .mask = newMask, .count = newCount};
    }

    size_t idx = Mix64(typeKey) & _schemaTable.mask;
    for (;;) {
        SchemaEntry& entry = _schemaTable.entries[idx];
        if (entry.slot == InvalidSlot) {
            entry = SchemaEntry {.key = typeKey, .slot = slotIndex};
            _schemaTable.count++;
            return;
        }
        if (entry.key == typeKey) {
            entry.slot = slotIndex;
            return;
        }
        idx = (idx + 1) & _schemaTable.mask;
    }
}

auto Registry::FindSlotIndexByNameLocked(std::string_view name) const noexcept -> uint32_t {
    if (name.empty() || _nameTable.entries == nullptr) {
        return InvalidSlot;
    }
    const uint64_t h    = Hash64(name);
    const size_t   mask = _nameTable.mask;
    size_t         idx  = Mix64(h) & mask;
    for (;;) {
        const NameEntry& entry = _nameTable.entries[idx];
        if (entry.slot == InvalidSlot) {
            return InvalidSlot;
        }
        if (entry.nameHash == h && _slots[entry.slot].name == name) {
            return entry.slot;
        }
        idx = (idx + 1) & mask;
    }
}

auto Registry::FindSlotIndexForInspectionLocked(std::string_view name) const noexcept -> uint32_t {
    if (name.empty()) {
        return InvalidSlot;
    }
    if (const uint32_t byName = FindSlotIndexByNameLocked(name); byName != InvalidSlot) {
        return byName;
    }
    if (const uint32_t byKey = FindSlotIndexByKeyLocked(Hash64(name)); byKey != InvalidSlot) {
        const ComponentSlot& slot = _slots[byKey];
        if (!slot.name.empty() || slot.ops.published) {
            return byKey;
        }
    }
    return InvalidSlot;
}

void Registry::InsertKeyTableLocked(uint64_t typeKey, uint32_t slotIndex, SparseSet* set) {
    KeyTable* old = _activeKeyTable.load(std::memory_order::relaxed);
    if (old == nullptr || ((old->count + 1) * 10 >= old->capacity * 7)) {
        const size_t newCap   = (old == nullptr) ? 64 : (old->capacity * 2);
        const size_t newMask  = newCap - 1;
        void*        tblRaw   = _memory->allocate(sizeof(KeyTable), alignof(KeyTable));
        void*        entsRaw  = _memory->allocate(newCap * sizeof(KeyEntry), alignof(KeyEntry));
        auto*        entries  = static_cast<KeyEntry*>(entsRaw);
        for (size_t i = 0; i < newCap; ++i) {
            std::construct_at(&entries[i]);
        }
        auto* newTbl = std::construct_at(static_cast<KeyTable*>(tblRaw), KeyTable {.entries = entries, .capacity = newCap, .mask = newMask, .count = 0});

        auto insertFresh = [newTbl](uint64_t k, uint32_t s, SparseSet* sp) {
            size_t i = Mix64(k) & newTbl->mask;
            while (newTbl->entries[i].set.load(std::memory_order::relaxed) != nullptr) {
                i = (i + 1) & newTbl->mask;
            }
            newTbl->entries[i].key  = k;
            newTbl->entries[i].slot = s;
            newTbl->entries[i].set.store(sp, std::memory_order::relaxed);
            newTbl->count++;
        };

        if (old != nullptr) {
            for (size_t i = 0; i < old->capacity; ++i) {
                SparseSet* sp = old->entries[i].set.load(std::memory_order::relaxed);
                if (sp != nullptr) {
                    insertFresh(old->entries[i].key, old->entries[i].slot, sp);
                }
            }
        }
        insertFresh(typeKey, slotIndex, set);

        _activeKeyTable.store(newTbl, std::memory_order::release);
        if (old != nullptr) {
            _retiredTables.push_back(old);
        }
        return;
    }

    size_t idx = Mix64(typeKey) & old->mask;
    while (old->entries[idx].set.load(std::memory_order::relaxed) != nullptr) {
        idx = (idx + 1) & old->mask;
    }
    old->entries[idx].key  = typeKey;
    old->entries[idx].slot = slotIndex;
    old->entries[idx].set.store(set, std::memory_order::release);
    old->count++;
}

auto Registry::InsertNameTableLocked(std::string_view name, uint32_t slotIndex) -> bool {
    if (name.empty()) {
        return true;
    }
    const uint64_t h = Hash64(name);

    if (_nameTable.entries == nullptr || ((_nameTable.count + 1) * 10 >= _nameTable.capacity * 7)) {
        const size_t newCap  = (_nameTable.capacity == 0) ? 64 : (_nameTable.capacity * 2);
        const size_t newMask = newCap - 1;
        auto*        newEnts = static_cast<NameEntry*>(_memory->allocate(newCap * sizeof(NameEntry), alignof(NameEntry)));
        for (size_t i = 0; i < newCap; ++i) {
            newEnts[i] = NameEntry {};
        }

        size_t newCount = 0;
        if (_nameTable.entries != nullptr) {
            for (size_t i = 0; i < _nameTable.capacity; ++i) {
                const NameEntry& old = _nameTable.entries[i];
                if (old.slot == InvalidSlot) {
                    continue;
                }
                // Drop stale name mappings if a slot was renamed.
                if (Hash64(_slots[old.slot].name) != old.nameHash) {
                    continue;
                }
                size_t pos = Mix64(old.nameHash) & newMask;
                while (newEnts[pos].slot != InvalidSlot) {
                    pos = (pos + 1) & newMask;
                }
                newEnts[pos] = old;
                ++newCount;
            }
            _memory->deallocate(_nameTable.entries, _nameTable.capacity * sizeof(NameEntry), alignof(NameEntry));
        }
        _nameTable = NameTable {.entries = newEnts, .capacity = newCap, .mask = newMask, .count = newCount};
    }

    size_t idx = Mix64(h) & _nameTable.mask;
    for (;;) {
        NameEntry& entry = _nameTable.entries[idx];
        if (entry.slot == InvalidSlot) {
            entry = NameEntry {.nameHash = h, .slot = slotIndex};
            _nameTable.count++;
            return true;
        }
        if (entry.nameHash == h && _slots[entry.slot].name == name) {
            return entry.slot == slotIndex;
        }
        idx = (idx + 1) & _nameTable.mask;
    }
}

auto Registry::EnsureTypedSlotLocked(uint64_t typeKey, const ComponentOps& ops) -> uint32_t {
    if (const uint32_t existing = FindSlotIndexByKeyLocked(typeKey); existing != InvalidSlot) {
        return existing;
    }
    const auto slotIdx = static_cast<uint32_t>(_slots.size());
    _slots.emplace_back(_memory);
    ComponentSlot& slot = _slots.back();
    slot.typeKey        = typeKey;
    slot.set            = nullptr;
    slot.ops            = ops;
    InsertSchemaTableLocked(typeKey, slotIdx);
    return slotIdx;
}

auto Registry::EnsureLiveSetLocked(uint32_t slotIndex) -> SparseSet* {
    ComponentSlot& slot = _slots[slotIndex];
    if (slot.set != nullptr) {
        return slot.set;
    }
    SparseSet* set = CreateSparseSet(slot.ops.size, slot.ops.alignment, slot.ops.destroy, slot.ops.relocate);
    if (slot.ops.onRetire != nullptr) {
        set->SetRetireHook(slot.ops.onRetire, slot.ops.hookCtx);
    }
    slot.set = set;
    InsertKeyTableLocked(slot.typeKey, slotIndex, set);
    _schemaEpoch.fetch_add(1, std::memory_order::release);
    return set;
}

auto Registry::CreateDynamicSlotLocked(std::string_view name, uint64_t typeKey, size_t size, size_t alignment, uint64_t layoutHash) -> uint32_t {
    const auto slotIdx = static_cast<uint32_t>(_slots.size());
    _slots.emplace_back(_memory);
    ComponentSlot& slot = _slots.back();
    slot.typeKey        = typeKey;
    slot.name.assign(name.data(), name.size());
    slot.set            = nullptr;
    slot.ops            = ComponentOps {
        .size       = size,
        .alignment  = alignment,
        .layoutHash = layoutHash,
        .debugDump  = +[](const void*, std::string& out) { out += "{}"; },
        .isDynamic  = true,
    };
    InsertSchemaTableLocked(typeKey, slotIdx);
    (void)InsertNameTableLocked(slot.name, slotIdx);
    return slotIdx;
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
    std::pmr::vector<PendingHeapRetire> pending(_memory);
    ZHLN::Lock(sync.shadowLock, [&] -> void {
        if (entity.index >= _entityCapacity || _generations[entity.index] != entity.generation) {
            return;
        }
        for (auto& slot: _slots) {
            if (slot.set == nullptr) {
                continue;
            }
            if (slot.ops.onRetire != nullptr && slot.set->Contains(entity)) {
                void* staged = _memory->allocate(slot.ops.size, slot.ops.alignment);
                if (slot.set->ExtractAndRemove(entity, staged)) {
                    pending.push_back(PendingHeapRetire {
                        .onRetire  = slot.ops.onRetire,
                        .hookCtx   = slot.ops.hookCtx,
                        .destroy   = slot.ops.destroy,
                        .storage   = staged,
                        .size      = slot.ops.size,
                        .alignment = slot.ops.alignment,
                    });
                } else {
                    _memory->deallocate(staged, slot.ops.size, slot.ops.alignment);
                }
            } else {
                slot.set->Remove(entity);
            }
        }
        _generations[entity.index]++;
        _freeIndices[_freeCount++] = entity.index;
    });

    for (const PendingHeapRetire& item: pending) {
        item.onRetire(item.hookCtx, entity, item.storage);
        if (item.destroy != nullptr) {
            item.destroy(item.storage);
        }
        _memory->deallocate(item.storage, item.size, item.alignment);
    }
}

auto Registry::IsAlive(Entity entity) const noexcept -> bool {
    return entity.index < _entityCapacity && _generations[entity.index] == entity.generation;
}

void Registry::Clear() {
    std::pmr::vector<PendingRetireBatch> batches(_memory);
    ZHLN::Lock(sync.shadowLock, [&] -> void {
        for (auto& slot: _slots) {
            if (slot.set == nullptr) {
                continue;
            }
            if (slot.ops.onRetire != nullptr && slot.set->Count() > 0) {
                SparseSet::ExtractedBatch extracted = slot.set->ExtractAll();
                if (extracted.count > 0) {
                    batches.push_back(PendingRetireBatch {
                        .onRetire    = slot.ops.onRetire,
                        .hookCtx     = slot.ops.hookCtx,
                        .destroy     = slot.ops.destroy,
                        .batch       = extracted,
                        .elementSize = slot.ops.size,
                        .alignment   = slot.ops.alignment,
                    });
                }
            } else {
                slot.set->Clear();
            }
        }
        _freeCount   = 0;
        _entityCount = 0;
        for (size_t i = 0; i < _entityCapacity; ++i) {
            _generations[i]++;
            _freeIndices[_freeCount++] = static_cast<uint32_t>(i);
        }
    });

    for (const PendingRetireBatch& entry: batches) {
        for (size_t i = 0; i < entry.batch.count; ++i) {
            void* component = entry.batch.data + (i * entry.elementSize);
            entry.onRetire(entry.hookCtx, entry.batch.entities[i], component);
            if (entry.destroy != nullptr) {
                entry.destroy(component);
            }
        }
        if (entry.batch.entities != nullptr) {
            _memory->deallocate(entry.batch.entities, entry.batch.denseCapacity * sizeof(Entity), alignof(Entity));
        }
        if (entry.batch.data != nullptr) {
            _memory->deallocate(entry.batch.data, entry.batch.denseCapacity * entry.elementSize, entry.alignment);
        }
    }
}

void Registry::RemoveByKey(Entity entity, uint64_t typeKey) {
    SparseSet* fastSet = FindSparseSet(typeKey);
    if (fastSet == nullptr || !fastSet->Contains(entity)) {
        return;
    }
    PendingRetire pending {};
    ZHLN::Lock(sync.shadowLock, [&] -> void {
        const uint32_t slotIdx = FindSlotIndexByKeyLocked(typeKey);
        if (slotIdx == InvalidSlot) {
            return;
        }
        ComponentSlot& slot = _slots[slotIdx];
        if (slot.set == nullptr || !slot.set->Contains(entity)) {
            return;
        }
        if (slot.ops.onRetire != nullptr) {
            void* staged = pending.Allocate(_memory, slot.ops.size, slot.ops.alignment);
            if (slot.set->ExtractAndRemove(entity, staged)) {
                pending.onRetire = slot.ops.onRetire;
                pending.hookCtx  = slot.ops.hookCtx;
                pending.destroy  = slot.ops.destroy;
            }
        } else {
            slot.set->Remove(entity);
        }
    });
    pending.InvokeAndRelease(_memory, entity);
}

auto Registry::RegisterComponentDynamic(std::string_view name, size_t size, size_t alignment, uint64_t layoutHash) -> uint32_t {
    if (name.empty() || !IsValidLayout(size, alignment)) {
        return InvalidSlot;
    }
    const uint64_t key = Hash64(name);
    return ZHLN::Lock(sync.shadowLock, [&] -> uint32_t {
        if (const uint32_t byKey = FindSlotIndexByKeyLocked(key); byKey != InvalidSlot) {
            const ComponentSlot& slot = _slots[byKey];
            if (!slot.ops.isDynamic || slot.name != name || slot.ops.size != size || slot.ops.alignment != alignment ||
                slot.ops.layoutHash != layoutHash) {
                return InvalidSlot;
            }
            return byKey;
        }
        // Refuse any spelling alias that collides with an already registered
        // component's display name (for example, a native typed short name).
        if (FindSlotIndexByNameLocked(name) != InvalidSlot) {
            return InvalidSlot;
        }
        return CreateDynamicSlotLocked(name, key, size, alignment, layoutHash);
    });
}

auto Registry::HasFamily(std::string_view name) const noexcept -> bool {
    return ZHLN::Lock(sync.shadowLock, [&] -> bool { return FindSlotIndexForInspectionLocked(name) != InvalidSlot; });
}

auto Registry::GetRawByName(Entity entity, std::string_view name) const noexcept -> void* {
    return ZHLN::Lock(sync.shadowLock, [&] -> void* {
        const uint32_t slotIdx = FindSlotIndexForInspectionLocked(name);
        if (slotIdx == InvalidSlot) {
            return nullptr;
        }
        SparseSet* set = _slots[slotIdx].set;
        return set != nullptr ? set->Get(entity) : nullptr;
    });
}

void Registry::DebugDumpEntity(Entity entity, std::string& out) const {
    ZHLN::Lock(sync.shadowLock, [&] -> void {
        for (const ComponentSlot& slot: _slots) {
            if (slot.set == nullptr) {
                continue;
            }
            if (void* raw = slot.set->Get(entity)) {
                out += slot.name;
                out += ": ";
                if (slot.ops.debugDump != nullptr) {
                    slot.ops.debugDump(raw, out);
                } else {
                    out += "{}";
                }
                out += "\n";
            }
        }
    });
}

auto Registry::IsPublishedComponent(std::string_view typeName) const noexcept -> bool {
    if (typeName.empty()) {
        return false;
    }
    const uint64_t key = Hash64(typeName);
    return ZHLN::Lock(sync.shadowLock, [&] -> bool {
        const uint32_t slotIdx = FindSlotIndexByKeyLocked(key);
        if (slotIdx == InvalidSlot) {
            return false;
        }
        const ComponentOps& ops = _slots[slotIdx].ops;
        return !ops.isDynamic && ops.published;
    });
}

auto Registry::IsReadOnlyComponent(std::string_view typeName) const noexcept -> bool {
    if (typeName.empty()) {
        return false;
    }
    const uint64_t key = Hash64(typeName);
    return ZHLN::Lock(sync.shadowLock, [&] -> bool {
        const uint32_t slotIdx = FindSlotIndexByKeyLocked(key);
        if (slotIdx == InvalidSlot) {
            return false;
        }
        const ComponentOps& ops = _slots[slotIdx].ops;
        return !ops.isDynamic && ops.readOnly;
    });
}

auto Registry::AttachRaw(Entity entity, std::string_view typeName, size_t size, size_t alignment, uint64_t layoutHash) -> void* {
    if (typeName.empty() || !IsValidLayout(size, alignment)) {
        return nullptr;
    }
    const uint64_t key = Hash64(typeName);
    PendingRetire  pending {};
    void*          result = ZHLN::Lock(sync.shadowLock, [&] -> void* {
        if (!IsAlive(entity)) {
            return nullptr;
        }
        if (const uint32_t slotIdx = FindSlotIndexByKeyLocked(key); slotIdx != InvalidSlot) {
            ComponentSlot& slot = _slots[slotIdx];
            if (!slot.ops.isDynamic) {
                if (!slot.ops.published) {
                    return nullptr;
                }
                if (layoutHash == 0 || slot.ops.size != size || slot.ops.alignment != alignment || slot.ops.layoutHash != layoutHash) {
                    return nullptr;
                }
                SparseSet* set = EnsureLiveSetLocked(slotIdx);
                if (void* existing = set->Get(entity)) {
                    if (slot.ops.onRetire != nullptr) {
                        void* staged = pending.Allocate(_memory, slot.ops.size, slot.ops.alignment);
                        if (slot.ops.relocate != nullptr) {
                            slot.ops.relocate(staged, existing);
                        } else {
                            std::memcpy(staged, existing, slot.ops.size);
                            if (slot.ops.destroy != nullptr) {
                                slot.ops.destroy(existing);
                            }
                        }
                        pending.onRetire = slot.ops.onRetire;
                        pending.hookCtx  = slot.ops.hookCtx;
                        pending.destroy  = slot.ops.destroy;
                    } else if (slot.ops.destroy != nullptr) {
                        slot.ops.destroy(existing);
                    }
                    if (slot.ops.constructDefault != nullptr) {
                        slot.ops.constructDefault(existing);
                    } else {
                        std::memset(existing, 0, slot.ops.size);
                    }
                    return existing;
                }
                void* storage = set->ReserveSlot(entity);
                if (slot.ops.constructDefault != nullptr) {
                    slot.ops.constructDefault(storage);
                } else {
                    std::memset(storage, 0, slot.ops.size);
                }
                return storage;
            }
            if (slot.name != typeName || slot.ops.size != size || slot.ops.alignment != alignment || slot.ops.layoutHash != layoutHash) {
                return nullptr;
            }
            SparseSet* set = EnsureLiveSetLocked(slotIdx);
            return set->InsertEmpty(entity);
        }

        // Refuse dynamic registration if typeName collides with an existing
        // slot's display name (for example, a native component's short name).
        if (FindSlotIndexByNameLocked(typeName) != InvalidSlot) {
            return nullptr;
        }
        const uint32_t newSlotIdx = CreateDynamicSlotLocked(typeName, key, size, alignment, layoutHash);
        SparseSet*     set        = EnsureLiveSetLocked(newSlotIdx);
        return set->InsertEmpty(entity);
    });
    if (pending.onRetire != nullptr) {
        pending.InvokeAndRelease(_memory, entity);
        if (SparseSet* live = FindSparseSet(key)) {
            return live->Get(entity);
        }
        return nullptr;
    }
    return result;
}

auto Registry::DetachRaw(Entity entity, std::string_view typeName) -> bool {
    if (typeName.empty()) {
        return false;
    }
    const uint64_t key = Hash64(typeName);
    PendingRetire  pending {};
    const bool     detached = ZHLN::Lock(sync.shadowLock, [&] -> bool {
        if (!IsAlive(entity)) {
            return false;
        }
        const uint32_t slotIdx = FindSlotIndexByKeyLocked(key);
        if (slotIdx == InvalidSlot) {
            return false;
        }
        ComponentSlot& slot = _slots[slotIdx];
        if (!slot.ops.isDynamic) {
            if (!slot.ops.published || slot.set == nullptr || !slot.set->Contains(entity)) {
                return false;
            }
            if (slot.ops.onRetire != nullptr) {
                void* staged = pending.Allocate(_memory, slot.ops.size, slot.ops.alignment);
                if (slot.set->ExtractAndRemove(entity, staged)) {
                    pending.onRetire = slot.ops.onRetire;
                    pending.hookCtx  = slot.ops.hookCtx;
                    pending.destroy  = slot.ops.destroy;
                }
            } else {
                slot.set->Remove(entity);
            }
            return true;
        }
        if (slot.name != typeName || slot.set == nullptr || !slot.set->Contains(entity)) {
            return false;
        }
        slot.set->Remove(entity);
        return true;
    });
    pending.InvokeAndRelease(_memory, entity);
    return detached;
}

auto Registry::GetRaw(Entity entity, std::string_view typeName) const noexcept -> void* {
    if (typeName.empty()) {
        return nullptr;
    }
    const uint64_t key = Hash64(typeName);
    return ZHLN::Lock(sync.shadowLock, [&] -> void* {
        if (!IsAlive(entity)) {
            return nullptr;
        }
        const uint32_t slotIdx = FindSlotIndexByKeyLocked(key);
        if (slotIdx == InvalidSlot) {
            return nullptr;
        }
        const ComponentSlot& slot = _slots[slotIdx];
        if (!slot.ops.isDynamic) {
            if (!slot.ops.published) {
                return nullptr;
            }
        } else if (slot.name != typeName) {
            return nullptr;
        }
        return slot.set != nullptr ? slot.set->Get(entity) : nullptr;
    });
}

auto Registry::EntitiesWithRaw(std::string_view typeName) const noexcept -> std::span<const Entity> {
    if (typeName.empty()) {
        return {};
    }
    const uint64_t key = Hash64(typeName);
    return ZHLN::Lock(sync.shadowLock, [&] -> std::span<const Entity> {
        const uint32_t slotIdx = FindSlotIndexByKeyLocked(key);
        if (slotIdx == InvalidSlot) {
            return {};
        }
        const ComponentSlot& slot = _slots[slotIdx];
        if (!slot.ops.isDynamic) {
            if (!slot.ops.published) {
                return {};
            }
        } else if (slot.name != typeName) {
            return {};
        }
        if (slot.set == nullptr) {
            return {};
        }
        return {slot.set->GetDenseArray(), slot.set->Count()};
    });
}

} // namespace ZHLN::ECS
