// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Description.hpp>
#include <cstdint>

namespace ZHLN {

enum class PacingPolicy : uint8_t {
    PacedClosedLoop ZHLN_ANNOTATION(ZHLN::Description<"Closed-loop paced presentation (FIFO latest-ready plus present timing)"> {}) = 0,
    AdaptiveVBlank ZHLN_ANNOTATION(ZHLN::Description<"Latest-ready V-blank presentation without timing feedback"> {}),
    Decoupled ZHLN_ANNOTATION(ZHLN::Description<"Uncapped immediate presentation (benchmark mode)"> {}),
    LegacyVBlank ZHLN_ANNOTATION(ZHLN::Description<"Legacy mailbox/FIFO V-blank presentation"> {}),
};

struct PresentTimingMetrics {
    PacingPolicy policy = PacingPolicy::LegacyVBlank;
    bool hasPacedTiming = false;
    uint64_t refreshIntervalNs = 0;
    bool hasMargin = false;
    uint64_t lastPresentMarginNs = 0;
    bool variableRefresh = false;
    uint64_t lastPresentId = 0;
};

}
