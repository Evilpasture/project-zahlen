// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "DrawQueueManager.hpp"

namespace ZHLN {

void DrawQueueManager::Sort() {
    auto drawCount = static_cast<uint32_t>(_queues.drawQueue.size());
    if (drawCount == 0) {
        return;
    }

    _sortItems.resize(drawCount);
    _sortTemp.resize(drawCount);
    _sorted.resize(drawCount);

    for (uint32_t i = 0; i < drawCount; ++i) {
        _sortItems[i] = {.key = SortKey::Pack(_queues.drawQueue[i].material, _queues.drawQueue[i].posMesh), .payload = i};
    }

    RadixSort64(_sortItems.data(), _sortTemp.data(), drawCount);

    for (uint32_t i = 0; i < drawCount; ++i) {
        _sorted[i] = _queues.drawQueue[_sortItems[i].payload];
    }

    _queues.drawQueue.swap(_sorted);
}

}
