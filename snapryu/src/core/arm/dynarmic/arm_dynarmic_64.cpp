// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <limits>

#include "common/settings.h"
#include "core/arm/dynarmic/arm_dynarmic.h"
#include "core/arm/dynarmic/arm_dynarmic_64.h"
#include "core/arm/dynarmic/dynarmic_exclusive_monitor.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_process.h"

namespace Core {

using namespace Common::Literals;

DynarmicCallbacks64::DynarmicCallbacks64(ArmDynarmic64& parent, Kernel::KProcess* process)
    : m_parent{parent}, m_memory(process->GetMemory())
    , m_process(process), m_debugger_enabled{parent.m_system.DebuggerEnabled()}
    , m_check_memory_access{m_debugger_enabled || !Settings::values.cpuopt_ignore_memory_aborts.GetValue()}
{}

void DynarmicCallbacks64::SetPrivateMemoryView(u64 base, u64 size) noexcept {
    if (size == 0 || base > std::numeric_limits<u64>::max() - size) {
        ClearPrivateMemoryView();
        return;
    }
    m_private_memory_base = base;
    m_private_memory_end = base + size;
}

void DynarmicCallbacks64::ClearPrivateMemoryView() noexcept {
    m_private_memory_base = 0;
    m_private_memory_end = 0;
}

bool DynarmicCallbacks64::ReadMemoryWithPrivateView(u64 vaddr, void* output,
                                                    std::size_t size) {
    if (size == 0) {
        return true;
    }
    if (vaddr > std::numeric_limits<u64>::max() - size) {
        return false;
    }

    const u64 request_end = vaddr + size;
    if (m_private_memory_base == 0 || request_end <= m_private_memory_base ||
        vaddr >= m_private_memory_end) {
        return m_memory.ReadBlock(vaddr, output, size);
    }

    auto* const bytes = static_cast<u8*>(output);
    std::size_t offset = 0;
    while (offset < size) {
        const u64 current = vaddr + offset;
        if (current >= m_private_memory_base && current < m_private_memory_end) {
            const auto chunk = static_cast<std::size_t>((std::min)(
                static_cast<u64>(size - offset), m_private_memory_end - current));
            std::memcpy(bytes + offset, reinterpret_cast<const void*>(current), chunk);
            offset += chunk;
            continue;
        }

        const u64 next =
            current < m_private_memory_base
                ? (std::min)(request_end, m_private_memory_base)
                : request_end;
        const auto chunk = static_cast<std::size_t>(next - current);
        if (!m_memory.ReadBlock(current, bytes + offset, chunk)) {
            return false;
        }
        offset += chunk;
    }
    return true;
}

bool DynarmicCallbacks64::WriteMemoryWithPrivateView(u64 vaddr, const void* input,
                                                     std::size_t size) {
    if (size == 0) {
        return true;
    }
    if (vaddr > std::numeric_limits<u64>::max() - size) {
        return false;
    }

    const u64 request_end = vaddr + size;
    if (m_private_memory_base == 0 || request_end <= m_private_memory_base ||
        vaddr >= m_private_memory_end) {
        return m_memory.WriteBlock(vaddr, input, size);
    }

    const auto* const bytes = static_cast<const u8*>(input);
    std::size_t offset = 0;
    while (offset < size) {
        const u64 current = vaddr + offset;
        if (current >= m_private_memory_base && current < m_private_memory_end) {
            const auto chunk = static_cast<std::size_t>((std::min)(
                static_cast<u64>(size - offset), m_private_memory_end - current));
            std::memcpy(reinterpret_cast<void*>(current), bytes + offset, chunk);
            offset += chunk;
            continue;
        }

        const u64 next =
            current < m_private_memory_base
                ? (std::min)(request_end, m_private_memory_base)
                : request_end;
        const auto chunk = static_cast<std::size_t>(next - current);
        if (!m_memory.WriteBlock(current, bytes + offset, chunk)) {
            return false;
        }
        offset += chunk;
    }
    return true;
}

u8 DynarmicCallbacks64::MemoryRead8(u64 vaddr) {
    CheckMemoryAccess(vaddr, 1, Kernel::DebugWatchpointType::Read);
    if (m_private_memory_base == 0) {
        return m_memory.Read8(vaddr);
    }
    u8 value{};
    ReadMemoryWithPrivateView(vaddr, &value, sizeof(value));
    return value;
}
u16 DynarmicCallbacks64::MemoryRead16(u64 vaddr) {
    CheckMemoryAccess(vaddr, 2, Kernel::DebugWatchpointType::Read);
    if (m_private_memory_base == 0) {
        return m_memory.Read16(vaddr);
    }
    u16 value{};
    ReadMemoryWithPrivateView(vaddr, &value, sizeof(value));
    return value;
}
u32 DynarmicCallbacks64::MemoryRead32(u64 vaddr) {
    CheckMemoryAccess(vaddr, 4, Kernel::DebugWatchpointType::Read);
    if (m_private_memory_base == 0) {
        return m_memory.Read32(vaddr);
    }
    u32 value{};
    ReadMemoryWithPrivateView(vaddr, &value, sizeof(value));
    return value;
}
u64 DynarmicCallbacks64::MemoryRead64(u64 vaddr) {
    CheckMemoryAccess(vaddr, 8, Kernel::DebugWatchpointType::Read);
    if (m_private_memory_base == 0) {
        return m_memory.Read64(vaddr);
    }
    u64 value{};
    ReadMemoryWithPrivateView(vaddr, &value, sizeof(value));
    return value;
}
Dynarmic::A64::Vector DynarmicCallbacks64::MemoryRead128(u64 vaddr) {
    CheckMemoryAccess(vaddr, 16, Kernel::DebugWatchpointType::Read);
    if (m_private_memory_base == 0) {
        return {m_memory.Read64(vaddr), m_memory.Read64(vaddr + 8)};
    }
    std::array<u64, 2> value{};
    ReadMemoryWithPrivateView(vaddr, value.data(), sizeof(value));
    return {value[0], value[1]};
}

std::optional<u32> DynarmicCallbacks64::MemoryReadCode(u64 vaddr) {
    // P3E: the private x18 runner uses Dynarmic single-step, whose translator fetches exactly the
    // current PC once. Return the original guest opcode directly for that active override. This
    // avoids the former unconditional 4 KiB ReadBlock performed before every direct-x18 Step().
    if (m_single_instruction_override_active && vaddr == m_single_instruction_override_pc) {
        return m_single_instruction_override_instruction;
    }

    if (!m_memory.IsValidVirtualAddressRange(vaddr, sizeof(u32)))
        return std::nullopt;
    auto const aligned_vaddr = vaddr & ~Core::Memory::YUZU_PAGEMASK;
    if (last_code_addr != aligned_vaddr) {
        m_memory.ReadBlock(aligned_vaddr, &cached_code_page, sizeof(cached_code_page));
        last_code_addr = aligned_vaddr;
    }
    return cached_code_page.inst[(vaddr & Core::Memory::YUZU_PAGEMASK) / sizeof(u32)];
}

void DynarmicCallbacks64::MemoryWrite8(u64 vaddr, u8 value) {
    if (CheckMemoryAccess(vaddr, 1, Kernel::DebugWatchpointType::Write)) {
        if (m_private_memory_base == 0) {
            m_memory.Write8(vaddr, value);
        } else {
            WriteMemoryWithPrivateView(vaddr, &value, sizeof(value));
        }
    }
}
void DynarmicCallbacks64::MemoryWrite16(u64 vaddr, u16 value) {
    if (CheckMemoryAccess(vaddr, 2, Kernel::DebugWatchpointType::Write)) {
        if (m_private_memory_base == 0) {
            m_memory.Write16(vaddr, value);
        } else {
            WriteMemoryWithPrivateView(vaddr, &value, sizeof(value));
        }
    }
}
void DynarmicCallbacks64::MemoryWrite32(u64 vaddr, u32 value) {
    if (CheckMemoryAccess(vaddr, 4, Kernel::DebugWatchpointType::Write)) {
        if (m_private_memory_base == 0) {
            m_memory.Write32(vaddr, value);
        } else {
            WriteMemoryWithPrivateView(vaddr, &value, sizeof(value));
        }
    }
}
void DynarmicCallbacks64::MemoryWrite64(u64 vaddr, u64 value) {
    if (CheckMemoryAccess(vaddr, 8, Kernel::DebugWatchpointType::Write)) {
        if (m_private_memory_base == 0) {
            m_memory.Write64(vaddr, value);
        } else {
            WriteMemoryWithPrivateView(vaddr, &value, sizeof(value));
        }
    }
}
void DynarmicCallbacks64::MemoryWrite128(u64 vaddr, Dynarmic::A64::Vector value) {
    if (CheckMemoryAccess(vaddr, 16, Kernel::DebugWatchpointType::Write)) {
        if (m_private_memory_base == 0) {
            m_memory.Write64(vaddr, value[0]);
            m_memory.Write64(vaddr + 8, value[1]);
        } else {
            const std::array<u64, 2> words{value[0], value[1]};
            WriteMemoryWithPrivateView(vaddr, words.data(), sizeof(words));
        }
    }
}

bool DynarmicCallbacks64::MemoryWriteExclusive8(u64 vaddr, std::uint8_t value, std::uint8_t expected) {
    return CheckMemoryAccess(vaddr, 1, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive8(vaddr, value, expected);
}
bool DynarmicCallbacks64::MemoryWriteExclusive16(u64 vaddr, std::uint16_t value, std::uint16_t expected) {
    return CheckMemoryAccess(vaddr, 2, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive16(vaddr, value, expected);
}
bool DynarmicCallbacks64::MemoryWriteExclusive32(u64 vaddr, std::uint32_t value, std::uint32_t expected) {
    return CheckMemoryAccess(vaddr, 4, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive32(vaddr, value, expected);
}
bool DynarmicCallbacks64::MemoryWriteExclusive64(u64 vaddr, std::uint64_t value, std::uint64_t expected) {
    return CheckMemoryAccess(vaddr, 8, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive64(vaddr, value, expected);
}
bool DynarmicCallbacks64::MemoryWriteExclusive128(u64 vaddr, Dynarmic::A64::Vector value, Dynarmic::A64::Vector expected) {
    return CheckMemoryAccess(vaddr, 16, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive128(vaddr, value, expected);
}

void DynarmicCallbacks64::InstructionCacheOperationRaised(Dynarmic::A64::InstructionCacheOperation op, u64 value) {
    last_code_addr = u64(-1); //invalidate cached page
    switch (op) {
    case Dynarmic::A64::InstructionCacheOperation::InvalidateByVAToPoU: {
        static constexpr u64 ICACHE_LINE_SIZE = 64;
        const u64 cache_line_start = value & ~(ICACHE_LINE_SIZE - 1);
        m_parent.InvalidateCacheRange(cache_line_start, ICACHE_LINE_SIZE);
        break;
    }
    case Dynarmic::A64::InstructionCacheOperation::InvalidateAllToPoU:
        m_parent.ClearInstructionCache();
        break;
    case Dynarmic::A64::InstructionCacheOperation::InvalidateAllToPoUInnerSharable:
    default:
        LOG_DEBUG(Core_ARM, "Unprocesseed instruction cache operation: {}", op);
        break;
    }
    m_parent.m_jit->HaltExecution(Dynarmic::HaltReason::CacheInvalidation);
}

void DynarmicCallbacks64::ExceptionRaised(u64 pc, Dynarmic::A64::Exception exception) {
    switch (exception) {
    case Dynarmic::A64::Exception::WaitForInterrupt:
    case Dynarmic::A64::Exception::WaitForEvent:
    case Dynarmic::A64::Exception::SendEvent:
    case Dynarmic::A64::Exception::SendEventLocal:
    case Dynarmic::A64::Exception::Yield:
        LOG_TRACE(Core_ARM, "ExceptionRaised(exception = {}, pc = {:08X}, code = {:08X}, cached = {:08X})", std::size_t(exception), pc, m_memory.Read32(pc), MemoryReadCode(pc).value_or(0));
        return;
    case Dynarmic::A64::Exception::NoExecuteFault:
        LOG_CRITICAL(Core_ARM, "Cannot execute instruction at unmapped address {:#016x}", pc);
        ReturnException(pc, PrefetchAbort);
        return;
    default:
        if (m_debugger_enabled) {
            ReturnException(pc, InstructionBreakpoint);
        } else if (m_parent.m_nce_fallback) {
            // R2c-A: LogBacktrace queries the guest page table under a kernel lock, which may wait
            // and reschedule this guest thread inside PhysicalCore's execution context.
            LOG_CRITICAL(Core_ARM, "ExceptionRaised(exception = {}, pc = {:08X}, code = {:08X}) in NCE fallback, backtrace skipped", static_cast<std::size_t>(exception), pc, m_memory.Read32(pc));
        } else {
            m_parent.LogBacktrace(m_process);
            LOG_CRITICAL(Core_ARM, "ExceptionRaised(exception = {}, pc = {:08X}, code = {:08X})", static_cast<std::size_t>(exception), pc, m_memory.Read32(pc));
        }
    }
}

void DynarmicCallbacks64::CallSVC(u32 svc) {
    m_parent.m_svc = svc;
    m_parent.m_jit->HaltExecution(SupervisorCall);
}

void DynarmicCallbacks64::AddTicks(u64 ticks) {
    ASSERT(!m_parent.m_uses_wall_clock && "Dynarmic ticking disabled");
    // Divide the number of ticks by the amount of CPU cores. TODO(Subv): This yields only a
    // rough approximation of the amount of executed ticks in the system, it may be thrown off
    // if not all cores are doing a similar amount of work. Instead of doing this, we should
    // device a way so that timing is consistent across all cores without increasing the ticks 4
    // times.
    u64 amortized_ticks = ticks / Core::Hardware::NUM_CPU_CORES;
    // Always execute at least one tick.
    amortized_ticks = std::max<u64>(amortized_ticks, 1);
    m_parent.m_system.CoreTiming().AddTicks(amortized_ticks);
}

u64 DynarmicCallbacks64::GetTicksRemaining() {
    ASSERT(!m_parent.m_uses_wall_clock && "Dynarmic ticking disabled");
    return std::max<s64>(m_parent.m_system.CoreTiming().downcount, 0);
}

u64 DynarmicCallbacks64::GetCNTPCT() {
    return m_parent.m_system.CoreTiming().GetClockTicks();
}

bool DynarmicCallbacks64::CheckMemoryAccess(u64 addr, u64 size, Kernel::DebugWatchpointType type) {
    if (!m_check_memory_access) {
        return true;
    }

    if (!m_memory.IsValidVirtualAddressRange(addr, size)) {
        LOG_CRITICAL(Core_ARM, "Stopping execution due to unmapped memory access at {:#x}",
                        addr);
        m_parent.m_jit->HaltExecution(PrefetchAbort);
        return false;
    }

    if (!m_debugger_enabled) {
        return true;
    }

    const auto match{m_parent.MatchingWatchpoint(addr, size, type)};
    if (match) {
        m_parent.m_halted_watchpoint = match;
        m_parent.m_jit->HaltExecution(DataAbort);
        return false;
    }

    return true;
}

void DynarmicCallbacks64::ReturnException(u64 pc, Dynarmic::HaltReason hr) {
    m_parent.GetContext(m_parent.m_breakpoint_context);
    m_parent.m_breakpoint_context.pc = pc;
    m_parent.m_jit->HaltExecution(hr);
}

void ArmDynarmic64::MakeJit(Common::PageTable* page_table, std::size_t address_space_bits) {
    Dynarmic::A64::UserConfig config;

    // Callbacks
    config.callbacks = std::addressof(*m_cb);

    // Memory
    if (page_table) {
        constexpr size_t PageLog2Stride = 5;
        static_assert(1 << PageLog2Stride == sizeof(Common::PageTable::PageEntryData));

        config.page_table = reinterpret_cast<void**>(page_table->entries.data());
        config.page_table_address_space_bits = std::uint32_t(address_space_bits);
        config.page_table_pointer_mask_bits = Common::PageTable::ATTRIBUTE_BITS;
        config.page_table_log2_stride = PageLog2Stride;
        config.silently_mirror_page_table = false;
        config.absolute_offset_page_table = true;
        config.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
        config.only_detect_misalignment_via_page_table_on_page_boundary = true;

        config.fastmem_pointer = page_table->fastmem_arena ?
            std::optional<uintptr_t>{reinterpret_cast<uintptr_t>(page_table->fastmem_arena)} :
            std::nullopt;
        config.fastmem_address_space_bits = std::uint32_t(address_space_bits);
        config.silently_mirror_fastmem = false;

        config.fastmem_exclusive_access = config.fastmem_pointer != std::nullopt;
        config.recompile_on_exclusive_fastmem_failure = true;
    }

    // Multi-process state
    config.processor_id = std::uint8_t(m_core_index);
    config.global_monitor = &m_exclusive_monitor.monitor;

    // System registers
    config.tpidrro_el0 = &m_cb->m_tpidrro_el0;
    config.tpidr_el0 = &m_cb->m_tpidr_el0;
    config.dczid_el0 = 4;
    config.ctr_el0 = 0x8444c004;
    config.cntfrq_el0 = Hardware::CNTFREQ;

    // Unpredictable instructions
    config.define_unpredictable_behaviour = true;

    // Timing
    config.wall_clock_cntpct = m_uses_wall_clock;
    config.enable_cycle_counting = !m_uses_wall_clock;

    // Code cache size
#if defined(ARCHITECTURE_arm64) || defined(__sun__) || defined(__NetBSD__) || defined(__DragonFly__) || defined(__OpenBSD__)
    config.code_cache_size = std::uint32_t(128_MiB);
#else
    config.code_cache_size = std::uint32_t(512_MiB);
#endif

    // Allow memory fault handling to work
    if (m_system.DebuggerEnabled()) {
        config.check_halt_on_memory_access = true;
    }

    // null_jit
    if (!page_table) {
        // Don't waste too much memory on null_jit
        config.code_cache_size = std::uint32_t(8_MiB);
    }

    switch (Settings::values.cpu_accuracy.GetValue()) {
    // Debug mode
    case Settings::CpuAccuracy::Debugging:
        if (!Settings::values.cpuopt_page_tables) {
            config.page_table = nullptr;
        }
        if (!Settings::values.cpuopt_block_linking) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::BlockLinking;
        }
        if (!Settings::values.cpuopt_return_stack_buffer) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::ReturnStackBuffer;
        }
        if (!Settings::values.cpuopt_fast_dispatcher) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::FastDispatch;
        }
        if (!Settings::values.cpuopt_context_elimination) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::GetSetElimination;
        }
        if (!Settings::values.cpuopt_const_prop) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::ConstProp;
        }
        if (!Settings::values.cpuopt_misc_ir) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::MiscIROpt;
        }
        if (!Settings::values.cpuopt_reduce_misalign_checks) {
            config.only_detect_misalignment_via_page_table_on_page_boundary = false;
        }
        if (!Settings::values.cpuopt_fastmem) {
            config.fastmem_pointer = std::nullopt;
            config.fastmem_exclusive_access = false;
        }
        if (!Settings::values.cpuopt_fastmem_exclusives) {
            config.fastmem_exclusive_access = false;
        }
        if (!Settings::values.cpuopt_recompile_exclusives) {
            config.recompile_on_exclusive_fastmem_failure = false;
        }
        if (!Settings::values.cpuopt_ignore_memory_aborts) {
            config.check_halt_on_memory_access = true;
        }
        break;
    // Unsafe optimizations
    case Settings::CpuAccuracy::Unsafe:
        config.unsafe_optimizations = true;
        if (!Settings::values.cpuopt_unsafe_host_mmu) {
            config.fastmem_pointer = std::nullopt;
            config.fastmem_exclusive_access = false;
        }
        if (Settings::values.cpuopt_unsafe_unfuse_fma) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA;
        }
        if (Settings::values.cpuopt_unsafe_reduce_fp_error) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP;
        }
        if (Settings::values.cpuopt_unsafe_inaccurate_nan) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN;
        }
        if (Settings::values.cpuopt_unsafe_fastmem_check) {
            config.fastmem_address_space_bits = 64;
        }
        if (Settings::values.cpuopt_unsafe_ignore_global_monitor) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        }
        break;
    // Safe optimisations
    case Settings::CpuAccuracy::Auto:
        config.unsafe_optimizations = true;
        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA;
        config.fastmem_address_space_bits = 64;
        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        break;
    // Paranoia mode for debugging optimizations
    case Settings::CpuAccuracy::Paranoid:
        config.unsafe_optimizations = false;
        config.optimizations = Dynarmic::no_optimizations;
        break;
    case Settings::CpuAccuracy::Accurate:
    default:
        break;
    }
    if (!Settings::IsFastmemEnabled()) {
        config.fastmem_pointer = std::nullopt;
        config.fastmem_exclusive_access = false;
    }
    m_jit.emplace(config);
}

