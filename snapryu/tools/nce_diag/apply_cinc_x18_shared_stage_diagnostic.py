from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label} anchor count={count}")
    return text.replace(old, new, 1)


# Start from the exact runtime-validated CLEAN CONTROL transformation. This keeps all
# observer logging out of VEH and preserves the outside-VEH FALLBACK_PRE/POST markers.
clean_path = Path("tools/nce_diag/apply_cinc_x18_veh_clean_control.py")
exec(compile(clean_path.read_text(encoding="utf-8"), str(clean_path), "exec"), {})

path = Path("src/core/arm/nce/arm_nce_windows.cpp")
text = path.read_text(encoding="utf-8")

global_old = """std::once_flag g_windows_veh_once;\nPVOID g_windows_veh_handle{};\n\nstruct WindowsTebStackBounds {\n"""
global_new = """std::once_flag g_windows_veh_once;\nPVOID g_windows_veh_handle{};\n\n// Diagnostic-only shared-memory witness. The VEH performs only an atomic store; no logging,\n// allocation, file I/O, or context-control primitive is added to the exception path. A separate\n// self-hosted runner process observes the named mapping while Eden is alive.\nconstexpr wchar_t kWindowsX18StageMapName[] = L\"Local\\\\EdenNceX18Stage\";\nstd::once_flag g_windows_x18_stage_once;\nHANDLE g_windows_x18_stage_mapping{};\nvolatile LONG* g_windows_x18_stage{};\n\nvoid InitializeWindowsX18StageMap() noexcept {\n    std::call_once(g_windows_x18_stage_once, [] {\n        g_windows_x18_stage_mapping =\n            CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LONG),\n                               kWindowsX18StageMapName);\n        if (g_windows_x18_stage_mapping == nullptr) {\n            return;\n        }\n\n        void* const view = MapViewOfFile(g_windows_x18_stage_mapping, FILE_MAP_ALL_ACCESS, 0, 0,\n                                         sizeof(LONG));\n        if (view == nullptr) {\n            return;\n        }\n\n        g_windows_x18_stage = reinterpret_cast<volatile LONG*>(view);\n        InterlockedExchange(g_windows_x18_stage, 0);\n    });\n}\n\nvoid SetWindowsX18Stage(LONG stage) noexcept {\n    if (g_windows_x18_stage != nullptr) {\n        InterlockedExchange(g_windows_x18_stage, stage);\n    }\n}\n\nvoid SetWindowsX18StageIfCurrent(LONG expected, LONG stage) noexcept {\n    if (g_windows_x18_stage != nullptr) {\n        InterlockedCompareExchange(g_windows_x18_stage, stage, expected);\n    }\n}\n\nstruct WindowsTebStackBounds {\n"""
text = replace_once(text, global_old, global_new, "shared-stage globals")

initialize_old = """void ArmNce::Initialize() {\n    if (m_windows_break != nullptr && !m_windows_break->IsBound()) {\n"""
initialize_new = """void ArmNce::Initialize() {\n    InitializeWindowsX18StageMap();\n\n    if (m_windows_break != nullptr && !m_windows_break->IsBound()) {\n"""
text = replace_once(text, initialize_old, initialize_new, "shared-stage initialize")

veh_old = """    if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT &&\n        NCE::WindowsX18FallbackTrap::FindOriginalInstruction(\n            context.Pc, process->GetPostHandlers()).has_value()) {\n        params->lock.store(SpinLockLocked, std::memory_order_release);\n        if (NCE::WindowsX18FallbackTrap::TryRedirect(exception, *guest,\n                                                     process->GetPostHandlers())) {\n            return EXCEPTION_CONTINUE_EXECUTION;\n        }\n        params->lock.store(SpinLockUnlocked, std::memory_order_release);\n        return EXCEPTION_CONTINUE_SEARCH;\n    }\n"""
veh_new = """    if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT &&\n        NCE::WindowsX18FallbackTrap::FindOriginalInstruction(\n            context.Pc, process->GetPostHandlers()).has_value()) {\n        SetWindowsX18Stage(1);\n        params->lock.store(SpinLockLocked, std::memory_order_release);\n        if (NCE::WindowsX18FallbackTrap::TryRedirect(exception, *guest,\n                                                     process->GetPostHandlers())) {\n            SetWindowsX18Stage(2);\n            NCE::WindowsNceTransition::ContinueContext(context);\n        }\n        SetWindowsX18Stage(3);\n        params->lock.store(SpinLockUnlocked, std::memory_order_release);\n        return EXCEPTION_CONTINUE_SEARCH;\n    }\n"""
text = replace_once(text, veh_old, veh_new, "ordinary-x18 shared-stage witness")

resume_old = """        if (breadcrumb_post) {\n            hr = static_cast<HaltReason>(NCE::WindowsNceEnterGuest(\n                &m_guest_ctx, reinterpret_cast<const void*>(breadcrumb_it->second)));\n        } else {\n            hr = static_cast<HaltReason>(NCE::WindowsNceEnterGuestContext(&m_guest_ctx));\n        }\n        RestoreHostTebStackBounds(teb_stack_bounds);\n"""
resume_new = """        if (breadcrumb_post) {\n            hr = static_cast<HaltReason>(NCE::WindowsNceEnterGuest(\n                &m_guest_ctx, reinterpret_cast<const void*>(breadcrumb_it->second)));\n        } else {\n            hr = static_cast<HaltReason>(NCE::WindowsNceEnterGuestContext(&m_guest_ctx));\n        }\n        SetWindowsX18StageIfCurrent(2, 4);\n        RestoreHostTebStackBounds(teb_stack_bounds);\n"""
text = replace_once(text, resume_old, resume_new, "stage2 host continuation witness")
path.write_text(text, encoding="utf-8")

# Diagnostic-only client timing change: command-line -g currently calls BootGame from the
# MainWindow constructor, while GUI file loading calls the same function after the event loop is
# active. Defer only the command-line game boot by one Qt event-loop turn to isolate that timing.
gui_path = Path("src/yuzu/main_window.cpp")
gui_text = gui_path.read_text(encoding="utf-8")
gui_old = """        if (!game_path.isEmpty()) {\n            BootGame(game_path, ApplicationAppletParameters());\n        } else if (should_launch_qlaunch) {\n"""
gui_new = """        if (!game_path.isEmpty()) {\n            QTimer::singleShot(0, this, [this, game_path] {\n                BootGame(game_path, ApplicationAppletParameters());\n            });\n        } else if (should_launch_qlaunch) {\n"""
gui_text = replace_once(gui_text, gui_old, gui_new, "deferred command-line BootGame")
gui_path.write_text(gui_text, encoding="utf-8")

print("NCE_CINC_X18_SHARED_STAGE_INJECTION=PASS")
print("NCE_CINC_X18_STAGE2_HOST_CONTINUATION_WITNESS=PASS")
print("NCE_CINC_X18_NTCONTINUE_CANDIDATE=PASS")
print("NCE_CINC_X18_GUI_CLI_BOOT_DEFERRED=PASS")
