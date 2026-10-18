// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/common_types.h"

namespace Core::Hypervisor {

constexpr size_t TableSize = 0x1000;

struct Table {
    u64* host;
    u64 ipa;
};

bool IsAvailable();

void SelectForProcess(bool is_64bit);

bool MapDram(u8* backing, size_t size);

Table AllocateTable();

void FreeTable(const Table& table);

u64* TableHost(u64 ipa);

constexpr u64 KernelVa = 0xFFFF'FF80'0000'0000ULL;
constexpr u64 ResumeStub = KernelVa + 0x1000;
constexpr u64 ResumeFlushStub = KernelVa + 0x1008;
constexpr u64 ResumeStubEnd = KernelVa + 0x2000;

u64 KernelTtbr1();

}
