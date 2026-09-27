// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/engines/zero_kai_workaround.h"

using namespace Tegra::Engines::Workarounds;

TEST_CASE("Zero Kai layer workaround matches only the captured descriptor", "[video_core]") {
    std::array<u32, 10> rt{5, 0x6c680000, 1920, 1080, 0xc6, 0x40, 0x101, 0x438000, 0, 0};
    REQUIRE(IsZeroKaiCorruptedLayerCount(ZeroKaiTitleId, ZeroKai101BuildId, rt[6], rt));
    REQUIRE_FALSE(IsZeroKaiCorruptedLayerCount(ZeroKaiTitleId + 1, ZeroKai101BuildId, rt[6], rt));
    auto different_build = ZeroKai101BuildId;
    different_build[0] ^= 1;
    REQUIRE_FALSE(IsZeroKaiCorruptedLayerCount(ZeroKaiTitleId, different_build, rt[6], rt));
    for (u32 layers : {1U, 16U, 32U, 256U, 258U, 512U}) {
        REQUIRE_FALSE(IsZeroKaiCorruptedLayerCount(ZeroKaiTitleId, ZeroKai101BuildId, layers, rt));
    }
    for (size_t word : {2U, 3U, 4U, 5U, 7U, 8U}) {
        auto different_rt = rt;
        different_rt[word] ^= 1;
        REQUIRE_FALSE(
            IsZeroKaiCorruptedLayerCount(ZeroKaiTitleId, ZeroKai101BuildId, rt[6], different_rt));
    }
}
