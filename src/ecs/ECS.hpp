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
#include <detail/Reflection.hpp>
#include <detail/Span.hpp>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <threading/Mutex.hpp>
#include <vector>

namespace ZHLN::ECS {

template <typename T, typename = void> struct is_complete : std::false_type {};

template <typename T> struct is_complete<T, std::void_t<decltype(sizeof(T))>> : std::true_type {};

template <typename T>
concept CompleteType = is_complete<T>::value;

struct ComponentTypeInfo {
	std::string_view name;
	size_t size = 0;
	size_t alignment = 0;
	void (*debugDump)(const void*, std::string&) = nullptr;
};

constexpr uint32_t HashTypeName(std::string_view str) {
	uint32_t hash = 2166136261u;
	for (char c : str) {
		hash ^= static_cast<uint8_t>(c);
		hash *= 16777619u;
	}
	return hash;
}

template <typename T> consteval uint32_t GetTypeHash() {
	if constexpr (requires { ZHLN::Reflect::TypeName<T>(); }) {
		return HashTypeName(ZHLN::Reflect::TypeName<T>());
	} else {
		return HashTypeName(std::source_location::current().function_name());
	}
}

template <typename T> constexpr std::string_view BoxedName() {
	if constexpr (requires { ZHLN::Reflect::GetSchemaNameOf(static_cast<T*>(nullptr)); }) {
		return ZHLN::Reflect::GetSchemaNameOf(static_cast<T*>(nullptr));
	} else if constexpr (requires { ZHLN::Reflect::TypeName<T>(); }) {
		return ZHLN::Reflect::TypeName<T>();
	} else {
		static_assert(false, "Unknown type");
	}
}

class ZHLN_API SparseSet {
  public:
	using DestructorFn = void (*)(void*);
	SparseSet(size_t elementSize, size_t alignment, BufferSync* syncPtr,
			  DestructorFn destructor = nullptr);
	~SparseSet();

	SparseSet(const SparseSet&) = delete;
	SparseSet& operator=(const SparseSet&) = delete;

	void Insert(Entity entity, const void* data);
	[[nodiscard]] void* InsertEmpty(Entity entity);
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
	DestructorFn _destructor = nullptr;

	void ResizeSparse(uint32_t required);
	void ResizeDense();
};

class ZHLN_API ComponentFamily {
  public:
	static uint32_t ResolveDenseID(uint32_t typeHash) noexcept;

	template <typename T>
		requires CompleteType<T>
	static uint32_t GetTypeID() noexcept {
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

	uint32_t RegisterComponentDynamic(std::string_view name, size_t size, size_t alignment);
	void* AddDynamic(Entity entity, uint32_t familyID);

	static void MapNameToFamilyID(std::string_view name, uint32_t id) noexcept;
	static uint32_t GetFamilyIDFromName(std::string_view name) noexcept;

	template <typename T> void RegisterComponent() { RegisterComponent<T>(BoxedName<T>()); }

	template <typename... Components> void RegisterComponents() {
		// A C++17 fold expression that expands for every component in the list
		(RegisterComponent<Components>(BoxedName<Components>()), ...);
	}

	/**
	 * @brief Automatically discovers and registers all nested component structures
	 * declared within a given container class using compile-time reflection.
	 */
	template <typename Container> void RegisterAllComponentsIn() {
		ZHLN::Reflect::ForEachNestedType<Container>(
			[this]<typename Comp>() { this->RegisterComponent<Comp>(BoxedName<Comp>()); });
	}

	template <typename T> T& Add(Entity entity, T&& component) {
		uint32_t id = ComponentFamily::GetTypeID<T>();
		EnsureComponentCapacity(id);
		if (!_components[id]) {
			typename SparseSet::DestructorFn dt = nullptr;
			if constexpr (requires(T* t) { T::OnDestroy(t); }) {
				dt = [](void* ptr) { T::OnDestroy(static_cast<T*>(ptr)); };
			}
			_components[id] = new SparseSet(sizeof(T), alignof(T), &this->sync, dt);
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

	template <typename T>
		requires CompleteType<T>
	T* Get(Entity entity) const noexcept {
		uint32_t id = ComponentFamily::GetTypeID<T>();
		if (id >= _compCapacity || !_components[id]) {
			return nullptr;
		}
		return static_cast<T*>(_components[id]->Get(entity));
	}

	template <typename T>
		requires CompleteType<T>
	ZHLN::RestrictSpan<T> GetRawArray() const noexcept {
		uint32_t id = ComponentFamily::GetTypeID<T>();
		if (id >= _compCapacity || !_components[id]) {
			ZHLN::Log("Unknown component: {}", BoxedName<T>());
			return ZHLN::RestrictSpan<T>(nullptr, 0); // Safely return an empty span
		}
		auto* set = _components[id];
		return ZHLN::RestrictSpan<T>(reinterpret_cast<T*>(set->GetDataArray()), set->Count());
	}

	template <typename T>
		requires CompleteType<T>
	std::span<const Entity> GetEntitiesWith() const noexcept {
		uint32_t id = ComponentFamily::GetTypeID<T>();
		if (id >= _compCapacity || !_components[id]) {
			return {};
		}
		return {_components[id]->GetDenseArray(), _components[id]->Count()};
	}

	// Overload: Populates _typeInfo metadata alongside sparse set allocation
	template <typename T> void RegisterComponent(std::string_view name) {
		uint32_t id = ComponentFamily::GetTypeID<T>();
		MapNameToFamilyID(name, id);
		EnsureComponentCapacity(id);

		if (!_components[id]) {
			typename SparseSet::DestructorFn dt = nullptr;
			if constexpr (requires(T* t) { T::OnDestroy(t); }) {
				dt = [](void* ptr) { T::OnDestroy(static_cast<T*>(ptr)); };
			}
			_components[id] = new SparseSet(sizeof(T), alignof(T), &this->sync, dt);
		}

		_typeInfo[id] = {
			.name = name,
			.size = sizeof(T),
			.alignment = alignof(T),
			.debugDump =
				[](const void* data, std::string& out) {
					out += ZHLN::Reflect::ToDebugString(*static_cast<const T*>(data));
				},
		};
	}

	// Inspector: Iterates and dumps active component state representations into out buffer
	void DebugDumpEntity(Entity entity, std::string& out) const {
		for (uint32_t id = 0; id < _compCapacity; ++id) {
			if (auto* raw = GetRawByFamily(entity, id)) {
				out += _typeInfo[id].name;
				out += ": ";
				if (_typeInfo[id].debugDump != nullptr) {
					_typeInfo[id].debugDump(raw, out);
				} else {
					out += "{}";
				}
				out += "\n";
			}
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
	std::vector<ComponentTypeInfo> _typeInfo;

	void EnsureEntityCapacity(uint32_t index);
	void EnsureComponentCapacity(uint32_t id);
};

} // namespace ZHLN::ECS
