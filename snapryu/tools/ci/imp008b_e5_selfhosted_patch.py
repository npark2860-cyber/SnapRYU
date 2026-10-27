from pathlib import Path


def read_lf(path: Path) -> str:
    return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


def write_lf(path: Path, text: str) -> None:
    path.write_bytes(text.encode("utf-8"))


transition = Path("src/core/arm/nce/windows_nce_transition.cpp")
text = read_lf(transition)
old = '''    // RtlRestoreContext converts a rejected NtContinue into an immediate fail-fast. Call the
    // underlying transition directly so the arbitrary-PC guest restore and exception continuation
    // share the same Windows context-resume primitive.
    std::fputs("IMP008B_E2_BEFORE_NT_CONTINUE=PASS\\n", stderr);
    std::fflush(stderr);
    WindowsNceTransition::ContinueContext(context);
'''
new = '''    // E5 checkout-only baseline: keep the already-tested inline initial NtContinue form.
    using NtContinueFn = LONG(NTAPI*)(PCONTEXT, BOOLEAN);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto nt_continue = ntdll != nullptr
                                 ? reinterpret_cast<NtContinueFn>(GetProcAddress(ntdll, "NtContinue"))
                                 : nullptr;
    if (nt_continue == nullptr) {
        std::fprintf(stderr, "IMP008B_E2_NT_CONTINUE_RESOLVE_ERROR=%lu\\n",
                     static_cast<unsigned long>(GetLastError()));
        std::fflush(stderr);
        std::abort();
    }

    std::fputs("IMP008B_E2_BEFORE_NT_CONTINUE=PASS\\n", stderr);
    std::fflush(stderr);
    const LONG status = nt_continue(reinterpret_cast<PCONTEXT>(&context), FALSE);
    std::fprintf(stderr, "IMP008B_E2_NT_CONTINUE_RETURN=0x%08lX\\n",
                 static_cast<unsigned long>(status));
    std::fflush(stderr);
    std::abort();
'''
if text.count(old) != 1:
    raise SystemExit(f"inline NtContinue restore anchor count={text.count(old)}")
write_lf(transition, text.replace(old, new, 1))

runtime = Path("src/core/arm/nce/arm_nce_windows.cpp")
rtext = read_lf(runtime)
if "#include <cstdio>\n" not in rtext:
    include_anchor = "#include <cstdint>\n"
    if rtext.count(include_anchor) != 1:
        raise SystemExit("cstdio include anchor mismatch")
    rtext = rtext.replace(include_anchor, include_anchor + "#include <cstdio>\n", 1)

old = (
    "    if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT &&\n"
    "        NCE::WindowsX18FallbackTrap::FindOriginalInstruction(\n"
    "            context.Pc, process->GetPostHandlers()).has_value()) {\n"
    "        params->lock.store(SpinLockLocked, std::memory_order_release);\n"
    "        if (NCE::WindowsX18FallbackTrap::TryRedirect(exception, *guest,\n"
    "                                                     process->GetPostHandlers())) {\n"
    "            NCE::WindowsNceTransition::ContinueContext(context);\n"
    "        }\n"
    "        params->lock.store(SpinLockUnlocked, std::memory_order_release);\n"
    "        return EXCEPTION_CONTINUE_SEARCH;\n"
    "    }\n"
)
new = (
    "    if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT &&\n"
    "        NCE::WindowsX18FallbackTrap::FindOriginalInstruction(\n"
    "            context.Pc, process->GetPostHandlers()).has_value()) {\n"
    "        u64 imp008b_e5_phys_x18_enter{};\n"
    "        asm volatile(\"mov %0, x18\" : \"=r\"(imp008b_e5_phys_x18_enter));\n"
    "        std::fputs(\"IMP008B_E5_X18_VEH_ENTER=PASS\\n\", stderr);\n"
    "        std::fprintf(stderr,\n"
    "                     \"IMP008B_E5_X18_VEH_ENTER_STATE PC=0x%llX SP=0x%llX CTX_X18=0x%llX PHYS_X18=0x%llX\\n\",\n"
    "                     static_cast<unsigned long long>(context.Pc),\n"
    "                     static_cast<unsigned long long>(context.Sp),\n"
    "                     static_cast<unsigned long long>(context.X[18]),\n"
    "                     static_cast<unsigned long long>(imp008b_e5_phys_x18_enter));\n"
    "        std::fflush(stderr);\n"
    "        params->lock.store(SpinLockLocked, std::memory_order_release);\n"
    "        const bool imp008b_e5_redirected = NCE::WindowsX18FallbackTrap::TryRedirect(\n"
    "            exception, *guest, process->GetPostHandlers());\n"
    "        std::fprintf(stderr, \"IMP008B_E5_X18_TRYREDIRECT=%s\\n\",\n"
    "                     imp008b_e5_redirected ? \"PASS\" : \"FAIL\");\n"
    "        std::fflush(stderr);\n"
    "        if (imp008b_e5_redirected) {\n"
    "            u64 imp008b_e5_phys_x18_return{};\n"
    "            asm volatile(\"mov %0, x18\" : \"=r\"(imp008b_e5_phys_x18_return));\n"
    "            std::fprintf(stderr,\n"
    "                         \"IMP008B_E5_X18_REDIRECT_STATE PC=0x%llX SP=0x%llX CTX_X18=0x%llX PHYS_X18=0x%llX\\n\",\n"
    "                         static_cast<unsigned long long>(context.Pc),\n"
    "                         static_cast<unsigned long long>(context.Sp),\n"
    "                         static_cast<unsigned long long>(context.X[18]),\n"
    "                         static_cast<unsigned long long>(imp008b_e5_phys_x18_return));\n"
    "            std::fputs(\"IMP008B_E5_X18_VEH_RETURN=PASS\\n\", stderr);\n"
    "            std::fflush(stderr);\n"
    "            return EXCEPTION_CONTINUE_EXECUTION;\n"
    "        }\n"
    "        params->lock.store(SpinLockUnlocked, std::memory_order_release);\n"
    "        return EXCEPTION_CONTINUE_SEARCH;\n"
    "    }\n"
)
if rtext.count(old) != 1:
    raise SystemExit(f"x18 VEH observer anchor count={rtext.count(old)}")
write_lf(runtime, rtext.replace(old, new, 1))
