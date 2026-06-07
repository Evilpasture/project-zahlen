#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>

namespace ZHLN {

template <typename Key, typename Value, size_t InitialCapacity = 32> class HashMap {
	static_assert((InitialCapacity & (InitialCapacity - 1)) == 0,
				  "InitialCapacity must be a power of two!");

  public:
	HashMap() : _capacity(InitialCapacity) { AllocateStorage(); }

	~HashMap() { ClearAndFree(); }

	// Move-only semantics (prevents dangerous shallow copying of raw pointers)
	HashMap(const HashMap&) = delete;
	HashMap& operator=(const HashMap&) = delete;

	HashMap(HashMap&& other) noexcept
		: _states(other._states), _keys(other._keys), _values(other._values),
		  _capacity(other._capacity), _size(other._size) {
		other._states = nullptr;
		other._keys = nullptr;
		other._values = nullptr;
		other._capacity = 0;
		other._size = 0;
	}

	HashMap& operator=(HashMap&& other) noexcept {
		if (this != &other) {
			ClearAndFree();

			_states = other._states;
			_keys = other._keys;
			_values = other._values;
			_capacity = other._capacity;
			_size = other._size;

			other._states = nullptr;
			other._keys = nullptr;
			other._values = nullptr;
			other._capacity = 0;
			other._size = 0;
		}
		return *this;
	}

	void Insert(const Key& key, const Value& value) {
		if (_size * 2 >= _capacity) {
			Resize(_capacity * 2);
		}

		// Fast power-of-two bitwise mask instead of integer modulo (%)
		const size_t mask = _capacity - 1;
		size_t idx = Hash(key) & mask;

		while (_states[idx] == 1) {
			if (_keys[idx] == key) {
				_values[idx] = value; // Assignment operator if key already constructed
				return;
			}
			idx = (idx + 1) & mask;
		}

		// Placement new: Construct objects directly into the raw buffer block
		_states[idx] = 1;
		::new (static_cast<void*>(&_keys[idx])) Key(key);
		::new (static_cast<void*>(&_values[idx])) Value(value);
		_size++;
	}

	[[nodiscard]] const Value* Find(const Key& key) const noexcept {
		if (_capacity == 0) {
			return nullptr;
		}

		const size_t mask = _capacity - 1;
		size_t idx = Hash(key) & mask;

		// Guaranteed to terminate because load factor constraint leaves empty slots
		while (_states[idx] != 0) {
			if (_states[idx] == 1 && _keys[idx] == key) {
				return &_values[idx];
			}
			idx = (idx + 1) & mask;
		}
		return nullptr;
	}

	void Clear() noexcept {
		if (_states == nullptr) {
			return;
		}

		// Manually destroy objects in all active slots
		for (size_t i = 0; i < _capacity; ++i) {
			if (_states[i] == 1) {
				_keys[i].~Key();
				_values[i].~Value();
				_states[i] = 0;
			}
		}
		_size = 0;
	}

	[[nodiscard]] size_t Size() const noexcept { return _size; }
	[[nodiscard]] size_t Capacity() const noexcept { return _capacity; }

  private:
	void AllocateStorage() {
		_states = new uint8_t[_capacity](); // Zero-initialized status bytes

		// Allocate raw, completely uninitialized memory chunks
		_keys = static_cast<Key*>(::operator new[](_capacity * sizeof(Key)));
		_values = static_cast<Value*>(::operator new[](_capacity * sizeof(Value)));
	}

	void ClearAndFree() {
		if (_states != nullptr) {
			Clear(); // Invokes destructors on active keys/values
			delete[] _states;
			::operator delete[](_keys);
			::operator delete[](_values);
		}
	}

	void Resize(size_t new_capacity) {
		uint8_t* old_states = _states;
		Key* old_keys = _keys;
		Value* old_values = _values;
		size_t old_capacity = _capacity;

		_capacity = new_capacity;
		AllocateStorage(); // Sets up empty buffers with new capacity
		_size = 0;

		for (size_t i = 0; i < old_capacity; ++i) {
			if (old_states[i] == 1) {
				Insert(old_keys[i], old_values[i]);
				// Destroy the old object after it has been safely migrated
				old_keys[i].~Key();
				old_values[i].~Value();
			}
		}

		delete[] old_states;
		::operator delete[](old_keys);
		::operator delete[](old_values);
	}

	[[nodiscard]] static constexpr size_t HashRawBytes(const char* str, size_t length) noexcept {
#if INTPTR_MAX == INT64_MAX
		constexpr size_t FNV_prime = 1099511628211ULL;
		constexpr size_t FNV_offset_basis = 14695981039346656037ULL;
#else
		constexpr size_t FNV_prime = 16777619U;
		constexpr size_t FNV_offset_basis = 2166136261U;
#endif
		size_t hash = FNV_offset_basis;
		for (size_t i = 0; i < length; ++i) {
			hash ^= static_cast<size_t>(str[i]);
			hash *= FNV_prime;
		}
		return hash;
	}

	[[nodiscard]] size_t Hash(const Key& key) const noexcept {
		// Handle integers, enums, handles, and raw pointers
		if constexpr (sizeof(Key) <= 8 &&
					  (std::is_integral_v<Key> || std::is_enum_v<Key> || std::is_pointer_v<Key>)) {
			uint64_t val = 0;
			if constexpr (std::is_pointer_v<Key>) {
				val = reinterpret_cast<uint64_t>(key);
			} else {
				val = static_cast<uint64_t>(key);
			}
			// High-entropy mixing step
			uint64_t scrambled = val * 11400714819323198485ULL;
			return static_cast<size_t>(scrambled >> (64 - std::countr_zero(_capacity)));
		}
		// Handle objects that expose data() and length() (std::string, std::string_view, custom
		// spans)
		else if constexpr (requires {
							   key.data();
							   key.length();
						   }) {
			return HashRawBytes(reinterpret_cast<const char*>(key.data()),
								key.length() * sizeof(*key.data()));
		} else {
			// Static assert prevents compilations with unhandled types,
			// forcing you to maintain a clean dependency structure.
			static_assert(sizeof(Key) == 0,
						  "Unsupported Key type! Write a specialized path to avoid <functional>.");
			return 0;
		}
	}

	uint8_t* _states = nullptr; // 0 = Empty, 1 = Active
	Key* _keys = nullptr;		// Raw storage pointer
	Value* _values = nullptr;	// Raw storage pointer
	size_t _capacity = 0;
	size_t _size = 0;
};

} // namespace ZHLN
