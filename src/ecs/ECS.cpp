#include "ECS.hpp"

#include "detail/ControlFlow.hpp"

#include <Zahlen/Log.hpp>
#include <cstring>
#include <new>

namespace ZHLN::ECS {

// ============================================================================
// Memory Utilities
// ============================================================================
template <typename T> [[nodiscard]] static T* AllocateAligned(size_t count, size_t alignment) {
	if (count == 0)
		return nullptr;
	return static_cast<T*>(::operator new[](count * sizeof(T), std::align_val_t{alignment}));
}

template <typename T> static void DeallocateAligned(T* ptr, size_t alignment) {
	if (ptr)
		::operator delete[](ptr, std::align_val_t{alignment});
}

// ============================================================================
// SparseSet
// ============================================================================

SparseSet::SparseSet(size_t elementSize, size_t alignment, BufferSync* syncPtr)
	: _elementSize(elementSize), _alignment(alignment), _sync(syncPtr) {}

SparseSet::~SparseSet() {
	DeallocateAligned(_sparse, alignof(uint32_t));
	DeallocateAligned(_dense, alignof(Entity));
	DeallocateAligned(_data, _alignment);
}

SparseSet::SparseSet(SparseSet&& other) noexcept
	: _sparse(std::exchange(other._sparse, nullptr)), _dense(std::exchange(other._dense, nullptr)),
	  _data(std::exchange(other._data, nullptr)), _elementSize(other._elementSize),
	  _alignment(other._alignment), _count(std::exchange(other._count, 0)),
	  _denseCapacity(std::exchange(other._denseCapacity, 0)),
	  _sparseCapacity(std::exchange(other._sparseCapacity, 0)) {}

SparseSet& SparseSet::operator=(SparseSet&& other) noexcept {
	if (this != &other) {
		this->~SparseSet();
		new (this) SparseSet(std::move(other));
	}
	return *this;
}

void SparseSet::EnsureSparseCapacity(uint32_t required) {
	if (_sparseCapacity > required)
		return;

	// Check if Lua is holding a pointer before we reallocate!
	if (_sync && _sync->viewExportCount.load(std::memory_order_acquire) > 0) {
		ZHLN::Panic(
			"FFI MEMORY VIOLATION: Attempted to resize SparseSet while Lua holds a BufferView!");
	}

	size_t newCap = _sparseCapacity == 0 ? 1024 : _sparseCapacity * 2;
	while (newCap <= required)
		newCap *= 2;

	uint32_t* newSparse = AllocateAligned<uint32_t>(newCap, alignof(uint32_t));

	if (_sparse) {
		std::memcpy(newSparse, _sparse, _sparseCapacity * sizeof(uint32_t));
		DeallocateAligned(_sparse, alignof(uint32_t));
	}

	// Fill the new trailing space with the invalid sentinel
	for (size_t i = _sparseCapacity; i < newCap; ++i) {
		newSparse[i] = INVALID_DENSE;
	}

	_sparse = newSparse;
	_sparseCapacity = newCap;
}

void SparseSet::EnsureDenseCapacity() {
	if (_count < _denseCapacity)
		return;

	// Check if Lua is holding a pointer before we reallocate!
	if (_sync && _sync->viewExportCount.load(std::memory_order_acquire) > 0) {
		ZHLN::Panic(
			"FFI MEMORY VIOLATION: Attempted to resize Dense Array while Lua holds a BufferView!");
	}

	size_t newCap = _denseCapacity == 0 ? 64 : _denseCapacity * 2;

	Entity* newDense = AllocateAligned<Entity>(newCap, alignof(Entity));
	std::byte* newData = AllocateAligned<std::byte>(newCap * _elementSize, _alignment);

	if (_dense) {
		std::memcpy(newDense, _dense, _count * sizeof(Entity));
		std::memcpy(newData, _data, _count * _elementSize);

		DeallocateAligned(_dense, alignof(Entity));
		DeallocateAligned(_data, _alignment);
	}

	_dense = newDense;
	_data = newData;
	_denseCapacity = newCap;
}

void SparseSet::Insert(Entity entity, const void* data) {
	EnsureSparseCapacity(entity.index);

	uint32_t denseIdx = _sparse[entity.index];

	// If it doesn't exist, append it
	if (denseIdx == INVALID_DENSE) {
		EnsureDenseCapacity();
		denseIdx = static_cast<uint32_t>(_count++);
		_dense[denseIdx] = entity;
		_sparse[entity.index] = denseIdx;
	}

	// Copy data into the contiguous array
	std::memcpy(_data + (denseIdx * _elementSize), data, _elementSize);
}

void SparseSet::Remove(Entity entity) {
	if (entity.index >= _sparseCapacity)
		return;

	uint32_t denseIdx = _sparse[entity.index];
	if (denseIdx == INVALID_DENSE)
		return;

	uint32_t lastIdx = static_cast<uint32_t>(_count - 1);

	// If it's not the last element, swap with the back to maintain dense packing
	if (denseIdx != lastIdx) {
		Entity lastEntity = _dense[lastIdx];

		// Move data
		std::memcpy(_data + (denseIdx * _elementSize), _data + (lastIdx * _elementSize),
					_elementSize);

		// Update mappings
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
	if (!Contains(entity))
		return nullptr;
	return _data + (_sparse[entity.index] * _elementSize);
}

void SparseSet::Clear() noexcept {
	if (_count > 0) {
		std::memset(_sparse, 0xFF,
					_sparseCapacity * sizeof(uint32_t)); // 0xFFFFFFFF = INVALID_DENSE
		_count = 0;
	}
}

BufferView SparseSet::GetBufferView(const void* owner, const char* format) const noexcept {
	BufferView view = {};
	view.buf = _data;
	view.obj = const_cast<void*>(owner);
	view.itemsize = static_cast<uint32_t>(_elementSize);
	std::strncpy(view.format, format, 7);
	view.readonly = 0;
	view.ndim = 1;
	view.shape[0] = _count;
	view.strides[0] = _elementSize;
	view.flags = ZHLN_BUFFER_CONTIGUOUS | ZHLN_BUFFER_WRITABLE;

	if (reinterpret_cast<uintptr_t>(_data) % 32 == 0) {
		view.flags |= ZHLN_BUFFER_ALIGNED_32;
	}

	return view;
}

BufferView SparseSet::GetEntityView(const void* owner) const noexcept {
	BufferView view = {};
	view.buf = _dense;
	view.obj = const_cast<void*>(owner);
	view.itemsize = sizeof(Entity);
	std::strncpy(view.format, "Q", 7); // 'Q' is standard Python/Lua format for uint64_t
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

Entity Registry::Create() {
	ZHLN_LOCK(sync.shadowLock) {

		if (_freeCount == 0) {
			const size_t newCap = _generations.empty() ? 1024 : _generations.size() * 2;
			const size_t oldSize = _generations.size();

			_generations.resize(newCap, 1);
			_freeIndices.resize(newCap);

			for (size_t i = oldSize; i < newCap; ++i) {
				_freeIndices[_freeCount++] = static_cast<uint32_t>((newCap - 1) - (i - oldSize));
			}
		}
		const uint32_t index = _freeIndices[--_freeCount];
		const uint32_t gen = _generations[index];
		_activeEntities++;

		return Entity{index, gen};
	}
}

void Registry::Destroy(Entity entity) {
	ZHLN_LOCK(sync.shadowLock) {

		if (entity.index >= _generations.size() ||
			_generations[entity.index] != entity.generation) {
			return; // Already dead
		}

		// 1. Remove from all active sparse sets
		for (auto* set : _components) {
			if (set)
				set->Remove(entity);
		}

		// 2. Kill Entity
		_generations[entity.index]++;
		_freeIndices[_freeCount++] = entity.index;
		_activeEntities--;
	}
}

bool Registry::IsAlive(Entity entity) const noexcept {
	ZHLN_LOCK(sync.shadowLock) {
		return entity.index < _generations.size() &&
			   _generations[entity.index] == entity.generation;
	}
}

void Registry::Clear() {
	ZHLN_LOCK(sync.shadowLock) {
		for (auto* set : _components) {
			if (set)
				set->Clear();
		}
		for (size_t i = 0; i < _generations.size(); ++i) {
			_freeIndices[i] = static_cast<uint32_t>((_generations.size() - 1) - i);
			_generations[i]++; // Invalidate all existing handles
		}
		_freeCount = _generations.size();
		_activeEntities = 0;
	}
}

} // namespace ZHLN::ECS