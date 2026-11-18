function(flitzfs_stage_msvc_asan_runtime)
  if(NOT WIN32 OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    return()
  endif()

  set(asan_flag "(^|[ \t\"])[/-]fsanitize=address([ \t\"]|$)")
  set(asan_configs)
  if("${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS}" MATCHES "${asan_flag}")
    set(stage_enabled 1)
  else()
    foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES CMAKE_BUILD_TYPE)
      string(TOUPPER "${config}" config_upper)
      if("${CMAKE_C_FLAGS_${config_upper}} ${CMAKE_CXX_FLAGS_${config_upper}}" MATCHES "${asan_flag}")
        list(APPEND asan_configs "${config}")
      endif()
    endforeach()
    if(NOT asan_configs)
      return()
    endif()
    string(JOIN "," asan_config_expression ${asan_configs})
    set(stage_enabled "$<CONFIG:${asan_config_expression}>")
  endif()

  if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    set(asan_arch x86_64)
  elseif(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "X86")
    set(asan_arch i386)
  else()
    message(FATAL_ERROR "Unsupported MSVC AddressSanitizer target: ${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
  endif()
  cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH compiler_bin)
  set(runtime "${compiler_bin}/clang_rt.asan_dynamic-${asan_arch}.dll")
  if(NOT EXISTS "${runtime}" OR IS_DIRECTORY "${runtime}")
    message(FATAL_ERROR
      "MSVC AddressSanitizer runtime missing: ${runtime}\n"
      "Install the AddressSanitizer component for the selected Visual C++ toolset.")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${runtime}")
  file(SHA256 "${runtime}" runtime_sha256)

  get_property(project_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  set(executables)
  set(output_directories)
  foreach(target IN LISTS project_targets)
    get_target_property(type "${target}" TYPE)
    if(type STREQUAL "EXECUTABLE")
      list(APPEND executables "${target}")
      list(APPEND output_directories "$<TARGET_FILE_DIR:${target}>")
    endif()
  endforeach()
  # One staging target owns all copies for a configuration, including parallel
  # single-target builds. App-local loading also works outside CTest.
  set(manifest "${CMAKE_CURRENT_BINARY_DIR}/flitzfs-asan-output-$<CONFIG>.txt")
  file(GENERATE OUTPUT "${manifest}"
    CONTENT "$<${stage_enabled}:$<JOIN:${output_directories},\n>>\n")
  set(runtime_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/StageWindowsAsanRuntime.cmake")
  set(runtime_command "${CMAKE_COMMAND}" "-Druntime=${runtime}"
    "-Dmanifest=${manifest}" "-Dexpected_sha256=${runtime_sha256}")
  add_custom_target(flitzfs_msvc_asan_runtime
    COMMAND ${runtime_command} -P "${runtime_script}"
    VERBATIM)
  foreach(target IN LISTS executables)
    add_dependencies("${target}" flitzfs_msvc_asan_runtime)
  endforeach()

  get_property(registered_tests DIRECTORY PROPERTY TESTS)
  if(registered_tests)
    set(fixture_configurations)
    if(asan_configs)
      set(fixture_configurations CONFIGURATIONS ${asan_configs})
    endif()
    # Plain CTest and filtered runs both check the app-local runtime before
    # invoking any test. A setup failure keeps test binaries out of the loader.
    add_test(NAME windows_asan_runtime_preflight ${fixture_configurations}
      COMMAND ${runtime_command} -Dverify_only=ON -P "${runtime_script}")
    set_tests_properties(windows_asan_runtime_preflight PROPERTIES
      FIXTURES_SETUP flitzfs_asan_runtime TIMEOUT 10)
    set_property(TEST ${registered_tests} APPEND PROPERTY FIXTURES_REQUIRED
      "$<${stage_enabled}:flitzfs_asan_runtime>")
  endif()
  message(STATUS "MSVC AddressSanitizer runtime for this build: ${runtime}")
endfunction()
