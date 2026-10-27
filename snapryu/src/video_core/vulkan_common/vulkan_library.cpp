// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>

#include "common/dynamic_library.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/vulkan_common/vulkan_library.h"

#ifdef _WIN32
#include <filesystem>
#include <optional>
#include "common/settings.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Vulkan {

#ifdef _WIN32
namespace {

std::optional<std::wstring> ReadEnvironment(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), size));
    return value;
}

void WriteEnvironment(const wchar_t* name, const std::optional<std::wstring>& value) {
    SetEnvironmentVariableW(name, value ? value->c_str() : nullptr);
}

/// Points the Vulkan loader at the bundled Turnip driver, or back at what the process started with.
/// The loader reads these variables whenever it scans for drivers (each instance creation), so the value
/// of the game being started is used.
void SelectDriver() {
    static const std::optional<std::wstring> start_driver_files = ReadEnvironment(L"VK_DRIVER_FILES");
    static const std::optional<std::wstring> start_icd_filenames = ReadEnvironment(L"VK_ICD_FILENAMES");
    std::optional<std::wstring> driver_files = start_driver_files;
    std::optional<std::wstring> icd_filenames = start_icd_filenames;
    if (Settings::values.vulkan_driver.GetValue() == Settings::VulkanDriver::Turnip) {
        const auto manifest = Common::FS::GetExeDirectory() / "turnip" / "turnip_icd.json";
        std::error_code error;
        if (std::filesystem::is_regular_file(manifest, error)) {
            driver_files = icd_filenames = manifest.wstring();
            LOG_INFO(Render_Vulkan, "Vulkan driver: Turnip ({})", Common::FS::PathToUTF8String(manifest));
        } else {
            LOG_ERROR(Render_Vulkan, "Vulkan driver: Turnip selected but {} is missing; using the system driver",
                      Common::FS::PathToUTF8String(manifest));
        }
    }
    if (driver_files == start_driver_files) {
        LOG_INFO(Render_Vulkan, "Vulkan driver: System (VK_DRIVER_FILES={})",
                 driver_files ? Common::FS::PathToUTF8String(std::filesystem::path(*driver_files))
                              : std::string("unset, loader default"));
    }
    WriteEnvironment(L"VK_DRIVER_FILES", driver_files);
    WriteEnvironment(L"VK_ICD_FILENAMES", icd_filenames);
}

} // Anonymous namespace
#endif

std::shared_ptr<Common::DynamicLibrary> OpenLibrary(
    [[maybe_unused]] Core::Frontend::GraphicsContext* context) {
    LOG_DEBUG(Render_Vulkan, "Looking for a Vulkan library");
#if defined(__ANDROID__) && defined(ARCHITECTURE_arm64)
    // Android manages its Vulkan driver from the frontend.
    return context->GetDriverLibrary();
#else
    auto library = std::make_shared<Common::DynamicLibrary>();
#ifdef __APPLE__
    const auto libvulkan_filename =
        Common::FS::GetBundleDirectory() / "Contents/Frameworks/libvulkan.1.dylib";
    const auto libmoltenvk_filename =
        Common::FS::GetBundleDirectory() / "Contents/Frameworks/libMoltenVK.dylib";
    const char* library_paths[] = {std::getenv("LIBVULKAN_PATH"), libvulkan_filename.c_str(),
                                   libmoltenvk_filename.c_str()};
    // Check if a path to a specific Vulkan library has been specified.
    for (const auto& library_path : library_paths) {
        if (library_path && library->Open(library_path)) {
            break;
        }
    }
#else
#ifdef _WIN32
    SelectDriver();
#endif
    std::string filename = Common::DynamicLibrary::GetVersionedFilename("vulkan", 1);
    LOG_DEBUG(Render_Vulkan, "Trying Vulkan library: {}", filename);
    if (!library->Open(filename.c_str())) {
        // Android devices may not have libvulkan.so.1, only libvulkan.so.
        filename = Common::DynamicLibrary::GetVersionedFilename("vulkan");
        LOG_DEBUG(Render_Vulkan, "Trying Vulkan library (second attempt): {}", filename);
        void(library->Open(filename.c_str()));
    }
#endif
    return library;
#endif
}

} // namespace Vulkan
