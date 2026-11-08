// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Engine/VisibilityList.hpp>

#include <algorithm>

namespace ZHLN {

void VisibilityList::Clear() noexcept {
    _storage.clear();
}

void VisibilityList::Add(Entity entity) {
    _storage.push_back(entity);
}

void VisibilityList::AddRange(std::span<const Entity> entities) {
    _storage.insert(_storage.end(), entities.begin(), entities.end());
}

auto VisibilityList::AsSpan() const noexcept -> std::span<const Entity> {
    return std::span<const Entity>(_storage.data(), _storage.size());
}

auto VisibilityList::Empty() const noexcept -> bool {
    return _storage.empty();
}

auto VisibilityList::Size() const noexcept -> size_t {
    return _storage.size();
}

} // namespace ZHLN
