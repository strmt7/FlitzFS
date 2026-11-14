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
add_executable(flitzfs_rocm_previous_priority_probe api_probe.cpp)
target_compile_features(flitzfs_rocm_previous_priority_probe PRIVATE cxx_std_20)
target_include_directories(flitzfs_rocm_previous_priority_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/windows")
target_include_directories(flitzfs_rocm_previous_priority_probe SYSTEM PRIVATE "${FLITZFS_ROCM_SDK}/include")
target_compile_definitions(flitzfs_rocm_previous_priority_probe PRIVATE __HIP_PLATFORM_AMD__=1
    __HIP_DISABLE_CPP_FUNCTIONS__=1 USE_PROF_API=0 NOMINMAX UNICODE _UNICODE
    FLITZFS_REQUIRED_HIP_RUNTIME_DLL_NAME="${runtime_name}")
target_compile_options(flitzfs_rocm_previous_priority_probe PRIVATE /W4 /permissive-)
target_compile_definitions(flitzfs_rocm_previous_priority_probe PRIVATE FLITZFS_PREVIOUS_HIP_API=1)
set_source_files_properties("${FLITZFS_ROCM_KERNEL_OBJECT}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(flitzfs_rocm_kernels SHARED "${FLITZFS_ROCM_KERNEL_OBJECT}" kernel_module.cpp)
target_compile_features(flitzfs_rocm_kernels PRIVATE cxx_std_20)
target_include_directories(flitzfs_rocm_kernels PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
target_link_libraries(flitzfs_rocm_kernels PRIVATE "${FLITZFS_ROCM_SDK}/lib/amdhip64.lib")
target_compile_options(flitzfs_rocm_kernels PRIVATE /W4 /permissive-)

# Separate manual link-only fixture. It is never loaded or added to the worker bundle.
set(FLITZFS_ROCM_LZ4_OBJECT "" CACHE FILEPATH "Qualified private byte-entry object")
set(FLITZFS_ROCM_LZ4_SOURCE "" CACHE FILEPATH "Qualified staged private module source")
if(FLITZFS_ROCM_LZ4_OBJECT OR FLITZFS_ROCM_LZ4_SOURCE)
  if(NOT EXISTS "${FLITZFS_ROCM_LZ4_OBJECT}" OR NOT EXISTS "${FLITZFS_ROCM_LZ4_SOURCE}")
    message(FATAL_ERROR "Both qualified byte-entry object and private module source are required")
  endif()
  get_filename_component(lz4_source_dir "${FLITZFS_ROCM_LZ4_SOURCE}" DIRECTORY)
  set_source_files_properties("${FLITZFS_ROCM_LZ4_OBJECT}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  add_library(flitzfs_rocm_lz4_candidate SHARED "${FLITZFS_ROCM_LZ4_OBJECT}" "${FLITZFS_ROCM_LZ4_SOURCE}")
  target_compile_features(flitzfs_rocm_lz4_candidate PRIVATE cxx_std_20)
  target_include_directories(flitzfs_rocm_lz4_candidate PRIVATE "${lz4_source_dir}")
  target_link_libraries(flitzfs_rocm_lz4_candidate PRIVATE "${FLITZFS_ROCM_SDK}/lib/amdhip64.lib")
  target_compile_options(flitzfs_rocm_lz4_candidate PRIVATE /W4 /permissive-)
endif()

# The SDK worker and its import closure stay in the disposable build fixture.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(flitzfs_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(FLITZFS_PORTABLE_CPU ON CACHE BOOL "" FORCE)
set(FLITZFS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# Optional decoder is source-pinned and static; no system package discovery.
set(FLITZFS_ROCM_LZ4_CPU_SOURCE "" CACHE PATH "Qualified staged CPU LZ4 source")
if(FLITZFS_ROCM_LZ4_CPU_SOURCE)
  if(NOT FLITZFS_ROCM_LZ4_OBJECT OR NOT FLITZFS_ROCM_LZ4_SOURCE)
    message(FATAL_ERROR "CPU LZ4 qualification requires the separate typed-module link fixture")
  endif()
  set(cpu_lz4_names lz4.c lz4.h LICENSE LICENSE-repository)
  set(cpu_lz4_sizes 126490 44407 1311 646)
  set(cpu_lz4_hashes
    2e58f124f1032f15aecc99772b7de624c4289245729fa5403506f00b06a54937
    c204caad0638a12c9ce80831094ec8fffb34aa57e74307067b08fc9452fc2611
    8b58c446121a109ccf32edc094bba3010a3d85e4ee3702950db55e4d3e87736c
    4bc9c403f6b679cc076dee210e0e21c27b4e8f6d3fb43d85364c76657bfaaac5)
  foreach(i RANGE 0 3)
    list(GET cpu_lz4_names ${i} name)
    list(GET cpu_lz4_sizes ${i} expected_size)
    list(GET cpu_lz4_hashes ${i} expected_hash)
    set(path "${FLITZFS_ROCM_LZ4_CPU_SOURCE}/${name}")
    if(NOT EXISTS "${path}" OR IS_SYMLINK "${path}" OR IS_DIRECTORY "${path}")
      message(FATAL_ERROR "Qualified CPU LZ4 source required")
    endif()
    file(SIZE "${path}" actual_size)
    if(NOT actual_size EQUAL expected_size)
      message(FATAL_ERROR "CPU LZ4 canonical source size differs")
    endif()
    file(SHA256 "${path}" actual_hash)
    if(NOT actual_hash STREQUAL expected_hash)
      message(FATAL_ERROR "CPU LZ4 canonical source digest differs")
    endif()
  endforeach()
  enable_language(C)
  add_library(flitzfs_sdk_lz4 STATIC "${FLITZFS_ROCM_LZ4_CPU_SOURCE}/lz4.c")
  target_include_directories(flitzfs_sdk_lz4 PUBLIC "${FLITZFS_ROCM_LZ4_CPU_SOURCE}")
  set(FLITZFS_WITH_LZ4 ON CACHE BOOL "" FORCE)
  set(FLITZFS_REQUIRE_LZ4 ON CACHE BOOL "" FORCE)
  set(FLITZFS_LZ4_INCLUDE "${FLITZFS_ROCM_LZ4_CPU_SOURCE}" CACHE PATH "" FORCE)
  set(FLITZFS_LZ4_LIBRARY flitzfs_sdk_lz4 CACHE STRING "" FORCE)
else()
  set(FLITZFS_WITH_LZ4 OFF CACHE BOOL "" FORCE)
  set(FLITZFS_REQUIRE_LZ4 OFF CACHE BOOL "" FORCE)
endif()
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

add_custom_target(flitzfs_hip_worker_bundle DEPENDS "${bundle_header}" "${bundle_receipt}")
add_library(flitzfs_hip_compute_common STATIC "${flitzfs_root}/src/windows/hip_decode_worker.cpp")
target_compile_features(flitzfs_hip_compute_common PUBLIC cxx_std_20)
target_include_directories(flitzfs_hip_compute_common PUBLIC "${flitzfs_root}/src/windows")
target_include_directories(flitzfs_hip_compute_common SYSTEM PUBLIC "${FLITZFS_ROCM_SDK}/include")
target_compile_definitions(flitzfs_hip_compute_common PUBLIC __HIP_PLATFORM_AMD__=1
  __HIP_DISABLE_CPP_FUNCTIONS__=1 USE_PROF_API=0 NOMINMAX UNICODE _UNICODE
  FLITZFS_REQUIRED_HIP_RUNTIME_DLL_NAME="${runtime_name}")
target_compile_options(flitzfs_hip_compute_common PRIVATE /W4 /permissive-)
target_link_libraries(flitzfs_hip_compute_common PUBLIC flitzfs_worker_exchange flitzfs_gpu_budget flitzfs_gpu_topology)

add_executable(flitzfs_hip_worker
  "${flitzfs_root}/src/windows/hip_worker_main.cpp"
  "${flitzfs_root}/src/windows/hip_module_verification.cpp")
add_dependencies(flitzfs_hip_worker flitzfs_hip_worker_bundle)
target_compile_features(flitzfs_hip_worker PRIVATE cxx_std_20)
target_include_directories(flitzfs_hip_worker PRIVATE "${worker_output}")
target_include_directories(flitzfs_hip_worker SYSTEM PRIVATE "${FLITZFS_ROCM_SDK}/include")
target_compile_definitions(flitzfs_hip_worker PRIVATE __HIP_PLATFORM_AMD__=1
  __HIP_DISABLE_CPP_FUNCTIONS__=1 USE_PROF_API=0 NOMINMAX UNICODE _UNICODE)
target_compile_options(flitzfs_hip_worker PRIVATE /W4 /permissive-)
target_link_libraries(flitzfs_hip_worker PRIVATE flitzfs_hip_compute_common bcrypt wintrust)

add_executable(flitzfs_hip_worker_probe worker_probe.cpp)
target_compile_features(flitzfs_hip_worker_probe PRIVATE cxx_std_20)
target_link_libraries(flitzfs_hip_worker_probe PRIVATE flitzfs_worker_exchange)
target_compile_options(flitzfs_hip_worker_probe PRIVATE /W4 /permissive-)

add_executable(flitzfs_hip_hardware_probe hardware_probe.cpp)
target_compile_features(flitzfs_hip_hardware_probe PRIVATE cxx_std_20)
target_link_libraries(flitzfs_hip_hardware_probe PRIVATE flitzfs_worker_exchange flitzfs_gpu_budget flitzfs_gpu_topology dxgi)
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

# CPU-only real decoder/framing/slab tests share the worker's exact core and CRT.
if(FLITZFS_ROCM_LZ4_CPU_SOURCE)
  foreach(kind IN ITEMS gpu_lz4_verify lz4_worker_wire gpu_lz4_slab)
    add_executable(flitzfs_sdk_${kind}_tests "${flitzfs_root}/tests/test_${kind}.cpp")
    target_link_libraries(flitzfs_sdk_${kind}_tests PRIVATE flitzfs_core flitzfs_sdk_lz4)
    target_compile_definitions(flitzfs_sdk_${kind}_tests PRIVATE FLITZFS_TEST_HAS_LZ4=1)
    target_compile_features(flitzfs_sdk_${kind}_tests PRIVATE cxx_std_20)
    target_compile_options(flitzfs_sdk_${kind}_tests PRIVATE /W4 /permissive-)
  endforeach()
endif()
add_executable(flitzfs_rocm_cpu_lz4_probe cpu_lz4_probe.cpp)
target_compile_features(flitzfs_rocm_cpu_lz4_probe PRIVATE cxx_std_20)
target_link_libraries(flitzfs_rocm_cpu_lz4_probe PRIVATE flitzfs_worker_exchange)
target_compile_options(flitzfs_rocm_cpu_lz4_probe PRIVATE /W4 /permissive-)

# SDK-only CPU qualification; neither implementation nor candidate module is
# linked into the normal worker, helper, filesystem, or distributable bundle.
if(FLITZFS_ROCM_LZ4_OBJECT AND FLITZFS_ROCM_LZ4_SOURCE)
  add_library(flitzfs_rocm_lz4_compute STATIC "${flitzfs_root}/src/windows/hip_lz4_compute.cpp")
  target_include_directories(flitzfs_rocm_lz4_compute PUBLIC "${lz4_source_dir}")
  target_link_libraries(flitzfs_rocm_lz4_compute PUBLIC flitzfs_hip_compute_common)
  target_compile_options(flitzfs_rocm_lz4_compute PRIVATE /W4 /permissive-)
  add_executable(flitzfs_rocm_lz4_compute_probe lz4_compute_probe.cpp)
  target_link_libraries(flitzfs_rocm_lz4_compute_probe PRIVATE flitzfs_rocm_lz4_compute)
  target_compile_options(flitzfs_rocm_lz4_compute_probe PRIVATE /W4 /permissive-)
endif()

# Explicit disposable-VM runtime fixture. This separate executable leaves the
# original/default worker disconnected from compression compute.
option(FLITZFS_ROCM_LZ4_RUNTIME_FIXTURE "Build isolated compression runtime fixture" OFF)
set(FLITZFS_ROCM_LZ4_COVERAGE "" CACHE FILEPATH "Qualified offline object receipt")
if(FLITZFS_ROCM_LZ4_RUNTIME_FIXTURE)
  if(NOT FLITZFS_ROCM_LZ4_CPU_SOURCE OR NOT TARGET flitzfs_rocm_lz4_compute OR
      NOT EXISTS "${FLITZFS_ROCM_LZ4_COVERAGE}")
    message(FATAL_ERROR "Runtime fixture requires qualified typed object and static CPU recovery")
  endif()
  set(lz4_bundle_header "${worker_output}/hip_lz4_bundle.hpp")
  set(lz4_bundle_receipt "${worker_output}/hip-lz4-bundle.json")
  add_custom_command(OUTPUT "${lz4_bundle_header}" "${lz4_bundle_receipt}"
    COMMAND "${Python3_EXECUTABLE}" -B "${flitzfs_root}/tools/ci/stage_lz4_runtime_bundle.py"
      --module "$<TARGET_FILE:flitzfs_rocm_lz4_candidate>"
      --object "${FLITZFS_ROCM_LZ4_OBJECT}" --coverage "${FLITZFS_ROCM_LZ4_COVERAGE}"
      --runtime "${runtime_name}" --output "${worker_output}/hip-lz4"
      --header "${lz4_bundle_header}" --receipt "${lz4_bundle_receipt}"
    DEPENDS flitzfs_rocm_lz4_candidate "${FLITZFS_ROCM_LZ4_OBJECT}"
      "${FLITZFS_ROCM_LZ4_COVERAGE}" "${flitzfs_root}/tools/ci/stage_lz4_runtime_bundle.py"
    VERBATIM)
  add_custom_target(flitzfs_hip_lz4_bundle DEPENDS "${lz4_bundle_header}" "${lz4_bundle_receipt}")
  add_executable(flitzfs_hip_lz4_worker
    "${flitzfs_root}/src/windows/hip_worker_main.cpp"
    "${flitzfs_root}/src/windows/hip_module_verification.cpp")
  add_dependencies(flitzfs_hip_lz4_worker flitzfs_hip_worker_bundle flitzfs_hip_lz4_bundle)
  target_include_directories(flitzfs_hip_lz4_worker PRIVATE "${worker_output}")
  target_compile_definitions(flitzfs_hip_lz4_worker PRIVATE FLITZFS_HIP_LZ4_RUNTIME_FIXTURE=1)
  target_compile_options(flitzfs_hip_lz4_worker PRIVATE /W4 /permissive-)
  target_link_libraries(flitzfs_hip_lz4_worker PRIVATE flitzfs_rocm_lz4_compute bcrypt wintrust)
  add_executable(flitzfs_lz4_runtime_probe lz4_runtime_probe.cpp)
  target_compile_features(flitzfs_lz4_runtime_probe PRIVATE cxx_std_20)
  target_compile_definitions(flitzfs_lz4_runtime_probe PRIVATE PSAPI_VERSION=2)
  target_link_libraries(flitzfs_lz4_runtime_probe PRIVATE psapi)
  target_link_libraries(flitzfs_lz4_runtime_probe PRIVATE flitzfs_worker_exchange flitzfs_gpu_budget flitzfs_core)
  target_compile_options(flitzfs_lz4_runtime_probe PRIVATE /W4 /permissive-)
endif()

