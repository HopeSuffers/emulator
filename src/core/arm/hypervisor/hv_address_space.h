// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <vector>

#include "core/arm/hypervisor/hv_vm.h"
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

    u64 RootIpa() const {
        return root.ipa;
    }

    bool TakeFlushRequest() {
        return flush_pending.exchange(false, std::memory_order_acq_rel);
    }

private:
    u64* GetLeaf(VAddr va, bool create);
    void Verify(VAddr va, u64 size);

    const Common::PageTable& page_table;
    Table root;
    std::vector<Table> tables;
    std::mutex mutex;
    std::atomic<bool> flush_pending{false};
    bool verify;
    u64 verify_failures = 0;
};

} // namespace Core::Hypervisor
