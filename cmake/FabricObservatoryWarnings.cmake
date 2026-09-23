include_guard(GLOBAL)

# Applies the Summon Software Labs first-party warning contract to a target.
# The first-party warning count must be zero; /WX makes that enforceable.
function(fabric_observatory_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4
      /permissive-
      /utf-8
      /Zc:__cplusplus
      /Zc:preprocessor
      /Zc:inline
      /EHsc
      /external:anglebrackets
      /external:W0)
    if(FABRIC_OBSERVATORY_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
      -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference)
    if(FABRIC_OBSERVATORY_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# Probes whether the active compiler can build (not merely link) an
# AddressSanitizer binary. The result is reported to the caller; it is never
# silently assumed.
function(fabric_observatory_asan_supported out_var)
  if(NOT MSVC)
    set(${out_var} FALSE PARENT_SCOPE)
    return()
  endif()
  set(probe_dir "${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/asan-probe")
  file(MAKE_DIRECTORY "${probe_dir}")
  file(WRITE "${probe_dir}/probe.cpp" "int main() { int* p = new int(1); int v = *p; delete p; return v - 1; }\n")
  try_compile(compiled
    "${probe_dir}/build"
    SOURCES "${probe_dir}/probe.cpp"
    CMAKE_FLAGS "-DCMAKE_CXX_STANDARD=20"
    COMPILE_DEFINITIONS "/fsanitize=address"
    LINK_OPTIONS "/fsanitize=address"
    OUTPUT_VARIABLE probe_log)
  if(NOT compiled)
    set(${out_var} FALSE PARENT_SCOPE)
    return()
  endif()
  set(${out_var} TRUE PARENT_SCOPE)
endfunction()

# Copies the AddressSanitizer runtime next to the produced binary so that the
# sanitizer is actually active when the binary runs (a missing runtime DLL
# silently prevents the process from starting at all).
function(fabric_observatory_enable_asan target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /fsanitize=address)
    target_link_options(${target} PRIVATE /fsanitize=address)
    get_filename_component(_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_runtime_names clang_rt.asan_dynamic-x86_64.dll clang_rt.asan_dbg_dynamic-x86_64.dll)
    foreach(_name IN LISTS _runtime_names)
      if(EXISTS "${_compiler_dir}/${_name}")
        add_custom_command(TARGET ${target} POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
                  "${_compiler_dir}/${_name}" "$<TARGET_FILE_DIR:${target}>/${_name}"
          VERBATIM)
      endif()
    endforeach()
  else()
    target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endif()
endfunction()
