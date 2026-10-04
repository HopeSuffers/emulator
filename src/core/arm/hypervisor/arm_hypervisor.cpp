// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <Hypervisor/Hypervisor.h>
#include <libkern/OSCacheControl.h>

#include "common/assert.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/arm/hypervisor/arm_hypervisor.h"
#include "core/arm/hypervisor/hv_address_space.h"
#include "core/arm/hypervisor/hv_vm.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"

namespace Core::Hypervisor {

namespace {

constexpr u64 CpsrEl1h = 0x3C5;
constexpr u64 Mair = 0xFF;
constexpr u64 Tcr = 0x00000011B5193519ULL;
constexpr u64 Sctlr = 0x34D5D925;
constexpr u64 CpacrFpen = 3ULL << 20;
constexpr u64 CounterFrequency = 19'200'000;

constexpr u32 EcUnknown = 0x00;
constexpr u32 EcSvc64 = 0x15;
constexpr u32 EcHvc64 = 0x16;
constexpr u32 EcSysReg = 0x18;
constexpr u32 EcInstructionAbortLower = 0x20;
constexpr u32 EcDataAbortLower = 0x24;
constexpr u32 EcBrk64 = 0x3C;

const bool StatsEnabled = std::getenv("CITRON_HV_STATS") != nullptr;

u32 ExceptionClass(u64 esr) {
    return static_cast<u32>((esr >> 26) & 0x3F);
}

void Check(hv_return_t result, const char* what) {
    if (result != HV_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Core_ARM, "{} failed: {:#x}", what, static_cast<u32>(result));
        std::abort();
    }
}

} // namespace

struct Vcpu {
    hv_vcpu_t id{};
    hv_vcpu_exit_t* exit{};
    u64 ttbr0{~0ULL};
    bool created{};

    ~Vcpu() {
        if (created) {
            hv_vcpu_destroy(id);
        }
    }

    u64 Reg(hv_reg_t reg) const {
        u64 value{};
        hv_vcpu_get_reg(id, reg, &value);
        return value;
    }

    void SetReg(hv_reg_t reg, u64 value) const {
        hv_vcpu_set_reg(id, reg, value);
    }

    u64 SysReg(hv_sys_reg_t reg) const {
        u64 value{};
        hv_vcpu_get_sys_reg(id, reg, &value);
        return value;
    }

    void SetSysReg(hv_sys_reg_t reg, u64 value) const {
        hv_vcpu_set_sys_reg(id, reg, value);
    }
};

namespace {

thread_local Vcpu t_vcpu;

Vcpu& CurrentVcpu() {
    if (!t_vcpu.created) {
        Check(hv_vcpu_create(&t_vcpu.id, &t_vcpu.exit, nullptr), "hv_vcpu_create");
        t_vcpu.created = true;
        t_vcpu.SetSysReg(HV_SYS_REG_MAIR_EL1, Mair);
        t_vcpu.SetSysReg(HV_SYS_REG_TCR_EL1, Tcr);
        t_vcpu.SetSysReg(HV_SYS_REG_TTBR1_EL1, KernelTtbr1());
        t_vcpu.SetSysReg(HV_SYS_REG_VBAR_EL1, KernelVa);
        t_vcpu.SetSysReg(HV_SYS_REG_CPACR_EL1, CpacrFpen);
        t_vcpu.SetSysReg(HV_SYS_REG_SCTLR_EL1, Sctlr);
        t_vcpu.SetReg(HV_REG_CPSR, CpsrEl1h);
    }
    return t_vcpu;
}

hv_reg_t XReg(size_t index) {
    return static_cast<hv_reg_t>(HV_REG_X0 + index);
}

hv_simd_fp_reg_t QReg(size_t index) {
    return static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + index);
}

} // namespace

ArmHypervisor::ArmHypervisor(System& system, Kernel::KProcess* process,
                             AddressSpace& address_space, std::size_t core_index)
    : ArmInterface{true}, m_system{system}, m_process{process}, m_address_space{address_space},
      m_core_index{core_index} {}

ArmHypervisor::~ArmHypervisor() = default;

