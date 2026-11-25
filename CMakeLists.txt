cmake_minimum_required(VERSION 4.3)

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

set(PRISM_STRATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../DxStrataFramework" CACHE PATH "DxStrataFramework checkout")

include(prism_options)

# strata is consumed as a library only: no sandbox, tests, overlay or dx12. its runtime / LTO settings come from
# prism_options (set before this point, so both agree)
if(NOT TARGET strata::strata)
    if(NOT EXISTS "${PRISM_STRATA_DIR}/strata/CMakeLists.txt")
        message(FATAL_ERROR "prism: no strata checkout at '${PRISM_STRATA_DIR}' -- set PRISM_STRATA_DIR")
    endif()
    set(STRATA_BUILD_SANDBOX OFF)
    set(STRATA_BUILD_TESTS   OFF)
    set(STRATA_BUILD_OVERLAY OFF)
    set(STRATA_BUILD_DX12    OFF)
    set(STRATA_BUILD_OPENXR  OFF)
    set(STRATA_BUILD_OPENVR  OFF)
    set(STRATA_BUILD_FUZZERS OFF)
    set(STRATA_INSTALL       OFF)
    set(STRATA_ASAN          OFF)
    set(STRATA_AVX2          OFF)
    if(WIN32)
        set(STRATA_BUILD_DX11   ON)
    else()
        set(STRATA_BUILD_DX11   OFF)
        set(STRATA_BUILD_OPENGL ON)
    endif()
    set(STRATA_STATIC_CRT    ${PRISM_STATIC_CRT})
    set(STRATA_FAST_MATH     OFF) # (file sizes and sort keys are integers; fast-math buys the explorer nothing)
    set(STRATA_LTO           ${PRISM_LTO})
    add_subdirectory("${PRISM_STRATA_DIR}" strata EXCLUDE_FROM_ALL)
endif()

add_subdirectory(src)
