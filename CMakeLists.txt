cmake_minimum_required(VERSION 3.28)

project(prism
    VERSION 0.1.0
    DESCRIPTION "fast, modern file explorer on the strata ui framework"
    LANGUAGES CXX
)

if(WIN32)
    enable_language(RC)
elseif(NOT UNIX OR APPLE)
    message(FATAL_ERROR "prism targets windows and linux")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "prism only supports x64 builds")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(PRISM_STRATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../DxStrataFramework" CACHE PATH "Local DxStrataFramework checkout (used when present; falls back to FetchContent)")
set(PRISM_STRATA_GIT_TAG "v0.1.0" CACHE STRING "strata git tag or branch used when fetching via FetchContent")

include(prism_options)

# strata is consumed as a library only: no sandbox, tests, overlay or unused GPU backends. its runtime / LTO settings
# come from prism_options (set before this point, so both agree)
if(NOT TARGET strata::strata)
    set(STRATA_BUILD_SANDBOX  OFF CACHE BOOL "" FORCE)
    set(STRATA_BUILD_WEB_DEMO OFF CACHE BOOL "" FORCE)
    set(STRATA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(STRATA_BUILD_OVERLAY  OFF CACHE BOOL "" FORCE)
    set(STRATA_BUILD_DX12     OFF CACHE BOOL "" FORCE)
    set(STRATA_BUILD_VULKAN   OFF CACHE BOOL "" FORCE)
    set(STRATA_BUILD_WEBGPU   OFF CACHE BOOL "" FORCE)
    set(STRATA_BUILD_METAL    OFF CACHE BOOL "" FORCE)
    set(STRATA_BUILD_OPENXR   OFF CACHE BOOL "" FORCE)
    set(STRATA_BUILD_OPENVR   OFF CACHE BOOL "" FORCE)
    set(STRATA_BUILD_FUZZERS  OFF CACHE BOOL "" FORCE)
    set(STRATA_INSTALL        OFF CACHE BOOL "" FORCE)
    set(STRATA_ASAN           OFF CACHE BOOL "" FORCE)
    set(STRATA_AVX2           OFF CACHE BOOL "" FORCE)
    if(WIN32)
        set(STRATA_BUILD_DX11   ON  CACHE BOOL "" FORCE)
        set(STRATA_BUILD_OPENGL OFF CACHE BOOL "" FORCE)
    else()
        set(STRATA_BUILD_DX11   OFF CACHE BOOL "" FORCE)
        set(STRATA_BUILD_OPENGL ON  CACHE BOOL "" FORCE)
    endif()
    set(STRATA_STATIC_CRT ${PRISM_STATIC_CRT} CACHE BOOL "" FORCE)
    set(STRATA_FAST_MATH  OFF                 CACHE BOOL "" FORCE) # file sizes and sort keys are integers
    set(STRATA_LTO        ${PRISM_LTO}        CACHE BOOL "" FORCE)

    if(EXISTS "${PRISM_STRATA_DIR}/strata/CMakeLists.txt")
        message(STATUS "prism: using local strata checkout at ${PRISM_STRATA_DIR}")
        add_subdirectory("${PRISM_STRATA_DIR}" strata EXCLUDE_FROM_ALL)
    else()
        message(STATUS "prism: local strata not found at '${PRISM_STRATA_DIR}', fetching ${PRISM_STRATA_GIT_TAG}")
        include(FetchContent)
        FetchContent_Declare(
            strata
            GIT_REPOSITORY https://github.com/root-skaii/strata-ui-framework.git
            GIT_TAG        ${PRISM_STRATA_GIT_TAG}
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(strata)
    endif()
endif()

add_subdirectory(src)
