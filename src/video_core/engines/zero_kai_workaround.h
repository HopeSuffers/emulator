// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <span>

#include "common/common_types.h"

namespace Tegra::Engines::Workarounds {

constexpr u64 ZeroKaiTitleId = 0x01005E5013862000;
constexpr std::array<u8, 32> ZeroKai101BuildId{
    0xec, 0xde, 0xeb, 0xa2, 0x66, 0x13, 0x03, 0xb1, 0x1b, 0x0d,
    0x3e, 0x24, 0x6c, 0x86, 0xc9, 0x94, 0x07, 0xc5, 0x08, 0x73,
};

// Zero Kai HK 1.01 indexes a 0xff queue sentinel without checking it. The resulting
// out-of-bounds byte store changes its temporary NVN view's layer count from 1 to
// 0x101. This predicate deliberately matches only the observed render target; it
// is not a mask for array counts and must not be applied to ordinary 257-layer images.
[[nodiscard]] constexpr bool IsZeroKaiCorruptedLayerCount(u64 title_id,
                                                          const std::array<u8, 32>& build_id,
                                                          u32 dimension,
                                                          std::span<const u32, 10> rt) {
    return title_id == ZeroKaiTitleId && build_id == ZeroKai101BuildId && dimension == 0x101 &&
           rt[2] == 1920 && rt[3] == 1080 && rt[4] == 0xc6 && rt[5] == 0x40 && rt[7] == 0x438000 &&
           rt[8] == 0;
}

} // namespace Tegra::Engines::Workarounds
