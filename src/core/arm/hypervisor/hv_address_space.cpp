// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>

#include "common/assert.h"
#include "common/logging.h"
#include "common/page_table.h"
#include "core/arm/hypervisor/hv_address_space.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/board/nintendo/nx/k_system_control.h"

namespace Core::Hypervisor {

namespace {

using Common::MemoryPermission;

constexpr u64 PageBits = 12;
constexpr u64 PageSize = 1ULL << PageBits;
constexpr u64 AddressMask = 0x0000'FFFF'FFFF'F000ULL;

constexpr u64 DescValid = 1ULL << 0;
constexpr u64 DescTableOrPage = 3;
constexpr u64 DescAf = 1ULL << 10;
constexpr u64 DescInnerShareable = 3ULL << 8;
constexpr u64 DescApEl0Rw = 1ULL << 6;
constexpr u64 DescApEl1Ro = 2ULL << 6;
constexpr u64 DescApRo = 3ULL << 6;
constexpr u64 DescPxn = 1ULL << 53;
constexpr u64 DescUxn = 1ULL << 54;

// Software bits, ignored by hardware: the page is mapped by the guest, and its base permission.
constexpr u64 SwPresent = 1ULL << 55;
constexpr u64 SwPermShift = 56;
constexpr u64 SwPermMask = 7ULL << SwPermShift;

u64 Load(const u64* entry) {
    return __atomic_load_n(entry, __ATOMIC_ACQUIRE);
}

void Store(u64* entry, u64 value) {
    __atomic_store_n(entry, value, __ATOMIC_RELEASE);
}

MemoryPermission PermOf(u64 leaf) {
    return static_cast<MemoryPermission>((leaf & SwPermMask) >> SwPermShift);
}

bool Has(MemoryPermission perm, MemoryPermission flag) {
    return (static_cast<u32>(perm) & static_cast<u32>(flag)) != 0;
}

u64 MakeLeaf(PAddr pa, MemoryPermission perm) {
    u64 leaf = (pa & AddressMask) | SwPresent | (static_cast<u64>(perm) << SwPermShift);
    const bool r = Has(perm, MemoryPermission::Read);
    const bool w = Has(perm, MemoryPermission::Write);
    const bool x = Has(perm, MemoryPermission::Execute);
    if (!r && !w && !x) {
        return leaf;
    }
    leaf |= DescTableOrPage | DescAf | DescInnerShareable | DescPxn;
    if (w) {
        leaf |= DescApEl0Rw;
    } else if (r) {
        leaf |= DescApRo;
    } else {
        leaf |= DescApEl1Ro;
    }
    if (!x) {
        leaf |= DescUxn;
    }
    return leaf;
}

bool NeedsFlush(u64 old_leaf, u64 new_leaf) {
    if ((old_leaf & DescValid) == 0) {
        return false;
    }
    if ((new_leaf & DescValid) == 0 || (old_leaf & AddressMask) != (new_leaf & AddressMask)) {
        return true;
    }
    const u32 old_perm = static_cast<u32>(PermOf(old_leaf));
    const u32 new_perm = static_cast<u32>(PermOf(new_leaf));
    return (old_perm & ~new_perm) != 0;
}

} // namespace

AddressSpace::AddressSpace(Core::System& system, const Common::PageTable& page_table_)
    : page_table{page_table_}, root{AllocateTable()}, verify{std::getenv("CITRON_HV_VERIFY") !=
                                                            nullptr} {
    const size_t dram_size =
        Kernel::Board::Nintendo::Nx::KSystemControl::Init::GetIntendedMemorySize();
    const bool mapped = MapDram(system.DeviceMemory().buffer.BackingBasePointer(), dram_size);
    if (!mapped) {
        LOG_CRITICAL(Core_ARM, "Guest DRAM could not be mapped into the hypervisor VM");
        std::abort();
    }
    tables.push_back(root);
    LOG_INFO(Core_ARM, "Hypervisor address space created{}", verify ? ", verifying" : "");
}

AddressSpace::~AddressSpace() {
    for (const Table& table : tables) {
        FreeTable(table);
    }
    LOG_INFO(Core_ARM, "Hypervisor address space freed: {} page tables, {} verify failures",
             tables.size(), verify_failures);
}

u64* AddressSpace::GetLeaf(VAddr va, bool create) {
    u64* table = root.host;
    for (u64 shift = 30; shift > PageBits; shift -= 9) {
        u64* entry = &table[(va >> shift) & 511];
        u64 desc = Load(entry);
        if ((desc & DescValid) == 0) {
            if (!create) {
                return nullptr;
            }
            const Table next = AllocateTable();
            tables.push_back(next);
            if (verify && tables.size() % 512 == 0) {
                LOG_INFO(Core_ARM, "Hypervisor page tables: {}", tables.size());
            }
            desc = next.ipa | DescTableOrPage;
            Store(entry, desc);
            table = next.host;
            continue;
        }
        table = TableHost(desc & AddressMask);
    }
    return &table[(va >> PageBits) & 511];
}

void AddressSpace::Map(VAddr va, PAddr pa, u64 size, MemoryPermission perm) {
    std::scoped_lock lk{mutex};
    for (u64 offset = 0; offset < size; offset += PageSize) {
        u64* leaf = GetLeaf(va + offset, true);
        const u64 new_leaf = MakeLeaf(pa + offset, perm);
        if (NeedsFlush(Load(leaf), new_leaf)) {
            flush_pending.store(true, std::memory_order_release);
        }
        Store(leaf, new_leaf);
    }
    if (verify) {
        Verify(va, size);
    }
}

void AddressSpace::Unmap(VAddr va, u64 size) {
    std::scoped_lock lk{mutex};
    for (u64 offset = 0; offset < size; offset += PageSize) {
        u64* leaf = GetLeaf(va + offset, false);
        if (!leaf) {
            continue;
        }
        if (Load(leaf) & DescValid) {
            flush_pending.store(true, std::memory_order_release);
        }
        Store(leaf, 0);
    }
    if (verify) {
        Verify(va, size);
    }
}

void AddressSpace::Protect(VAddr va, u64 size, MemoryPermission perm) {
    std::scoped_lock lk{mutex};
    for (u64 offset = 0; offset < size; offset += PageSize) {
        u64* leaf = GetLeaf(va + offset, false);
        if (!leaf) {
            continue;
        }
        const u64 old_leaf = Load(leaf);
        if ((old_leaf & SwPresent) == 0) {
            continue;
        }
        const u64 new_leaf = MakeLeaf(old_leaf & AddressMask, perm);
        if (NeedsFlush(old_leaf, new_leaf)) {
            flush_pending.store(true, std::memory_order_release);
        }
        Store(leaf, new_leaf);
    }
    if (verify) {
        Verify(va, size);
    }
}

void AddressSpace::Verify(VAddr va, u64 size) {
    for (u64 addr = va; addr < va + size; addr += PageSize) {
        const auto& entry = page_table.entries[addr >> PageBits];
        const bool mapped = entry.pointer.Type() != Common::PageType::Unmapped;
        const u64* leaf = GetLeaf(addr, false);
        const u64 desc = leaf ? Load(leaf) : 0;
        const bool present = (desc & SwPresent) != 0;
        const PAddr expected = entry.backing_addr + addr;
        if (mapped == present && (!mapped || (desc & AddressMask) == expected)) {
            continue;
        }
        if (++verify_failures <= 20) {
            LOG_ERROR(Core_ARM,
                      "Hypervisor page table mismatch at {:#x}: citron {} pa {:#x}, hv {} pa {:#x}",
                      addr, mapped ? "mapped" : "unmapped", expected,
                      present ? "mapped" : "unmapped", desc & AddressMask);
        }
    }
}

} // namespace Core::Hypervisor
