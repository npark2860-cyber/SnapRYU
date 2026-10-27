cmake_minimum_required(VERSION 3.24)
project(SnapRyuNativeLsfg LANGUAGES C CXX)
set(LSFG_CEMU_BUILD_TESTS ON CACHE BOOL "" FORCE)
add_subdirectory(ported)
add_library(snapryu_lsfg_arm64 SHARED exports.def dll.cpp)
target_link_libraries(snapryu_lsfg_arm64 PRIVATE LSFGCemu::LSFGCemu)
set_target_properties(snapryu_lsfg_arm64 PROPERTIES OUTPUT_NAME "lsfg_arm64")

# Optional, reproducible self-contained build using the user's original shaders.
# Existing external-file builds remain unchanged when this path is omitted.
set(LSFG_SHADER_SOURCE "" CACHE FILEPATH "Hash-pinned original Lossless.dll for embedding")
if(LSFG_SHADER_SOURCE)
  enable_language(RC)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/original-shaders")
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/shader_resources.py"
    "${LSFG_SHADER_SOURCE}" --out "${_shader_dir}" COMMAND_ERROR_IS_FATAL ANY)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${LSFG_SHADER_SOURCE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/shader_resources.py")
  target_sources(snapryu_lsfg_arm64 PRIVATE "${_shader_dir}/shaders.rc")
  target_compile_definitions(lsfg_cemu_arm64 PRIVATE LSFG_EMBEDDED_SHADERS=1)
  target_include_directories(lsfg_cemu_arm64 PRIVATE "${_shader_dir}")
endif()

enable_testing()
# Exercise the exported C ABI across the DLL boundary as well as the original static tests.
add_executable(lsfg_dll_contracts ported/tests/failure_contracts.cpp)
target_compile_features(lsfg_dll_contracts PRIVATE cxx_std_20)
target_compile_options(lsfg_dll_contracts PRIVATE /utf-8 /EHsc)
target_include_directories(lsfg_dll_contracts PRIVATE ported/include ported/third_party/vulkan_headers/include)
target_link_libraries(lsfg_dll_contracts PRIVATE snapryu_lsfg_arm64)
add_test(NAME lsfg_dll_contracts COMMAND lsfg_dll_contracts)

# Explicitly invoked GPU validator. It does not run implicitly during CTest.
set(_common_root "${CMAKE_CURRENT_SOURCE_DIR}/ported/third_party/lsfg_legacy/lsfg-vk-common")
file(GLOB _validation_common CONFIGURE_DEPENDS "${_common_root}/src/vulkan/*.cpp")
add_executable(lsfg_gpu_validate tests/gpu_validate.cpp ${_validation_common}
  "${_common_root}/src/helpers/errors.cpp")
target_compile_features(lsfg_gpu_validate PRIVATE cxx_std_20)
target_compile_options(lsfg_gpu_validate PRIVATE /utf-8 /EHsc /W3 /wd5030)
target_compile_definitions(lsfg_gpu_validate PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN VK_NO_PROTOTYPES)
target_include_directories(lsfg_gpu_validate PRIVATE ported/include
  ported/third_party/vulkan_headers/include "${_common_root}/include")
