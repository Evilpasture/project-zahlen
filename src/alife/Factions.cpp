// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include <Zahlen/alife/Factions.hpp>

namespace ZHLN::ALife {

FactionRegistry::FactionRegistry(uint32_t capacity) : _capacity(capacity) {
	// Allocate space for a 2D relationship grid stored linearly
	_relations.resize(static_cast<size_t>(capacity) * capacity, 0.0f);

	// Set default self-relation to 1.0f (allied with self)
	for (uint32_t i = 0; i < capacity; ++i) {
		_relations[i * capacity + i] = 1.0f;
	}
}

uint32_t FactionRegistry::Register(std::string_view name) {
	uint32_t existing = GetID(name);
	if (existing != 0xFFFFFFFF) {
		return existing;
	}
	if (_definitions.size() < _capacity) {
		uint32_t id = static_cast<uint32_t>(_definitions.size());
		_definitions.push_back({String32(name), id});
		return id;
	}
	return 0xFFFFFFFF; // Registry full
}

uint32_t FactionRegistry::GetID(std::string_view name) const {
	for (const auto& def : _definitions) {
		// Explicitly cast to std::string_view to avoid Clang conversion ambiguity
		if (std::string_view(def.name) == name) {
			return def.id;
		}
	}
	return 0xFFFFFFFF; // Not found
}

const char* FactionRegistry::GetName(uint32_t id) const {
	if (id < _definitions.size()) {
		return _definitions[id].name.c_str();
	}
	return nullptr;
}

void FactionRegistry::SetRelation(uint32_t a, uint32_t b, float value) {
	if (a < _capacity && b < _capacity) {
		// Relationships are symmetric (a -> b is same as b -> a)
		_relations[a * _capacity + b] = value;
		_relations[b * _capacity + a] = value;
	}
}

float FactionRegistry::GetRelation(uint32_t a, uint32_t b) const {
	if (a == b) {
		return 1.0f; // Self is always allied
	}
	if (a < _capacity && b < _capacity) {
		return _relations[a * _capacity + b];
	}
	return 0.0f; // Default is neutral
}

} // namespace ZHLN::ALife