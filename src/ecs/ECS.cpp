// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ECS.hpp"

#include "detail/ControlFlow.hpp"

#include <Zahlen/Log.hpp>
#include <cstdlib>
#include <cstring>

namespace ZHLN::ECS {

static ZHLN::Mutex s_FamilyMutex;
static uint32_t s_TypeCounter = 0;
static HashMap<uint32_t, uint32_t> s_HashToDense;
static HashMap<std::string_view, uint32_t> s_NameToFamilyID;

uint32_t ComponentFamily::ResolveDenseID(uint32_t typeHash) noexcept {
	ZHLN_LOCK(s_FamilyMutex) {
		const uint32_t* existing = s_HashToDense.Find(typeHash);
		if (existing != nullptr) {
			return *existing;
		}
		uint32_t id = s_TypeCounter++;
		s_HashToDense.Insert(typeHash, id);
		return id;
	}
}

void Registry::MapNameToFamilyID(std::string_view name, uint32_t id) noexcept {
	ZHLN_LOCK(s_FamilyMutex) {
		s_NameToFamilyID.Insert(name, id);
	}
}

uint32_t Registry::GetFamilyIDFromName(std::string_view name) noexcept {
	ZHLN_LOCK(s_FamilyMutex) {
		const uint32_t* id = s_NameToFamilyID.Find(name);
		if (id != nullptr) {
			return *id;
		}
	}
	return 0xFFFFFFFF; // Invalid
}

// --- HELPER: Manual Aligned Realloc ---
static void* ReallocAligned(void* oldPtr, size_t oldSize, size_t newSize, size_t alignment) {
	void* newPtr = ::operator new[](newSize, std::align_val_t{alignment});
	if (oldPtr != nullptr) {
		std::memcpy(newPtr, oldPtr, oldSize);
		::operator delete[](oldPtr, std::align_val_t{alignment});
	}
	return newPtr;
}

// ============================================================================
// SparseSet
// ============================================================================

SparseSet::SparseSet(size_t elementSize, size_t alignment, BufferSync* syncPtr,
					 DestructorFn destructor)
	: _elementSize(elementSize), _alignment(alignment), _sync(syncPtr), _destructor(destructor) {}

SparseSet::~SparseSet() {
	if (_destructor != nullptr && _data != nullptr) {
		for (size_t i = 0; i < _count; ++i) {
			_destructor(_data + (i * _elementSize));
		}
	}
	if (_sparse != nullptr) {
		::operator delete[](_sparse, std::align_val_t{alignof(uint32_t)});
	}
	if (_dense != nullptr) {
		::operator delete[](_dense, std::align_val_t{alignof(Entity)});
	}
	if (_data != nullptr) {
		::operator delete[](_data, std::align_val_t{_alignment});
	}
}

void SparseSet::ResizeSparse(uint32_t required) {
	if ((_sync != nullptr) && _sync->viewExportCount.load(std::memory_order_acquire) > 0) {
		ZHLN::Panic("FFI MEMORY VIOLATION: SparseSet Sparse Resize");
	}

	size_t oldCap = _sparseCapacity;
	_sparseCapacity = oldCap == 0 ? 1024 : oldCap * 2;
	while (_sparseCapacity <= required) {
		_sparseCapacity *= 2;
	}

	_sparse = (uint32_t*)ReallocAligned(_sparse, oldCap * sizeof(uint32_t),
										_sparseCapacity * sizeof(uint32_t), alignof(uint32_t));

	for (size_t i = oldCap; i < _sparseCapacity; ++i) {
		_sparse[i] = INVALID_DENSE;
	}
}

void SparseSet::ResizeDense() {
	if ((_sync != nullptr) && _sync->viewExportCount.load(std::memory_order_acquire) > 0) {
		ZHLN::Panic("FFI MEMORY VIOLATION: SparseSet Dense Resize");
	}

	size_t oldCap = _denseCapacity;
	_denseCapacity = oldCap == 0 ? 64 : oldCap * 2;

	_dense = (Entity*)ReallocAligned(_dense, oldCap * sizeof(Entity),
									 _denseCapacity * sizeof(Entity), alignof(Entity));
	_data = (std::byte*)ReallocAligned(_data, oldCap * _elementSize, _denseCapacity * _elementSize,
									   _alignment);
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
		denseIdx = (uint32_t)_count++;
		_dense[denseIdx] = entity;
		_sparse[entity.index] = denseIdx;
	}
	std::memcpy(_data + (denseIdx * _elementSize), data, _elementSize);
}

