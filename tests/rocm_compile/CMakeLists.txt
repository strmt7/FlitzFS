cmake_minimum_required(VERSION 3.24)
project(FlitzFSCurrentRocmCompile LANGUAGES CXX)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Native Windows MSVC AMD64 qualification required")
endif()
set(FLITZFS_ROCM_SDK "" CACHE PATH "Explicit complete verified ROCm distribution")
set(FLITZFS_ROCM_KERNEL_OBJECT "" CACHE FILEPATH "Current SDK-compiled native kernel object")
foreach(input IN ITEMS
    "share/therock/therock_manifest.json" "include/hip/hip_version.h"
    "include/hip/hip_runtime_api.h" "lib/amdhip64.lib")
  if(NOT EXISTS "${FLITZFS_ROCM_SDK}/${input}")
    message(FATAL_ERROR "Complete verified SDK is required; no component fallback")
  endif()
endforeach()
if(NOT EXISTS "${FLITZFS_ROCM_KERNEL_OBJECT}")
  message(FATAL_ERROR "Source-bound SDK kernel compilation must complete first")
endif()
file(READ "${FLITZFS_ROCM_SDK}/share/therock/therock_manifest.json" metadata LIMIT 1048576)
string(JSON rocm_version GET "${metadata}" rocm_version)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/rocm-sdk-lock.json" lock LIMIT 16384)
string(JSON selected_version GET "${lock}" version)
if(NOT rocm_version STREQUAL selected_version)
  message(FATAL_ERROR "SDK distribution differs from the whole-package lock")
endif()
file(GLOB runtimes "${FLITZFS_ROCM_SDK}/bin/amdhip64*.dll")
list(LENGTH runtimes runtime_count)
if(NOT runtime_count EQUAL 1)
  message(FATAL_ERROR "Exactly one SDK-bound runtime basename is required")
endif()
list(GET runtimes 0 runtime_file)
get_filename_component(runtime_name "${runtime_file}" NAME)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_executable(flitzfs_rocm_header_probe header_probe.cpp)
target_compile_features(flitzfs_rocm_header_probe PRIVATE cxx_std_20)
target_include_directories(flitzfs_rocm_header_probe SYSTEM PRIVATE "${FLITZFS_ROCM_SDK}/include")
target_compile_definitions(flitzfs_rocm_header_probe PRIVATE __HIP_PLATFORM_AMD__=1
    __HIP_DISABLE_CPP_FUNCTIONS__=1 USE_PROF_API=0 NOMINMAX UNICODE _UNICODE
    FLITZFS_SELECTED_ROCM="${rocm_version}"
    FLITZFS_REQUIRED_HIP_RUNTIME_DLL_NAME="${runtime_name}")
