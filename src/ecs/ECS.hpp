#pragma once

#include <Zahlen/Buffer.h>
#include <Zahlen/Entity.hpp>
#include <cstddef>
#include <detail/Span.hpp>
#include <threading/Mutex.hpp>
#include <type_traits>
#include <vector>

namespace ZHLN::ECS {

// ============================================================================
// Type-Erased Sparse Set (The Data Backend)
// ============================================================================
class SparseSet {
  public:
	SparseSet(size_t elementSize, size_t alignment);
	~SparseSet();

	// Non-copyable to prevent accidental massive allocations
	SparseSet(const SparseSet&) = delete;
	SparseSet& operator=(const SparseSet&) = delete;
	SparseSet(SparseSet&&) noexcept;
	SparseSet& operator=(SparseSet&&) noexcept;

	void Insert(Entity entity, const void* data);
	void Remove(Entity entity);
	bool Contains(Entity entity) const noexcept;
	void* Get(Entity entity) const noexcept;
	void Clear() noexcept;

	// FFI Bridges
	BufferView GetBufferView(const void* owner, const char* format) const noexcept;
	BufferView GetEntityView(const void* owner) const noexcept;

	// Direct Access (Careful!)
	[[nodiscard]] size_t Count() const noexcept { return _count; }
	[[nodiscard]] Entity* GetDenseArray() const noexcept { return _dense; }
	[[nodiscard]] std::byte* GetDataArray() const noexcept { return _data; }

  private:
	static constexpr uint32_t INVALID_DENSE = 0xFFFFFFFF;

	uint32_t* _sparse = nullptr;
	Entity* _dense = nullptr;
	std::byte* _data = nullptr;

	size_t _elementSize = 0;
	size_t _alignment = 0;

	size_t _count = 0;
	size_t _denseCapacity = 0;
	size_t _sparseCapacity = 0;

	void EnsureSparseCapacity(uint32_t required);
	void EnsureDenseCapacity();
};

// ============================================================================
// Component ID Generator
// ============================================================================
class ComponentFamily {
	static inline uint32_t _typeCounter = 0;

  public:
	template <typename T> static uint32_t GetTypeID() noexcept {
		static uint32_t id = _typeCounter++;
		return id;
	}
};

// ============================================================================
// The Registry
// ============================================================================
class Registry {
  public:
	Registry() = default;
	~Registry() = default;

	Entity Create();
	void Destroy(Entity entity);
	bool IsAlive(Entity entity) const noexcept;
	void Clear();

	// Type-safe Component API
	template <typename T> void RegisterComponent() {
		static_assert(std::is_trivially_copyable_v<T>,
					  "ECS Components must be trivial for FFI safety!");
		uint32_t id = ComponentFamily::GetTypeID<T>();
		if (id >= _components.size()) {
			_components.resize(id + 1, nullptr);
		}
		if (!_components[id]) {
			_components[id] = new SparseSet(sizeof(T), alignof(T));
		}
	}

	template <typename T> T& Add(Entity entity, const T& component) {
		uint32_t id = ComponentFamily::GetTypeID<T>();
		_components[id]->Insert(entity, &component);
		return *static_cast<T*>(_components[id]->Get(entity));
	}

	template <typename T> void Remove(Entity entity) {
		_components[ComponentFamily::GetTypeID<T>()]->Remove(entity);
	}

	template <typename T> bool Has(Entity entity) const noexcept {
		return _components[ComponentFamily::GetTypeID<T>()]->Contains(entity);
	}

	template <typename T> T* Get(Entity entity) const noexcept {
		uint32_t id = ComponentFamily::GetTypeID<T>();
		if (id >= _components.size() || !_components[id])
			return nullptr;
		return static_cast<T*>(_components[id]->Get(entity));
	}

	// Advanced: Get the raw C++ array of a component for high-speed loops
	template <typename T> ZHLN::RestrictSpan<T> GetRawArray() const noexcept {
		auto* set = _components[ComponentFamily::GetTypeID<T>()];
		return ZHLN::RestrictSpan<T>(reinterpret_cast<T*>(set->GetDataArray()), set->Count());
	}

	// Bridge for FFI
	template <typename T> BufferView GetBufferView(const char* format) const noexcept {
		return _components[ComponentFamily::GetTypeID<T>()]->GetBufferView(this, format);
	}

  private:
	std::vector<uint32_t> _generations;
	std::vector<uint32_t> _freeIndices;
	size_t _freeCount = 0;
	size_t _activeEntities = 0;

	std::vector<SparseSet*> _components;

	// Keeps structural changes thread-safe
	mutable Mutex _lock;
};

} // namespace ZHLN::ECS