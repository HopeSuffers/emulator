// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>

#include "core/arm/arm_interface.h"

namespace Core {
class System;
}

namespace Core::Hypervisor {

class AddressSpace;
struct Vcpu;

class ArmHypervisor final : public ArmInterface {
public:
    ArmHypervisor(System& system, Kernel::KProcess* process, AddressSpace& address_space,
                  std::size_t core_index);
    ~ArmHypervisor() override;

    Architecture GetArchitecture() const override {
        return Architecture::AArch64;
    }

    HaltReason RunThread(Kernel::KThread* thread) override;
    HaltReason StepThread(Kernel::KThread* thread) override;

    void GetContext(Kernel::Svc::ThreadContext& ctx) const override;
    void SetContext(const Kernel::Svc::ThreadContext& ctx) override;
    void SetTpidrroEl0(u64 value) override;

    void GetSvcArguments(std::span<uint64_t, 8> args) const override;
    void SetSvcArguments(std::span<const uint64_t, 8> args) override;
    u32 GetSvcNumber() const override;

    void SignalInterrupt(Kernel::KThread* thread) override;
    void ClearInstructionCache() override;
    void InvalidateCacheRange(u64 addr, std::size_t size) override;

protected:
    const Kernel::DebugWatchpoint* HaltedWatchpoint() const override {
        return nullptr;
    }

    void RewindBreakpointInstruction() override {}

private:
    void LoadState(Vcpu& vcpu, bool flush);
    void SaveState(Vcpu& vcpu, u64 pc, u64 pstate);
    void HandleSystemRegister(Vcpu& vcpu, u64 esr);
    enum class AbortAction { Retry, Skip, Halt };
    AbortAction HandleDataAbort(u64 far, u64 elr, u64 esr);
    bool TakeFlush();
    void Resume(Vcpu& vcpu, u64 pc, u64 spsr, bool flush);

    System& m_system;
    Kernel::KProcess* m_process;
    AddressSpace& m_address_space;
    std::size_t m_core_index;

    Kernel::Svc::ThreadContext m_ctx{};
    u64 m_tpidrro_el0{};
    u32 m_svc{};

    std::atomic<bool> m_interrupt{false};
    std::atomic<bool> m_icache_flush{false};
    struct ExitStats {
        u64 svc{};
        u64 sysreg{};
        u64 abort{};
        u64 canceled{};
        u64 watchdog{};
        u64 flush{};
        u64 read_fault{};
        u64 barrier{};
    };
    void CountExit(u64 ExitStats::* counter);

    ExitStats m_stats{};
    std::chrono::steady_clock::time_point m_stats_start{};
    u64 m_flush_generation{};
    u64 m_last_fault_page{};
    u32 m_fault_repeats{};
    u64 m_gpu_fault_page{};
    u32 m_gpu_fault_repeats{};
};

} // namespace Core::Hypervisor
