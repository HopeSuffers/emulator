// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>

#include <Hypervisor/Hypervisor.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/arm/hypervisor/hv_vm.h"

namespace Core::Hypervisor {

constexpr bool BackendImplemented = false;

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
    bool usable = is_64bit && IsAvailable();
    if (usable && !BackendImplemented) {
        LOG_WARNING(Core_ARM, "Hypervisor backend is not complete yet, using Dynarmic");
        usable = false;
    }
    Settings::SetHypervisorEnabled(usable);
}

}
