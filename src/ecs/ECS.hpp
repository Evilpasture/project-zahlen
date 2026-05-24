#pragma once

#include <Zahlen/Buffer.h>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Sync.hpp>
#include <cstddef>
#include <detail/Span.hpp>
#include <span>
#include <string>
#include <threading/Mutex.hpp>
#include <unordered_map>

namespace ZHLN::ECS {

class SparseSet {
  public:
	SparseSet(size_t elementSize, size_t alignment, BufferSync* syncPtr);
	~SparseSet();

	SparseSet(const SparseSet&) = delete;
	SparseSet& operator=(const SparseSet&) = delete;

	void Insert(Entity entity, const void* data);
	void Remove(Entity entity);
	[[nodiscard]] bool Contains(Entity entity) const noexcept;
	[[nodiscard]] void* Get(Entity entity) const noexcept;
	void Clear() noexcept;

	BufferView GetBufferView(const void* owner, const char* format) const noexcept;
	BufferView GetEntityView(const void* owner) const noexcept;

	[[nodiscard]] size_t Count() const noexcept { return _count; }
	[[nodiscard]] Entity* GetDenseArray() const noexcept { return _dense; }
	[[nodiscard]] std::byte* GetDataArray() const noexcept { return _data; }

  private:
	static constexpr uint32_t INVALID_DENSE = 0xFFFFFFFF;

	uint32_t* _sparse = nullptr;
	Entity* _dense = nullptr;
	std::byte* _data = nullptr;

	size_t _elementSize;
	size_t _alignment;
	size_t _count = 0;
	size_t _denseCapacity = 0;
	size_t _sparseCapacity = 0;

	BufferSync* _sync;

	void ResizeSparse(uint32_t required);
	void ResizeDense();
};

class ComponentFamily {
	static inline uint32_t _typeCounter = 0;

  public:
	template <typename T> static uint32_t GetTypeID() noexcept {
		static uint32_t id = _typeCounter++;
		return id;
	}
};

class Registry {
  public:
	mutable BufferSync sync;

	Registry();
	~Registry();

	Entity Create();
	void Destroy(Entity entity);
	bool IsAlive(Entity entity) const noexcept;
	void Clear();

	template <typename T> void RegisterComponent() {
		uint32_t id = ComponentFamily::GetTypeID<T>();
		EnsureComponentCapacity(id);
		if (!_components[id]) {
			_components[id] = new SparseSet(sizeof(T), alignof(T), &this->sync);
		}
	}

	template <typename T> T& Add(Entity entity, T&& component) {
		uint32_t id = ComponentFamily::GetTypeID<T>();
		EnsureComponentCapacity(id);
		if (!_components[id]) {
			_components[id] = new SparseSet(sizeof(T), alignof(T), &this->sync);
		}
		_components[id]->Insert(entity, &component);
		return *static_cast<T*>(_components[id]->Get(entity));
	}

	template <typename... Ts> void Add(Entity entity, Ts&&... components) {
		(Add(entity, std::forward<Ts>(components)), ...);
	}

	template <typename T> void Remove(Entity entity) {
		uint32_t id = ComponentFamily::GetTypeID<T>();
		if (id < _compCapacity && _components[id]) {
			_components[id]->Remove(entity);
		}
	}

	template <typename T> T* Get(Entity entity) const noexcept {
		uint32_t id = ComponentFamily::GetTypeID<T>();
		if (id >= _compCapacity || !_components[id]) {
			return nullptr;
		}
		return static_cast<T*>(_components[id]->Get(entity));
	}

	template <typename T> ZHLN::RestrictSpan<T> GetRawArray() const noexcept {
		auto* set = _components[ComponentFamily::GetTypeID<T>()];
		return ZHLN::RestrictSpan<T>(reinterpret_cast<T*>(set->GetDataArray()), set->Count());
	}

	template <typename T> std::span<const Entity> GetEntitiesWith() const noexcept {
		uint32_t id = ComponentFamily::GetTypeID<T>();
		if (id >= _compCapacity || !_components[id]) {
			return {};
		}
		return {_components[id]->GetDenseArray(), _components[id]->Count()};
	}

	// Map component name strings to their unique family IDs
	inline static std::unordered_map<std::string, uint32_t> s_NameToFamilyID;

	template <typename T> void RegisterComponent(const std::string& name) {
		uint32_t id = ComponentFamily::GetTypeID<T>();
		s_NameToFamilyID[name] = id; // Store mapping

		EnsureComponentCapacity(id);
		if (!_components[id]) {
			_components[id] = new SparseSet(sizeof(T), alignof(T), &this->sync);
		}
	}

	// Direct constant-time pointer fetch
	void* GetRawByFamily(Entity entity, uint32_t familyID) const noexcept {
		if (familyID >= _compCapacity || (_components[familyID] == nullptr)) {
			return nullptr;
		}
		return _components[familyID]->Get(entity);
	}

  private:
	uint32_t* _generations = nullptr;
	uint32_t* _freeIndices = nullptr;
	size_t _entityCount = 0;
	size_t _entityCapacity = 0;
	size_t _freeCount = 0;

	SparseSet** _components = nullptr;
	size_t _compCapacity = 0;

	void EnsureEntityCapacity(uint32_t index);
	void EnsureComponentCapacity(uint32_t id);
};

} // namespace ZHLN::ECS