HaltReason ArmDynarmic64::RunThread(Kernel::KThread* thread) {
    m_jit->ClearExclusiveState();
    return TranslateHaltReason(m_jit->Run());
}

HaltReason ArmDynarmic64::StepThread(Kernel::KThread* thread) {
    m_jit->ClearExclusiveState();
    return TranslateHaltReason(m_jit->Step());
}

u32 ArmDynarmic64::GetSvcNumber() const {
    return m_svc;
}

void ArmDynarmic64::GetSvcArguments(std::span<uint64_t, 8> args) const {
    Dynarmic::A64::Jit const& j = *m_jit;
    for (size_t i = 0; i < 8; i++)
        args[i] = j.GetRegister(i);
}

void ArmDynarmic64::SetSvcArguments(std::span<const uint64_t, 8> args) {
    Dynarmic::A64::Jit& j = *m_jit;
    for (size_t i = 0; i < 8; i++)
        j.SetRegister(i, args[i]);
}

const Kernel::DebugWatchpoint* ArmDynarmic64::HaltedWatchpoint() const {
    return m_halted_watchpoint;
}

void ArmDynarmic64::RewindBreakpointInstruction() {
    this->SetContext(m_breakpoint_context);
}

ArmDynarmic64::ArmDynarmic64(System& system, bool uses_wall_clock, Kernel::KProcess* process,
                               DynarmicExclusiveMonitor& exclusive_monitor,
                               std::size_t core_index, bool force_memory_callbacks)
    : ArmInterface{uses_wall_clock}, m_system{system}, m_exclusive_monitor{exclusive_monitor}
    , m_cb(std::make_optional<DynarmicCallbacks64>(*this, process))
    , m_core_index{core_index}
{
    auto& page_table = process->GetPageTable().GetBasePageTable();
    auto& page_table_impl = page_table.GetImpl();
    MakeJit(force_memory_callbacks ? nullptr : &page_table_impl,
            page_table.GetAddressSpaceWidth());
}

