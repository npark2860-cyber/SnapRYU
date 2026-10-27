# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2022 Andrea Pappacoda <andrea@pappacoda.it>
#
# SPDX-License-Identifier: GPL-2.0-or-later

include(FindPackageHandleStandardArgs)

# The native Windows CMake/libarchive path on the Korean self-hosted ARM64 runner
# cannot extract cpp-httplib's archive because it tries to convert archive pathnames
# from CP949. The E5 harness already exposes MSYS2_LOCATION only for that configure
# step, so use a single-file header fetch there and keep every normal build on the
# existing package discovery/CPM path.
if (WIN32 AND DEFINED ENV{MSYS2_LOCATION})
    set(_httplib_selfhost_dir "${CMAKE_BINARY_DIR}/_selfhost_httplib")
    set(_httplib_selfhost_header "${_httplib_selfhost_dir}/httplib.h")
    set(_httplib_selfhost_patch_stamp "${_httplib_selfhost_dir}/.mingw-patched")

    file(MAKE_DIRECTORY "${_httplib_selfhost_dir}")
    if (NOT EXISTS "${_httplib_selfhost_header}")
        file(DOWNLOAD
            "https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.46.0/httplib.h"
            "${_httplib_selfhost_header}"
            TLS_VERIFY ON
            STATUS _httplib_download_status)
        list(GET _httplib_download_status 0 _httplib_download_code)
        if (NOT _httplib_download_code EQUAL 0)
            list(GET _httplib_download_status 1 _httplib_download_message)
            message(FATAL_ERROR
                "self-hosted httplib header download failed: ${_httplib_download_message}")
        endif()
    endif()

    if (NOT EXISTS "${_httplib_selfhost_patch_stamp}")
        find_program(_httplib_patch_exe patch REQUIRED)
        execute_process(
            COMMAND "${_httplib_patch_exe}" -p1
            INPUT_FILE "${CMAKE_SOURCE_DIR}/.patch/httplib/0001-mingw.patch"
            WORKING_DIRECTORY "${_httplib_selfhost_dir}"
            RESULT_VARIABLE _httplib_patch_rc)
        if (NOT _httplib_patch_rc EQUAL 0)
            message(FATAL_ERROR "self-hosted httplib MinGW patch failed: ${_httplib_patch_rc}")
        endif()
        file(WRITE "${_httplib_selfhost_patch_stamp}" "v0.46.0\n")
    endif()

    if (NOT TARGET httplib::httplib)
        add_library(httplib_selfhost INTERFACE)
        add_library(httplib::httplib ALIAS httplib_selfhost)
        target_include_directories(httplib_selfhost INTERFACE "${_httplib_selfhost_dir}")
        target_compile_definitions(httplib_selfhost INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
        if (TARGET OpenSSL::SSL)
            target_link_libraries(httplib_selfhost INTERFACE OpenSSL::SSL OpenSSL::Crypto)
        endif()
    endif()

    set(httplib_FOUND TRUE)
    set(httplib_VERSION 0.46.0)
    set(httplib_OpenSSL_FOUND TRUE)
    message(STATUS "Using self-hosted ARM64 single-header httplib@0.46.0")
else()
    find_package(httplib QUIET CONFIG)
    if (httplib_CONSIDERED_CONFIGS)
        find_package_handle_standard_args(httplib HANDLE_COMPONENTS CONFIG_MODE)
    else()
        find_package(PkgConfig QUIET)
        pkg_search_module(HTTPLIB QUIET IMPORTED_TARGET cpp-httplib)
        if ("-DCPPHTTPLIB_OPENSSL_SUPPORT" IN_LIST HTTPLIB_CFLAGS_OTHER)
            set(httplib_OpenSSL_FOUND TRUE)
        endif()
        if ("-DCPPHTTPLIB_ZLIB_SUPPORT" IN_LIST HTTPLIB_CFLAGS_OTHER)
            set(httplib_ZLIB_FOUND TRUE)
        endif()
        if ("-DCPPHTTPLIB_BROTLI_SUPPORT" IN_LIST HTTPLIB_CFLAGS_OTHER)
            set(httplib_Brotli_FOUND TRUE)
        endif()
        find_package_handle_standard_args(httplib
            REQUIRED_VARS HTTPLIB_INCLUDEDIR
            VERSION_VAR HTTPLIB_VERSION
            HANDLE_COMPONENTS
        )
    endif()

    if (httplib_FOUND AND NOT TARGET httplib::httplib)
        add_library(httplib::httplib ALIAS PkgConfig::HTTPLIB)
    endif()
endif()
