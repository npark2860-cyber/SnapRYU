# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(tests
    common/bit_field.cpp
    common/cityhash.cpp
    common/container_hash.cpp
    common/fibers.cpp
    common/host_memory.cpp
    common/param_package.cpp
    common/npu_resolution.cpp
    common/range_map.cpp
    common/ring_buffer.cpp
    common/scratch_buffer.cpp
    common/undefined_fix.cpp
    common/unique_function.cpp
    core/core_timing.cpp
    core/snapgo_compact_profile.cpp
    core/internal_network/network.cpp
    video_core/memory_tracker.cpp
    video_core/pending_downloads.cpp
    video_core/vulkan_output_policy.cpp
    input_common/calibration_configuration_job.cpp
)

create_target_directory_groups(tests)

target_link_libraries(tests PRIVATE common core input_common video_core)
target_link_libraries(tests PRIVATE ${PLATFORM_LIBRARIES} Catch2::Catch2WithMain Threads::Threads)

add_test(NAME tests COMMAND tests)

if(WIN32)
    # A status-only ABI fake validates the real boot snapshot without loading the
    # product host or touching the network/profile. Never place it in product bin.
    add_library(snapgo_compact_test_host SHARED common/snapgo_compact_test_host.cpp)
    target_include_directories(snapgo_compact_test_host PRIVATE ${PROJECT_SOURCE_DIR}/src)
    set_target_properties(snapgo_compact_test_host PROPERTIES PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/../evidence/compact-totk")
    target_compile_definitions(tests PRIVATE
        SNAPGO_COMPACT_TEST_HOST_PATH="$<TARGET_FILE:snapgo_compact_test_host>")
    add_dependencies(tests snapgo_compact_test_host)
endif()

# needed for vma
if (NOT MSVC)
    target_compile_options(tests PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-conversion>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-variable>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-parameter>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-field-initializers>)
endif()
