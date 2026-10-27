from pathlib import Path

base = Path("tools/nce_diag/apply_cinc_checkpoint_epilogue_bypass.py").read_text(encoding="utf-8")
exec(compile(base, "<apply_cinc_checkpoint_epilogue_bypass.py>", "exec"), {})

path = Path("src/core/arm/nce/arm_nce_windows.cpp")
text = path.read_text(encoding="utf-8")
old = """                    LOG_ERROR(Core_ARM,\n                              \"IMP008_CINC_CHECKPOINT_EPILOGUE_APPLIED n={} new_sp={:016X} new_x29={:016X} new_x30={:016X}\",\n                              breadcrumb_index, m_guest_ctx.sp, m_guest_ctx.cpu_registers[29],\n                              m_guest_ctx.cpu_registers[30]);\n"""
new = old + """\n                    const u64 cinc_raw_arg = m_guest_ctx.sp + 0x38;\n                    const u64 cinc_raw_bl = cinc_caller_lr + 4;\n                    LOG_ERROR(Core_ARM,\n                              \"IMP008_CINC_CALLER_ARG_SETUP_BYPASS n={} caller_pc={:016X} bl_pc={:016X} raw_arg={:016X} old_x0={:016X} old_x30={:016X}\",\n                              breadcrumb_index, cinc_caller_lr, cinc_raw_bl, cinc_raw_arg,\n                              m_guest_ctx.cpu_registers[0], m_guest_ctx.cpu_registers[30]);\n                    m_guest_ctx.cpu_registers[0] = cinc_raw_arg;\n                    m_guest_ctx.cpu_registers[30] = cinc_raw_bl;\n                    LOG_ERROR(Core_ARM,\n                              \"IMP008_CINC_CALLER_ARG_SETUP_APPLIED n={} new_x0={:016X} new_x30={:016X}\",\n                              breadcrumb_index, m_guest_ctx.cpu_registers[0],\n                              m_guest_ctx.cpu_registers[30]);\n"""
count = text.count(old)
if count != 1:
    raise SystemExit(f"caller arg setup anchor count={count}")
path.write_text(text.replace(old, new, 1), encoding="utf-8")
print("NCE_CINC_CALLER_ARG_SETUP_BYPASS_INJECTION=PASS")