target_compile_options(flitzfs_rocm_header_probe PRIVATE /W4 /permissive-)
target_include_directories(flitzfs_rocm_header_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/windows")
add_executable(flitzfs_rocm_api_probe api_probe.cpp)
target_compile_features(flitzfs_rocm_api_probe PRIVATE cxx_std_20)
target_include_directories(flitzfs_rocm_api_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/windows")
target_include_directories(flitzfs_rocm_api_probe SYSTEM PRIVATE "${FLITZFS_ROCM_SDK}/include")
target_compile_definitions(flitzfs_rocm_api_probe PRIVATE __HIP_PLATFORM_AMD__=1
    __HIP_DISABLE_CPP_FUNCTIONS__=1 USE_PROF_API=0 NOMINMAX UNICODE _UNICODE
    FLITZFS_REQUIRED_HIP_RUNTIME_DLL_NAME="${runtime_name}")
target_compile_options(flitzfs_rocm_api_probe PRIVATE /W4 /permissive-)
set_source_files_properties("${FLITZFS_ROCM_KERNEL_OBJECT}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(flitzfs_rocm_kernels SHARED "${FLITZFS_ROCM_KERNEL_OBJECT}" kernel_module.cpp)
target_compile_features(flitzfs_rocm_kernels PRIVATE cxx_std_20)
target_include_directories(flitzfs_rocm_kernels PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
target_link_libraries(flitzfs_rocm_kernels PRIVATE "${FLITZFS_ROCM_SDK}/lib/amdhip64.lib")
target_compile_options(flitzfs_rocm_kernels PRIVATE /W4 /permissive-)

# The SDK worker and its import closure stay in the disposable build fixture.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(flitzfs_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(FLITZFS_PORTABLE_CPU ON CACHE BOOL "" FORCE)
set(FLITZFS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FLITZFS_WITH_LZ4 OFF CACHE BOOL "" FORCE)
set(FLITZFS_WITH_ZSTD OFF CACHE BOOL "" FORCE)
add_subdirectory("${flitzfs_root}" "${CMAKE_CURRENT_BINARY_DIR}/core" EXCLUDE_FROM_ALL)

set(worker_output "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
set(bundle_header "${worker_output}/hip_worker_bundle.hpp")
set(bundle_receipt "${worker_output}/hip-bundle.json")
add_custom_command(OUTPUT "${bundle_header}" "${bundle_receipt}"
  COMMAND "${Python3_EXECUTABLE}" -B "${flitzfs_root}/tools/ci/stage_hip_worker_bundle.py"
    --runtime "${runtime_file}" --kernel "$<TARGET_FILE:flitzfs_rocm_kernels>"
    --output "${worker_output}/hip" --header "${bundle_header}" --receipt "${bundle_receipt}"
  DEPENDS flitzfs_rocm_kernels "${runtime_file}"
    "${flitzfs_root}/tools/ci/stage_hip_worker_bundle.py"
    "${flitzfs_root}/tools/ci/check_portable_cpu.py"
  VERBATIM)

add_executable(flitzfs_hip_worker
  "${flitzfs_root}/src/windows/hip_worker_main.cpp"
  "${flitzfs_root}/src/windows/hip_decode_worker.cpp"
  "${flitzfs_root}/src/windows/hip_module_verification.cpp"
  "${bundle_header}")
target_compile_features(flitzfs_hip_worker PRIVATE cxx_std_20)
target_include_directories(flitzfs_hip_worker PRIVATE "${worker_output}")
target_include_directories(flitzfs_hip_worker SYSTEM PRIVATE "${FLITZFS_ROCM_SDK}/include")
target_compile_definitions(flitzfs_hip_worker PRIVATE __HIP_PLATFORM_AMD__=1
  __HIP_DISABLE_CPP_FUNCTIONS__=1 USE_PROF_API=0 NOMINMAX UNICODE _UNICODE)
target_compile_options(flitzfs_hip_worker PRIVATE /W4 /permissive-)
target_link_libraries(flitzfs_hip_worker PRIVATE flitzfs_worker_exchange flitzfs_gpu_budget bcrypt wintrust)

add_executable(flitzfs_hip_worker_probe worker_probe.cpp)
target_compile_features(flitzfs_hip_worker_probe PRIVATE cxx_std_20)
target_link_libraries(flitzfs_hip_worker_probe PRIVATE flitzfs_worker_exchange)
target_compile_options(flitzfs_hip_worker_probe PRIVATE /W4 /permissive-)

add_executable(flitzfs_hip_hardware_probe hardware_probe.cpp)
target_compile_features(flitzfs_hip_hardware_probe PRIVATE cxx_std_20)
target_link_libraries(flitzfs_hip_hardware_probe PRIVATE flitzfs_worker_exchange dxgi)
target_compile_options(flitzfs_hip_hardware_probe PRIVATE /W4 /permissive-)

# CPU metadata qualification uses OS-owned kernel32; it never loads AMD code.
add_executable(flitzfs_system_module_probe system_module_probe.cpp
  "${flitzfs_root}/src/windows/hip_module_verification.cpp")
target_compile_features(flitzfs_system_module_probe PRIVATE cxx_std_20)
target_include_directories(flitzfs_system_module_probe PRIVATE "${flitzfs_root}/src/windows")
target_compile_definitions(flitzfs_system_module_probe PRIVATE NOMINMAX UNICODE _UNICODE
  FLITZFS_REQUIRED_HIP_RUNTIME_DLL_NAME="${runtime_name}")
target_compile_options(flitzfs_system_module_probe PRIVATE /W4 /permissive-)
target_link_libraries(flitzfs_system_module_probe PRIVATE flitzfs_core bcrypt wintrust)
