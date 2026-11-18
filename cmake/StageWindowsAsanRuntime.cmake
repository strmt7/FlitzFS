cmake_minimum_required(VERSION 3.22)

function(require_asan_runtime path)
  if(NOT EXISTS "${path}" OR IS_DIRECTORY "${path}")
    message(FATAL_ERROR
      "MSVC AddressSanitizer runtime is missing: ${path}\n"
      "Restore the selected toolset's AddressSanitizer component if necessary, then rebuild.")
  endif()
  file(SHA256 "${path}" actual_sha256)
  if(NOT actual_sha256 STREQUAL expected_sha256)
    message(FATAL_ERROR
      "MSVC AddressSanitizer runtime differs from the configured toolset: ${path}\n"
      "Reconfigure and rebuild the selected FlitzFS configuration.")
  endif()
endfunction()

file(STRINGS "${manifest}" output_directories ENCODING UTF-8)
list(REMOVE_ITEM output_directories "")
list(REMOVE_DUPLICATES output_directories)
if(NOT output_directories)
  if(verify_only)
    message(FATAL_ERROR "No runtime output directories recorded in ${manifest}. Reconfigure and rebuild.")
  endif()
  return()
endif()

cmake_path(GET runtime FILENAME runtime_name)
if(NOT verify_only)
  require_asan_runtime("${runtime}")
endif()
foreach(directory IN LISTS output_directories)
  set(staged "${directory}/${runtime_name}")
  if(NOT verify_only)
    file(MAKE_DIRECTORY "${directory}")
    file(COPY_FILE "${runtime}" "${staged}" ONLY_IF_DIFFERENT)
  endif()
  require_asan_runtime("${staged}")
endforeach()