void* SparseSet::InsertEmpty(Entity entity) {
	if (entity.index >= _sparseCapacity) {
		ResizeSparse(entity.index);
	}

	uint32_t denseIdx = _sparse[entity.index];
	if (denseIdx == INVALID_DENSE) {
		if (_count >= _denseCapacity) {
			ResizeDense();
		}
		denseIdx = (uint32_t)_count++;
		_dense[denseIdx] = entity;
		_sparse[entity.index] = denseIdx;

		// Zero-initialize the generic memory slot
		std::memset(_data + (denseIdx * _elementSize), 0, _elementSize);
	}
	return _data + (denseIdx * _elementSize);
}

void SparseSet::Remove(Entity entity) {
	if (entity.index >= _sparseCapacity) {
		return;
	}
	uint32_t denseIdx = _sparse[entity.index];
	if (denseIdx == INVALID_DENSE) {
		return;
	}

	if (_destructor != nullptr) {
		_destructor(_data + (denseIdx * _elementSize));
	}

	uint32_t lastIdx = (uint32_t)_count - 1;
	if (denseIdx != lastIdx) {
		Entity lastEntity = _dense[lastIdx];
		std::memcpy(_data + (denseIdx * _elementSize), _data + (lastIdx * _elementSize),
					_elementSize);
		_dense[denseIdx] = lastEntity;
		_sparse[lastEntity.index] = denseIdx;
	}
	_sparse[entity.index] = INVALID_DENSE;
	_count--;
}

bool SparseSet::Contains(Entity entity) const noexcept {
	return entity.index < _sparseCapacity && _sparse[entity.index] != INVALID_DENSE;
}

void* SparseSet::Get(Entity entity) const noexcept {
	if (!Contains(entity)) {
		return nullptr;
	}
	return _data + (_sparse[entity.index] * _elementSize);
}

void SparseSet::Clear() noexcept {
	if (_destructor != nullptr && _data != nullptr) {
		for (size_t i = 0; i < _count; ++i) {
			_destructor(_data + (i * _elementSize));
		}
	}
	if (_sparse != nullptr) {
		std::memset(_sparse, 0xFF, _sparseCapacity * sizeof(uint32_t));
	}
	_count = 0;
}

BufferView SparseSet::GetBufferView(const void* owner, const char* format) const noexcept {
	BufferView view = {};
	view.buf = _data;
	view.obj = const_cast<void*>(owner);
	view.itemsize = (uint32_t)_elementSize;
	std::strncpy(view.format, format, 7);
	view.readonly = 0;
	view.ndim = 1;
	view.shape[0] = _count;
	view.strides[0] = _elementSize;
	view.flags = ZHLN_BUFFER_CONTIGUOUS | ZHLN_BUFFER_WRITABLE;
	return view;
}

BufferView SparseSet::GetEntityView(const void* owner) const noexcept {
	BufferView view = {};
	view.buf = _dense;
	view.obj = const_cast<void*>(owner);
	view.itemsize = sizeof(Entity);
	std::strncpy(view.format, "Q", 7);
	view.readonly = 1;
	view.ndim = 1;
	view.shape[0] = _count;
	view.strides[0] = sizeof(Entity);
	view.flags = ZHLN_BUFFER_CONTIGUOUS;
	return view;
}

// ============================================================================
// Registry
// ============================================================================

