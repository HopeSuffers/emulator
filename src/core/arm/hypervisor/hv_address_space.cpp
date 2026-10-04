// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>

#include <Hypervisor/Hypervisor.h>

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

// Software bits, ignored by hardware: mapped by the guest, its base permission, and the
// permissions taken away while the GPU caches the page.
constexpr u64 SwPresent = 1ULL << 55;
constexpr u64 SwPermShift = 56;
constexpr u64 SwPermMask = 7ULL << SwPermShift;
constexpr u64 SwNoWrite = 1ULL << 59;
constexpr u64 SwNoRead = 1ULL << 60;
constexpr u64 SwTracked = SwNoWrite | SwNoRead;

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

u64 MakeLeaf(PAddr pa, MemoryPermission perm, u64 tracked = 0) {
    u64 leaf = (pa & AddressMask) | SwPresent | (static_cast<u64>(perm) << SwPermShift) | tracked;
    const bool r = Has(perm, MemoryPermission::Read) && (tracked & SwNoRead) == 0;
    const bool w = Has(perm, MemoryPermission::Write) && (tracked & SwTracked) == 0;
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

u32 EffectivePerm(u64 leaf) {
    if ((leaf & DescValid) == 0) {
        return 0;
    }
    u32 perm = static_cast<u32>(PermOf(leaf));
    if (leaf & SwNoRead) {
        perm &= ~static_cast<u32>(MemoryPermission::Read);
    }
    if (leaf & SwTracked) {
        perm &= ~static_cast<u32>(MemoryPermission::Write);
    }
    return perm;
}

bool NeedsFlush(u64 old_leaf, u64 new_leaf) {
    if ((old_leaf & DescValid) == 0) {
        return false;
    }
    if ((new_leaf & DescValid) == 0 || (old_leaf & AddressMask) != (new_leaf & AddressMask)) {
        return true;
    }
    return (EffectivePerm(old_leaf) & ~EffectivePerm(new_leaf)) != 0;
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

void AddressSpace::Update(u64* leaf, u64 new_leaf, bool& flush) {
    flush |= NeedsFlush(Load(leaf), new_leaf);
    Store(leaf, new_leaf);
}

void AddressSpace::RequestFlush() {
    flush_generation.fetch_add(1, std::memory_order_acq_rel);
    for (const auto& running : running_vcpus) {
        if (const u64 vcpu = running.load(); vcpu != 0) {
            hv_vcpu_t id = vcpu - 1;
            hv_vcpus_exit(&id, 1);
        }
    }
}

void AddressSpace::Map(VAddr va, PAddr pa, u64 size, MemoryPermission perm) {
    bool flush = false;
    {
        std::scoped_lock lk{mutex};
        for (u64 offset = 0; offset < size; offset += PageSize) {
            Update(GetLeaf(va + offset, true), MakeLeaf(pa + offset, perm), flush);
        }
        if (verify) {
            Verify(va, size);
        }
    }
    if (flush) {
        RequestFlush();
    }
}

void AddressSpace::Unmap(VAddr va, u64 size) {
    bool flush = false;
    {
        std::scoped_lock lk{mutex};
        for (u64 offset = 0; offset < size; offset += PageSize) {
            if (u64* leaf = GetLeaf(va + offset, false)) {
                Update(leaf, 0, flush);
            }
        }
        if (verify) {
            Verify(va, size);
        }
    }
    if (flush) {
        RequestFlush();
    }
}

void AddressSpace::Protect(VAddr va, u64 size, MemoryPermission perm) {
    bool flush = false;
    {
        std::scoped_lock lk{mutex};
        for (u64 offset = 0; offset < size; offset += PageSize) {
            u64* leaf = GetLeaf(va + offset, false);
            if (!leaf) {
                continue;
            }
            const u64 old_leaf = Load(leaf);
            if (old_leaf & SwPresent) {
                Update(leaf, MakeLeaf(old_leaf & AddressMask, perm, old_leaf & SwTracked), flush);
            }
        }
        if (verify) {
            Verify(va, size);
        }
    }
    if (flush) {
        RequestFlush();
    }
}

void AddressSpace::MarkCached(VAddr va, u64 size, bool cached, bool reactive) {
    const u64 tracked = cached ? (SwNoWrite | (reactive ? SwNoRead : 0)) : 0;
    const VAddr begin = va & ~(PageSize - 1);
    const VAddr end = va + size;
    bool flush = false;
    {
        std::scoped_lock lk{mutex};
        for (VAddr addr = begin; addr < end; addr += PageSize) {
            u64* leaf = GetLeaf(addr, false);
            if (!leaf) {
                continue;
            }
            const u64 old_leaf = Load(leaf);
            if ((old_leaf & SwPresent) && (old_leaf & SwTracked) != tracked) {
                Update(leaf, MakeLeaf(old_leaf & AddressMask, PermOf(old_leaf), tracked), flush);
            }
        }
    }
    if (flush) {
        RequestFlush();
    }
}

bool AddressSpace::IsTracked(VAddr va) {
    std::scoped_lock lk{mutex};
    const u64* leaf = GetLeaf(va, false);
    return leaf && (Load(leaf) & SwTracked) != 0;
}

bool AddressSpace::Permits(VAddr va, bool write) {
    std::scoped_lock lk{mutex};
    const u64* leaf = GetLeaf(va, false);
    if (!leaf) {
        return false;
    }
    const auto flag = write ? MemoryPermission::Write : MemoryPermission::Read;
    return (EffectivePerm(Load(leaf)) & static_cast<u32>(flag)) != 0;
}

void AddressSpace::Untrack(VAddr va) {
    std::scoped_lock lk{mutex};
    if (u64* leaf = GetLeaf(va, false); leaf && (Load(leaf) & SwPresent)) {
        const u64 old_leaf = Load(leaf);
        Store(leaf, MakeLeaf(old_leaf & AddressMask, PermOf(old_leaf)));
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