void ArmHypervisor::LoadState(Vcpu& vcpu, bool flush) {
    if (vcpu.ttbr0 != m_address_space.RootIpa()) {
        vcpu.ttbr0 = m_address_space.RootIpa();
        vcpu.SetSysReg(HV_SYS_REG_TTBR0_EL1, vcpu.ttbr0);
        flush = true;
    }
    for (size_t i = 0; i < 29; i++) {
        vcpu.SetReg(XReg(i), m_ctx.r[i]);
    }
    vcpu.SetReg(HV_REG_FP, m_ctx.fp);
    vcpu.SetReg(HV_REG_LR, m_ctx.lr);
    vcpu.SetSysReg(HV_SYS_REG_SP_EL0, m_ctx.sp);
    for (size_t i = 0; i < 32; i++) {
        hv_simd_fp_uchar16_t value;
        std::memcpy(&value, &m_ctx.v[i], sizeof(value));
        hv_vcpu_set_simd_fp_reg(vcpu.id, QReg(i), value);
    }
    vcpu.SetReg(HV_REG_FPCR, m_ctx.fpcr);
    vcpu.SetReg(HV_REG_FPSR, m_ctx.fpsr);
    vcpu.SetSysReg(HV_SYS_REG_TPIDR_EL0, m_ctx.tpidr);
    vcpu.SetSysReg(HV_SYS_REG_TPIDRRO_EL0, m_tpidrro_el0);
    vcpu.SetReg(HV_REG_CPSR, CpsrEl1h);
    Resume(vcpu, m_ctx.pc, m_ctx.pstate & 0xF000'0000, flush);
}

void ArmHypervisor::SaveState(Vcpu& vcpu, u64 pc, u64 pstate) {
    for (size_t i = 0; i < 29; i++) {
        m_ctx.r[i] = vcpu.Reg(XReg(i));
    }
    m_ctx.fp = vcpu.Reg(HV_REG_FP);
    m_ctx.lr = vcpu.Reg(HV_REG_LR);
    m_ctx.sp = vcpu.SysReg(HV_SYS_REG_SP_EL0);
    m_ctx.pc = pc;
    m_ctx.pstate = static_cast<u32>(pstate & 0xF000'0000);
    for (size_t i = 0; i < 32; i++) {
        hv_simd_fp_uchar16_t value;
        hv_vcpu_get_simd_fp_reg(vcpu.id, QReg(i), &value);
        std::memcpy(&m_ctx.v[i], &value, sizeof(value));
    }
    m_ctx.fpcr = static_cast<u32>(vcpu.Reg(HV_REG_FPCR));
    m_ctx.fpsr = static_cast<u32>(vcpu.Reg(HV_REG_FPSR));
    m_ctx.tpidr = vcpu.SysReg(HV_SYS_REG_TPIDR_EL0);
}

void ArmHypervisor::CountExit(u64 ExitStats::* counter) {
    if (!StatsEnabled) {
        return;
    }
    m_stats.*counter += 1;
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - m_stats_start).count();
    if (seconds < 5.0) {
        return;
    }
    if (m_stats_start.time_since_epoch().count() != 0) {
        const auto rate = [seconds](u64 count) { return static_cast<double>(count) / seconds; };
        LOG_INFO(Core_ARM, "HV core {} exits/s: svc {:.0f}, sysreg {:.0f}, abort {:.0f}, "
                           "canceled {:.0f}, flush {:.0f}, read faults {:.0f}, barrier {:.0f}",
                 m_core_index, rate(m_stats.svc), rate(m_stats.sysreg), rate(m_stats.abort),
                 rate(m_stats.canceled), rate(m_stats.flush), rate(m_stats.read_fault),
                 rate(m_stats.barrier));
    }
    m_stats = {};
    m_stats_start = now;
}

bool ArmHypervisor::TakeFlush() {
    const u64 generation = m_address_space.FlushGeneration();
    bool flush = m_icache_flush.exchange(false);
    if (generation != m_flush_generation) {
        m_flush_generation = generation;
        flush = true;
    }
    return flush;
}

void ArmHypervisor::Resume(Vcpu& vcpu, u64 pc, u64 spsr, bool flush) {
    flush |= TakeFlush();
    if (flush) {
        CountExit(&ExitStats::flush);
    }
    vcpu.SetSysReg(HV_SYS_REG_ELR_EL1, pc);
    vcpu.SetSysReg(HV_SYS_REG_SPSR_EL1, spsr);
    vcpu.SetReg(HV_REG_PC, flush ? ResumeFlushStub : ResumeStub);
}

void ArmHypervisor::HandleSystemRegister(Vcpu& vcpu, u64 esr) {
    const bool is_read = (esr & 1) != 0;
    const u32 rt = static_cast<u32>((esr >> 5) & 0x1F);
    const u32 sysreg = static_cast<u32>(esr & 0x3FFC1E);
    // Op0=3, Op1=3, CRn=14, CRm=0; Op2 selects CNTFRQ/CNTPCT/CNTVCT.
    constexpr u32 CntFrq = (3 << 20) | (0 << 17) | (3 << 14) | (14 << 10) | (0 << 1);
    constexpr u32 CntPct = (3 << 20) | (1 << 17) | (3 << 14) | (14 << 10) | (0 << 1);
    constexpr u32 CntVct = (3 << 20) | (2 << 17) | (3 << 14) | (14 << 10) | (0 << 1);

    u64 value = 0;
    if (is_read && sysreg == CntFrq) {
        value = CounterFrequency;
    } else if (is_read && (sysreg == CntPct || sysreg == CntVct)) {
        value = m_system.CoreTiming().GetClockTicks();
    } else {
        LOG_WARNING(Core_ARM, "Unhandled system register {} {:#x} (esr={:#x})",
                    is_read ? "read" : "write", sysreg, esr);
    }
    if (is_read && rt != 31) {
        vcpu.SetReg(XReg(rt), value);
    }
}

ArmHypervisor::AbortAction ArmHypervisor::HandleDataAbort(u64 far, u64 elr, u64 esr) {
    const u64 page = far & ~Memory::CITRON_PAGEMASK;
    const bool is_write = (esr & (1ULL << 6)) != 0;
    if (m_address_space.WaitReadBarrier(page)) {
        CountExit(&ExitStats::barrier);
        return AbortAction::Retry;
    }
    if (!is_write) {
        CountExit(&ExitStats::read_fault);
        if (m_address_space.Permits(page, false)) {
            return AbortAction::Retry;
        }
    }
    if (page == m_last_fault_page) {
        if (++m_fault_repeats > 1000) {
            LOG_WARNING(Core_ARM, "Page {:#x} keeps faulting at pc={:#x}, untracking it", page,
                        elr);
            m_address_space.Untrack(page);
            m_fault_repeats = 0;
        }
    } else {
        m_last_fault_page = page;
        m_fault_repeats = 0;
    }
    if (m_address_space.IsTracked(page) &&
        m_process->GetMemory().InvalidateNCE(page, Memory::CITRON_PAGESIZE)) {
        return AbortAction::Retry;
    }
    if (m_address_space.Permits(page, is_write)) {
        return AbortAction::Retry;
    }
    if (Settings::values.cpuopt_ignore_memory_aborts.GetValue()) {
        LOG_ERROR(Core_ARM, "Ignoring guest data abort: pc={:#x}, addr={:#x}", elr, far);
        return AbortAction::Skip;
    }
    LOG_CRITICAL(Core_ARM, "Guest data abort: pc={:#x}, addr={:#x}", elr, far);
    return AbortAction::Halt;
}

HaltReason ArmHypervisor::RunThread(Kernel::KThread* thread) {
    Vcpu& vcpu = CurrentVcpu();
    m_address_space.SetRunningVcpu(m_core_index, vcpu.id + 1);
    if (m_interrupt.exchange(false)) {
        m_address_space.SetRunningVcpu(m_core_index, 0);
        return HaltReason::BreakLoop;
    }

    LoadState(vcpu, false);

    HaltReason hr{};
    while (true) {
        Check(hv_vcpu_run(vcpu.id), "hv_vcpu_run");
        const hv_vcpu_exit_t& exit = *vcpu.exit;

        if (exit.reason == HV_EXIT_REASON_CANCELED) {
            CountExit(&ExitStats::canceled);
            const u64 cpsr = vcpu.Reg(HV_REG_CPSR);
            const u64 pc = vcpu.Reg(HV_REG_PC);
            if ((cpsr & 0xF) == 0) {
                if (m_interrupt.exchange(false)) {
                    SaveState(vcpu, pc, cpsr);
                    hr = HaltReason::BreakLoop;
                    break;
                }
                if (TakeFlush()) {
                    vcpu.SetReg(HV_REG_CPSR, CpsrEl1h);
                    Resume(vcpu, pc, cpsr, true);
                }
                continue;
            }
            if (pc >= ResumeStub && pc < ResumeStubEnd && m_interrupt.exchange(false)) {
                if (pc != ResumeStub) {
                    m_icache_flush.store(true);
                }
                SaveState(vcpu, vcpu.SysReg(HV_SYS_REG_ELR_EL1),
                          vcpu.SysReg(HV_SYS_REG_SPSR_EL1));
                hr = HaltReason::BreakLoop;
                break;
            }
            if (pc == ResumeStub && TakeFlush()) {
                vcpu.SetReg(HV_REG_PC, ResumeFlushStub);
            }
            // Otherwise inside an exception vector: the HVC that reports it runs next.
            continue;
        }

        if (exit.reason != HV_EXIT_REASON_EXCEPTION ||
            ExceptionClass(exit.exception.syndrome) != EcHvc64) {
            LOG_CRITICAL(Core_ARM, "Unexpected hypervisor exit: reason={}, syndrome={:#x}",
                         static_cast<u32>(exit.reason), exit.exception.syndrome);
            SaveState(vcpu, vcpu.Reg(HV_REG_PC), vcpu.Reg(HV_REG_CPSR));
            hr = HaltReason::PrefetchAbort;
            break;
        }

        const u64 esr = vcpu.SysReg(HV_SYS_REG_ESR_EL1);
        const u64 elr = vcpu.SysReg(HV_SYS_REG_ELR_EL1);
        const u64 spsr = vcpu.SysReg(HV_SYS_REG_SPSR_EL1);
        u64 next_pc = elr;
        bool flush_on_resume = false;

        switch (ExceptionClass(esr)) {
        case EcSvc64:
            CountExit(&ExitStats::svc);
            m_svc = static_cast<u32>(esr & 0xFFFF);
            SaveState(vcpu, elr, spsr);
            hr = HaltReason::SupervisorCall;
            break;
        case EcSysReg:
            CountExit(&ExitStats::sysreg);
            HandleSystemRegister(vcpu, esr);
            next_pc = elr + 4;
            break;
        case EcDataAbortLower:
            CountExit(&ExitStats::abort);
            switch (HandleDataAbort(vcpu.SysReg(HV_SYS_REG_FAR_EL1), elr, esr)) {
            case AbortAction::Retry:
                flush_on_resume = true;
                break;
            case AbortAction::Skip:
                next_pc = elr + 4;
                break;
            case AbortAction::Halt:
                SaveState(vcpu, elr, spsr);
                LogBacktrace(m_process);
                hr = HaltReason::PrefetchAbort;
                break;
            }
            break;
        case EcBrk64:
            SaveState(vcpu, elr, spsr);
            if (m_system.DebuggerEnabled()) {
                hr = HaltReason::InstructionBreakpoint;
            } else {
                LogBacktrace(m_process);
                LOG_CRITICAL(Core_ARM, "Guest breakpoint: pc={:#x}, esr={:#x}", elr, esr);
                hr = HaltReason::PrefetchAbort;
            }
            break;
        case EcInstructionAbortLower:
        case EcUnknown:
        default:
            SaveState(vcpu, elr, spsr);
            LogBacktrace(m_process);
            LOG_CRITICAL(Core_ARM, "Guest exception: pc={:#x}, esr={:#x}, far={:#x}", elr, esr,
                         vcpu.SysReg(HV_SYS_REG_FAR_EL1));
            hr = HaltReason::PrefetchAbort;
            break;
        }

        if (True(hr)) {
            break;
        }
        if (m_interrupt.exchange(false)) {
            SaveState(vcpu, next_pc, spsr);
            hr = HaltReason::BreakLoop;
            break;
        }
        Resume(vcpu, next_pc, spsr, flush_on_resume);
    }

    m_address_space.SetRunningVcpu(m_core_index, 0);
    return hr;
}

HaltReason ArmHypervisor::StepThread(Kernel::KThread* thread) {
    return HaltReason::StepThread;
}

void ArmHypervisor::GetContext(Kernel::Svc::ThreadContext& ctx) const {
    ctx = m_ctx;
}

void ArmHypervisor::SetContext(const Kernel::Svc::ThreadContext& ctx) {
    m_ctx = ctx;
}

void ArmHypervisor::SetTpidrroEl0(u64 value) {
    m_tpidrro_el0 = value;
}

void ArmHypervisor::GetSvcArguments(std::span<uint64_t, 8> args) const {
    for (size_t i = 0; i < 8; i++) {
        args[i] = m_ctx.r[i];
    }
}

void ArmHypervisor::SetSvcArguments(std::span<const uint64_t, 8> args) {
    for (size_t i = 0; i < 8; i++) {
        m_ctx.r[i] = args[i];
    }
}

u32 ArmHypervisor::GetSvcNumber() const {
    return m_svc;
}

void ArmHypervisor::SignalInterrupt(Kernel::KThread* thread) {
    m_interrupt.store(true);
    if (const u64 running = m_address_space.RunningVcpu(m_core_index); running != 0) {
        hv_vcpu_t id = running - 1;
        hv_vcpus_exit(&id, 1);
    }
}

void ArmHypervisor::ClearInstructionCache() {
    m_icache_flush.store(true);
}

void ArmHypervisor::InvalidateCacheRange(u64 addr, std::size_t size) {
    auto& memory = m_process->GetMemory();
    const u64 end = addr + size;
    for (u64 page = addr & ~Memory::CITRON_PAGEMASK; page < end; page += Memory::CITRON_PAGESIZE) {
        u8* const pointer = memory.GetPointerSilent(page);
        if (!pointer) {
            continue;
        }
        const u64 begin = std::max(addr, page);
        const u64 stop = std::min(end, page + Memory::CITRON_PAGESIZE);
        sys_icache_invalidate(pointer + (begin - page), stop - begin);
    }
}

} // namespace Core::Hypervisor
