// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "core/arm/hypervisor/hv_vm.h"
#include "core/hardware_properties.h"
#include "core/memory.h"

namespace Common {
struct PageTable;
}

namespace Core {
class System;
}

namespace Core::Hypervisor {

class AddressSpace final : public Core::Memory::GuestMappingObserver {
public:
    AddressSpace(Core::System& system, const Common::PageTable& page_table);
    ~AddressSpace() override;

    void Map(VAddr va, PAddr pa, u64 size, Common::MemoryPermission perm) override;
    void Unmap(VAddr va, u64 size) override;
    void Protect(VAddr va, u64 size, Common::MemoryPermission perm) override;
    void MarkCached(VAddr va, u64 size, bool cached, bool reactive) override;
    void MarkReadBarrier(VAddr va, u64 size, bool enable) override;
    void MarkGpuModified(VAddr va, u64 size) override;
    bool SupportsReadBarriers() const override {
        return true;
    }

    bool WaitReadBarrier(VAddr va, const std::atomic<bool>& interrupt);
    void WakeBarrierWaiters();

    std::optional<u64> GpuModifiedEpoch(VAddr va);
    bool ClearGpuModified(VAddr va, u64 epoch, bool force);

    bool IsTracked(VAddr va);
    bool Permits(VAddr va, bool write);
    void Untrack(VAddr va);

    u64 RootIpa() const {
        return root.ipa;
    }

    u64 FlushGeneration() const {
        return flush_generation.load(std::memory_order_acquire);
    }

    void SetRunningVcpu(size_t core, u64 vcpu_plus_one) {
        running_vcpus[core].store(vcpu_plus_one);
    }

    u64 RunningVcpu(size_t core) const {
        return running_vcpus[core].load();
    }

private:
    u64* GetLeaf(VAddr va, bool create);
    void Update(u64* leaf, u64 new_leaf, bool& flush);
    void RequestFlush();
    void Verify(VAddr va, u64 size);
    bool OrderExclusives(PAddr pa);

    const Common::PageTable& page_table;
    u8* dram_backing{};
    Table root;
    std::vector<Table> tables;
    std::mutex mutex;
    std::condition_variable barrier_cv;
    std::unordered_map<VAddr, u32> barrier_counts;
    std::atomic<u64> flush_generation{0};
    std::unordered_map<VAddr, u64> gpu_modified_epochs;
    std::array<std::atomic<u64>, Core::Hardware::NUM_CPU_CORES> running_vcpus{};
    bool verify;
    u64 verify_failures = 0;
};

} // namespace Core::Hypervisor
