file(GLOB_RECURSE PRISM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(prismfs WIN32 ${PRISM_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/prism.manifest")

target_include_directories(prismfs PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(prismfs PRIVATE
    strata::strata prism_options
    d3d11.lib dxgi.lib dwmapi.lib shell32.lib ole32.lib user32.lib gdi32.lib shlwapi.lib propsys.lib uuid.lib shcore.lib
)