Registry::Registry() {
	sync.viewExportCount.store(0, std::memory_order_relaxed);
	std::memset(&sync.shadowLock, 0, sizeof(sync.shadowLock));
}

Registry::~Registry() {
	if (_generations != nullptr) {
		std::free(_generations);
	}
	if (_freeIndices != nullptr) {
		std::free(_freeIndices);
	}
	for (size_t i = 0; i < _compCapacity; ++i) {
		if (_components[i] != nullptr) {
			delete _components[i];
		}
	}
	if (_components != nullptr) {
		std::free(static_cast<void*>(_components));
	}
}

void Registry::EnsureEntityCapacity(uint32_t index) {
	if (index < _entityCapacity) {
		return;
	}

	size_t oldCap = _entityCapacity;
	_entityCapacity = oldCap == 0 ? 1024 : oldCap * 2;
	while (_entityCapacity <= index) {
		_entityCapacity *= 2;
	}

	_generations = (uint32_t*)std::realloc(_generations, _entityCapacity * sizeof(uint32_t));
	_freeIndices = (uint32_t*)std::realloc(_freeIndices, _entityCapacity * sizeof(uint32_t));

	for (size_t i = oldCap; i < _entityCapacity; ++i) {
		_generations[i] = 1;
		_freeIndices[_freeCount++] = (uint32_t)i;
	}
}

void Registry::EnsureComponentCapacity(uint32_t id) {
	if (id < _compCapacity) {
		return;
	}
	size_t oldCap = _compCapacity;
	_compCapacity = id + 8; // Small growth for component pointer array
	_components = (SparseSet**)std::realloc(static_cast<void*>(_components),
											_compCapacity * sizeof(SparseSet*));
	for (size_t i = oldCap; i < _compCapacity; ++i) {
		_components[i] = nullptr;
	}
}

Entity Registry::Create() {
	ZHLN_LOCK(sync.shadowLock) {
		if (_freeCount == 0) {
			EnsureEntityCapacity((uint32_t)_entityCapacity);
		}
		uint32_t index = _freeIndices[--_freeCount];
		return Entity{.index = index, .generation = _generations[index]};
	}
}

void Registry::Destroy(Entity entity) {
	ZHLN_LOCK(sync.shadowLock) {
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
	}
}

bool Registry::IsAlive(Entity entity) const noexcept {
	return entity.index < _entityCapacity && _generations[entity.index] == entity.generation;
}

void Registry::Clear() {
	ZHLN_LOCK(sync.shadowLock) {
		for (size_t i = 0; i < _compCapacity; ++i) {
			if (_components[i] != nullptr) {
				_components[i]->Clear();
			}
		}
		_freeCount = 0;
		_entityCount = 0; // Reset active entity tracking
		for (size_t i = 0; i < _entityCapacity; ++i) {
			_generations[i]++;
			_freeIndices[_freeCount++] = (uint32_t)i;
		}
	}
}

uint32_t Registry::RegisterComponentDynamic(std::string_view name, size_t size, size_t alignment) {
	uint32_t id = GetFamilyIDFromName(name);
	ZHLN_LOCK(sync.shadowLock) {
		// Return early if already mapped
		if (id != 0xFFFFFFFF) {
			return id;
		}

		// Resolve a unique dynamic dense ID using the name hash
		uint32_t typeHash = HashTypeName(name);
		id = ComponentFamily::ResolveDenseID(typeHash);
		MapNameToFamilyID(name, id);

		EnsureComponentCapacity(id);
		if (_components[id] == nullptr) {
			_components[id] = new SparseSet(size, alignment, &this->sync);
		}
	}
	return id;
}

void* Registry::AddDynamic(Entity entity, uint32_t familyID) {
	ZHLN_LOCK(sync.shadowLock) {
		EnsureComponentCapacity(familyID);
		if (familyID >= _compCapacity || (_components[familyID] == nullptr)) {
			return nullptr;
		}
	}
	return _components[familyID]->InsertEmpty(entity);
}

} // namespace ZHLN::ECS
