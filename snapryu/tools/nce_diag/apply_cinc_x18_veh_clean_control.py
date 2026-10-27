from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label} anchor count={count}")
    return text.replace(old, new, 1)


# Reuse the exact already-runtime-validated CINC diagnostic path first. This preserves
# witness/checkpoint/caller host-equivalents, metadata observation, reentry breadcrumbs,
# and the outside-VEH x18 FALLBACK_PRE/POST observation without duplicating that logic.
validated_path = Path("tools/nce_diag/apply_cinc_checkpoint_epilogue_bypass.py")
validated_namespace: dict[str, object] = {}
exec(
    compile(validated_path.read_text(encoding="utf-8"), str(validated_path), "exec"),
    validated_namespace,
)

path = Path("src/core/arm/nce/arm_nce_windows.cpp")
text = path.read_text(encoding="utf-8")

# CLEAN CONTROL variable 1: restore the ordinary-x18 VEH block to its pre-observer form.
veh_old = validated_namespace.get("veh_old")
veh_new = validated_namespace.get("veh_new")
if not isinstance(veh_old, str) or not isinstance(veh_new, str):
    raise SystemExit("validated x18 VEH anchors unavailable")
text = replace_once(text, veh_new, veh_old, "x18 VEH observer cleanup")

# CLEAN CONTROL variable 2: remove the earlier unknown-exception LOG_ERROR observer while
# retaining the exact chainable production behavior (EXCEPTION_CONTINUE_SEARCH).
unknown_old = """    // IMP-008A does not claim complete game fault compatibility. Unknown host/guest exception\n    // classes remain chainable instead of being swallowed by the NCE VEH.\n    return EXCEPTION_CONTINUE_SEARCH;\n"""
unknown_new = """    // Diagnostic only: preserve the existing chainable semantics, but expose the first-chance\n    // exception that would otherwise disappear when the process terminates before RunThread returns.\n    static std::atomic_uint32_t unknown_exception_count{0};\n    const u32 unknown_index = unknown_exception_count.fetch_add(1, std::memory_order_relaxed);\n    const auto& record = *exception->ExceptionRecord;\n    const u64 info0 = record.NumberParameters > 0 ? static_cast<u64>(record.ExceptionInformation[0]) : 0;\n    const u64 info1 = record.NumberParameters > 1 ? static_cast<u64>(record.ExceptionInformation[1]) : 0;\n    if (unknown_index < 16) {\n        LOG_ERROR(Core_ARM,\n                  \"IMP008_UNKNOWN_GUEST_EXCEPTION n={} code={:08X} flags={:08X} address={:016X} \"\n                  \"pc={:016X} sp={:016X} x0={:016X} x1={:016X} x2={:016X} x3={:016X} \"\n                  \"x19={:016X} x20={:016X} x29={:016X} x30={:016X} params={} info0={:016X} info1={:016X}\",\n                  unknown_index, static_cast<u32>(record.ExceptionCode),\n                  static_cast<u32>(record.ExceptionFlags),\n                  reinterpret_cast<u64>(record.ExceptionAddress), context.Pc, context.Sp,\n                  context.X[0], context.X[1], context.X[2], context.X[3], context.X[19],\n                  context.X[20], context.X[29], context.X[30], record.NumberParameters, info0, info1);\n    }\n\n    // IMP-008A does not claim complete game fault compatibility. Unknown host/guest exception\n    // classes remain chainable instead of being swallowed by the NCE VEH.\n    return EXCEPTION_CONTINUE_SEARCH;\n"""
text = replace_once(text, unknown_new, unknown_old, "unknown exception observer cleanup")

# CLEAN CONTROL variable 3: remove the host-stack VEH LOG_ERROR observer and restore the
# original host-stack early-out. Reentry breadcrumbs outside VEH remain intact.
host_old = """    if (nce->m_windows_break != nullptr && nce->m_windows_break->IsHostStackPointer(context.Sp)) {\n        return EXCEPTION_CONTINUE_SEARCH;\n    }\n"""
host_new = """    if (nce->m_windows_break != nullptr && nce->m_windows_break->IsHostStackPointer(context.Sp)) {\n        const auto& host_record = *exception->ExceptionRecord;\n        const u32 host_code = static_cast<u32>(host_record.ExceptionCode);\n        if (host_code != 0x40010006U && host_code != 0x4001000AU) {\n            static std::atomic_uint32_t host_stack_exception_count{0};\n            const u32 host_index = host_stack_exception_count.fetch_add(1, std::memory_order_relaxed);\n            if (host_index < 16) {\n                LOG_ERROR(Core_ARM,\n                          \"IMP008_HOST_STACK_EXCEPTION n={} code={:08X} flags={:08X} address={:016X} pc={:016X} sp={:016X} x30={:016X}\",\n                          host_index, host_code, static_cast<u32>(host_record.ExceptionFlags),\n                          reinterpret_cast<u64>(host_record.ExceptionAddress), context.Pc, context.Sp,\n                          context.X[30]);\n            }\n        }\n        return EXCEPTION_CONTINUE_SEARCH;\n    }\n"""
text = replace_once(text, host_new, host_old, "host-stack exception observer cleanup")

path.write_text(text, encoding="utf-8")
print("NCE_CINC_X18_VEH_CLEAN_CONTROL_INJECTION=PASS")
print("NCE_CINC_X18_VEH_LOGGING_REMOVED=PASS")
print("NCE_CINC_X18_FALLBACK_TRACE_RETAINED=PASS")