void ArmDynarmic64::SetPrivateMemoryView(u64 base, u64 size) noexcept {
    m_cb->SetPrivateMemoryView(base, size);
}

void ArmDynarmic64::ClearPrivateMemoryView() noexcept {
    m_cb->ClearPrivateMemoryView();
}

ArmDynarmic64::~ArmDynarmic64() = default;

void ArmDynarmic64::SetTpidrroEl0(u64 value) {
    m_cb->m_tpidrro_el0 = value;
}

void ArmDynarmic64::GetContext(Kernel::Svc::ThreadContext& ctx) const {
    Dynarmic::A64::Jit const& j = *m_jit;
    auto gpr = j.GetRegisters();
    auto fpr = j.GetVectors();
    // TODO: this is inconvenient
    for (size_t i = 0; i < 29; i++)
        ctx.r[i] = gpr[i];
    ctx.fp = gpr[29];
    ctx.lr = gpr[30];
    ctx.sp = j.GetSP();
    ctx.pc = j.GetPC();
    ctx.pstate = j.GetPstate();
    ctx.v = fpr;
    ctx.fpcr = j.GetFpcr();
    ctx.fpsr = j.GetFpsr();
    ctx.tpidr = m_cb->m_tpidr_el0;
}

void ArmDynarmic64::SetContext(const Kernel::Svc::ThreadContext& ctx) {
    Dynarmic::A64::Jit& j = *m_jit;
    // TODO: this is inconvenient
    std::array<u64, 31> gpr;
    for (size_t i = 0; i < 29; i++)
        gpr[i] = ctx.r[i];
    gpr[29] = ctx.fp;
    gpr[30] = ctx.lr;
    j.SetRegisters(gpr);
    j.SetSP(ctx.sp);
    j.SetPC(ctx.pc);
    j.SetPstate(ctx.pstate);
    j.SetVectors(ctx.v);
    j.SetFpcr(ctx.fpcr);
    j.SetFpsr(ctx.fpsr);
    m_cb->m_tpidr_el0 = ctx.tpidr;
}

void ArmDynarmic64::SignalInterrupt(Kernel::KThread* thread) {
    m_jit->HaltExecution(BreakLoop);
}

void ArmDynarmic64::ClearInstructionCache() {
    m_jit->ClearCache();
}

void ArmDynarmic64::InvalidateCacheRange(u64 addr, std::size_t size) {
    m_jit->InvalidateCacheRange(addr, size);
}

} // namespace Core
