// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <Hypervisor/Hypervisor.h>
#include <sys/mman.h>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/literals.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/arm/hypervisor/hv_vm.h"
#include "core/device_memory.h"

namespace Core::Hypervisor {

using namespace Common::Literals;

constexpr u64 TableIpaBase = 0x5'0000'0000ULL;
constexpr u64 KernelIpa = 0x4'C000'0000ULL;
constexpr size_t KernelPageSize = 0x4000;
constexpr size_t TableRegionSize = 1_GiB;

namespace {
std::mutex table_mutex;
u8* table_region = nullptr;
std::vector<u64> free_tables;
u64 next_table_ipa = TableIpaBase;

std::mutex dram_mutex;
u8* dram_backing = nullptr;
size_t dram_size = 0;
} // namespace

bool IsAvailable() {
    static std::once_flag once;
    static bool available = false;
    std::call_once(once, [] {
        const hv_return_t result = hv_vm_create(nullptr);
        if (result != HV_SUCCESS) {
            LOG_ERROR(Core_ARM,
                      "Hypervisor.framework unavailable (hv_vm_create returned {:#x}), "
                      "falling back to Dynarmic",
                      static_cast<u32>(result));
            return;
        }
        u32 max_vcpus = 0;
        hv_vm_get_max_vcpu_count(&max_vcpus);
        LOG_INFO(Core_ARM, "Hypervisor.framework VM created, max vCPUs {}", max_vcpus);
        available = true;
    });
    return available;
}

void SelectForProcess(bool is_64bit) {
    const bool is_selected =
        Settings::values.cpu_backend.GetValue() == Settings::CpuBackend::Hypervisor;
    if (!is_selected) {
        Settings::SetHypervisorEnabled(false);
        return;
    }
    if (!is_64bit) {
        LOG_INFO(Core_ARM, "32-bit program, using Dynarmic instead of the hypervisor");
    }
    Settings::SetHypervisorEnabled(is_64bit && IsAvailable());
}

bool MapDram(u8* backing, size_t size) {
    std::scoped_lock lk{dram_mutex};
    if (backing == dram_backing && size == dram_size) {
        return true;
    }
    if (dram_backing) {
        hv_vm_unmap(DramMemoryMap::Base, dram_size);
        dram_backing = nullptr;
    }
    const hv_return_t result = hv_vm_map(backing, DramMemoryMap::Base, size,
                                         HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
    if (result != HV_SUCCESS) {
        LOG_ERROR(Core_ARM, "hv_vm_map of guest DRAM ({:#x} bytes) failed: {:#x}", size,
                  static_cast<u32>(result));
        return false;
    }
    dram_backing = backing;
    dram_size = size;
    return true;
}

Table AllocateTable() {
    std::scoped_lock lk{table_mutex};
    if (!table_region) {
        void* region = mmap(nullptr, TableRegionSize, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        const hv_return_t result =
            region == MAP_FAILED
                ? HV_NO_RESOURCES
                : hv_vm_map(region, TableIpaBase, TableRegionSize, HV_MEMORY_READ | HV_MEMORY_WRITE);
        if (result != HV_SUCCESS) {
            LOG_CRITICAL(Core_ARM, "hv_vm_map of page tables failed: {:#x}",
                         static_cast<u32>(result));
            std::abort();
        }
        table_region = static_cast<u8*>(region);
    }
    u64 ipa;
    if (!free_tables.empty()) {
        ipa = free_tables.back();
        free_tables.pop_back();
    } else {
        if (next_table_ipa >= TableIpaBase + TableRegionSize) {
            LOG_CRITICAL(Core_ARM, "Out of page tables");
            std::abort();
        }
        ipa = next_table_ipa;
        next_table_ipa += TableSize;
    }
    const Table table{TableHost(ipa), ipa};
    std::memset(table.host, 0, TableSize);
    return table;
}

u64* TableHost(u64 ipa) {
    return reinterpret_cast<u64*>(table_region + (ipa - TableIpaBase));
}

void FreeTable(const Table& table) {
    std::scoped_lock lk{table_mutex};
    free_tables.push_back(table.ipa);
}

u64 KernelTtbr1() {
    static std::once_flag once;
    static u64 ttbr1 = 0;
    std::call_once(once, [] {
        auto* page = static_cast<u32*>(mmap(nullptr, KernelPageSize, PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (page == MAP_FAILED) {
            LOG_CRITICAL(Core_ARM, "Could not allocate the kernel page");
            std::abort();
        }
        constexpr u32 Hvc = 0xD41FFFE2;
        constexpr u32 Eret = 0xD69F03E0;
        for (size_t vector = 0; vector < 16; vector++) {
            page[vector * 0x20] = Hvc;
            page[vector * 0x20 + 1] = Eret;
        }
        const u32 resume_flush[] = {
            Eret,       // +0x1000
            0xD5033A9F, // +0x1004 dsb ishst
            0xD508831F, // tlbi vmalle1is
            0xD508711F, // ic ialluis
            0xD5033B9F, // dsb ish
            0xD5033FDF, // isb
            Eret,
        };
        std::memcpy(page + 0x1000 / 4, resume_flush, sizeof(resume_flush));
        const hv_return_t result =
            hv_vm_map(page, KernelIpa, KernelPageSize, HV_MEMORY_READ | HV_MEMORY_EXEC);
        if (result != HV_SUCCESS) {
            LOG_CRITICAL(Core_ARM, "hv_vm_map of the kernel page failed: {:#x}",
                         static_cast<u32>(result));
            std::abort();
        }

        const Table l1 = AllocateTable();
        const Table l2 = AllocateTable();
        const Table l3 = AllocateTable();
        l1.host[(KernelVa >> 30) & 511] = l2.ipa | 3;
        l2.host[(KernelVa >> 21) & 511] = l3.ipa | 3;
        for (u64 i = 0; i < 2; i++) {
            // EL1 read-only, EL0 no access, EL0 never executes.
            l3.host[i] = (KernelIpa + i * 0x1000) | 3 | (1ULL << 10) | (3ULL << 8) |
                         (2ULL << 6) | (1ULL << 54);
        }
        ttbr1 = l1.ipa;
    });
    return ttbr1;
}

}
