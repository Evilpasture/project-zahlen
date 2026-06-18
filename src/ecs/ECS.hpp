// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Buffer.h>
#include <Zahlen/Common.h>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Sync.hpp>
#include <cstddef>
#include <detail/HashMap.hpp>
#include <detail/Span.hpp>
#include <source_location>
#include <span>
#include <string_view>
#include <threading/Mutex.hpp>

namespace ZHLN::ECS {

consteval uint32_t HashTypeName(std::string_view str) {
	uint32_t hash = 2166136261u;
	for (char c : str) {
		hash ^= static_cast<uint8_t>(c);
		hash *= 16777619u;
	}
	return hash;
}

template <typename T> consteval uint32_t GetTypeHash() {
	// The resolved function name contains the template parameter (e.g., "[with T = MeshComponent]")
	return HashTypeName(std::source_location::current().function_name());
}

template <typename T> constexpr std::string_view BoxedName() {
#if defined(__clang__) || defined(__GNUC__)
	// GCC & Clang format: "constexpr std::string_view BoxedName() [with T = Type]"
	std::string_view name = __PRETTY_FUNCTION__;
	size_t start = name.find("T = ");
	if (start != std::string_view::npos) {
		start += 4;
	} else {
		return "Unknown";
	}
	size_t end = name.find_first_of("];", start);
	if (end != std::string_view::npos) {
		name = name.substr(start, end - start);
	} else {
		name = name.substr(start);
	}
#elif defined(_MSC_VER)
	// MSVC format: "auto __cdecl BoxedName<struct Type>(void)"
	std::string_view name = __FUNCSIG__;
	size_t start = name.find("BoxedName<");
	if (start != std::string_view::npos) {
		start += 10;
	} else {
		return "Unknown";
	}
	size_t end = name.find_first_of(">(", start);
	if (end != std::string_view::npos) {
		name = name.substr(start, end - start);
	} else {
		name = name.substr(start);
	}
#else
	std::string_view name = "Unknown";
#endif

	// Strip MSVC-specific 'struct' / 'class' prefixes if present
	if (name.starts_with("struct ")) {
		name.remove_prefix(7);
	} else if (name.starts_with("class ")) {
		name.remove_prefix(6);
	}

	// Strip namespace qualifiers (e.g., "ZHLN::ALife::ALifeComponent" -> "ALifeComponent")
	size_t last_colon = name.find_last_of(':');
	if (last_colon != std::string_view::npos) {
		name.remove_prefix(last_colon + 1);
	}

	return name;
}

class ZHLN_API SparseSet {
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

class ZHLN_API ComponentFamily {
  public:
	static uint32_t ResolveDenseID(uint32_t typeHash) noexcept;

	template <typename T> static uint32_t GetTypeID() noexcept {
		static uint32_t denseID = ResolveDenseID(GetTypeHash<T>());
		return denseID;
	}
};

class ZHLN_API Registry {
	friend class EntityCommandBuffer;

  public:
	mutable BufferSync sync;

	Registry();
	~Registry();

	Entity Create();
	void Destroy(Entity entity);
	bool IsAlive(Entity entity) const noexcept;
	void Clear();

	static void MapNameToFamilyID(std::string_view name, uint32_t id) noexcept;
	static uint32_t GetFamilyIDFromName(std::string_view name) noexcept;

	template <typename T> void RegisterComponent() {
		uint32_t id = ComponentFamily::GetTypeID<T>();
		EnsureComponentCapacity(id);
		if (!_components[id]) {
			_components[id] = new SparseSet(sizeof(T), alignof(T), &this->sync);
		}
	}

	template <typename... Components> void RegisterComponents() {
		// A C++17 fold expression that expands for every component in the list
		(RegisterComponent<Components>(BoxedName<Components>()), ...);
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
		uint32_t id = ComponentFamily::GetTypeID<T>();
		if (id >= _compCapacity || !_components[id]) {
			ZHLN::Log("Unknown component: {}", BoxedName<T>());
			return ZHLN::RestrictSpan<T>(nullptr, 0); // Safely return an empty span
		}
		auto* set = _components[id];
		return ZHLN::RestrictSpan<T>(reinterpret_cast<T*>(set->GetDataArray()), set->Count());
	}

	template <typename T> std::span<const Entity> GetEntitiesWith() const noexcept {
		uint32_t id = ComponentFamily::GetTypeID<T>();
		if (id >= _compCapacity || !_components[id]) {
			return {};
		}
		return {_components[id]->GetDenseArray(), _components[id]->Count()};
	}

	template <typename T> void RegisterComponent(std::string_view name) {
		uint32_t id = ComponentFamily::GetTypeID<T>();
		MapNameToFamilyID(name, id);

		EnsureComponentCapacity(id);
		if (!_components[id]) {
			_components[id] = new SparseSet(sizeof(T), alignof(T), &this->sync);
		}
	}

	std::span<const Entity> GetEntitiesByFamilyID(uint32_t familyID) const noexcept {
		if (familyID >= _compCapacity || (_components[familyID] == nullptr)) {
			return {};
		}
		return {_components[familyID]->GetDenseArray(), _components[familyID]->Count()};
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
