# project-wide options, output layout and the `prism_options` compile interface every prism target links PRIVATE.
# mirrors strata_options (language level, warnings, optimisation, link flags) so the two libraries agree on runtime and ABI.

include_guard(GLOBAL)

option(PRISM_STATIC_CRT "link the static msvc runtime (/MT): one exe, no redistributable" ON)
option(PRISM_LTO        "whole-program optimisation outside debug builds (ThinLTO with clang-cl, /GL otherwise)" ON)
option(PRISM_WERROR     "treat warnings as errors" OFF)
option(PRISM_PCH        "precompile the standard headers" ON)

if(PROJECT_IS_TOP_LEVEL)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "build type" FORCE)
    endif()

    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    # cl.exe: /GL + /LTCG. clang-cl gets ThinLTO by hand below (cmake's IPO switch emits full -flto there)
    if(PRISM_LTO AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE        ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    endif()

    # cmake's default /EHsc and /Ob1|/Ob2 would trigger D9025 next to our overrides
    string(REGEX REPLACE "/EH[a-z-]+" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    foreach(_cfg RELEASE RELWITHDEBINFO)
        string(REGEX REPLACE "/Ob[0-3]" "/Ob3" CMAKE_CXX_FLAGS_${_cfg} "${CMAKE_CXX_FLAGS_${_cfg}}")
    endforeach()

    if(PRISM_STATIC_CRT)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

add_library(prism_options INTERFACE)

# cmake 4.3 maps cxx_std_23 to /std:c++latest for msvc; clang-cl's feature detection would pin c++23 instead, so there
# the explicit /std:c++latest below does the work
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_features(prism_options INTERFACE cxx_std_23)
endif()
set_target_properties(prism_options PROPERTIES CXX_EXTENSIONS OFF)

# exception free like strata: errors are std::expected / bool, the stl does not pull in unwind tables
target_compile_definitions(prism_options INTERFACE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS _HAS_EXCEPTIONS=0
    $<$<CONFIG:Debug>:PRISM_DEBUG=1>
)

if(PRISM_PCH)
    target_precompile_headers(prism_options INTERFACE
        <algorithm> <array> <atomic> <chrono> <cmath> <cstdint> <cstring> <expected> <format> <functional> <memory>
        <mutex> <optional> <span> <string> <string_view> <thread> <unordered_map> <utility> <vector>
    )
endif()

if(MSVC)
    target_compile_options(prism_options INTERFACE
        /std:c++latest /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:inline
        /GR- /EHs-c-
        $<$<NOT:$<CONFIG:Debug>>:/Oi /Gy /Gw>
    )
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(prism_options INTERFACE /Zc:preprocessor /Zc:throwingNew)
    endif()
    target_link_options(prism_options INTERFACE $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>)

    if(PRISM_LTO AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(prism_options INTERFACE $<$<NOT:$<CONFIG:Debug>>:-flto=thin>)
        target_link_options(prism_options INTERFACE $<$<NOT:$<CONFIG:Debug>>:-flto=thin>)
    endif()
    if(PRISM_WERROR)
        target_compile_options(prism_options INTERFACE /WX)
    endif()
endif()
