file(GLOB_RECURSE PRISM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(prismfs WIN32 ${PRISM_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/prism.manifest" "${CMAKE_CURRENT_SOURCE_DIR}/prismfs.rc")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/prismfs.rc" PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../resources/prismfs.ico")

target_include_directories(prismfs PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(prismfs PRIVATE
    strata::strata prism_options
    d3d11.lib dxgi.lib dwmapi.lib shell32.lib ole32.lib user32.lib gdi32.lib shlwapi.lib propsys.lib uuid.lib shcore.lib version.lib bcrypt.lib advapi32.lib
)

# logic tests for core/ and model/ (no window, no gpu): build/<preset>/bin/prismfs_selftest.exe
add_executable(prismfs_selftest
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/selftest.cpp"
    core/text.cpp core/path.cpp core/file_kind.cpp core/query.cpp core/directory_loader.cpp core/file_ops.cpp core/file_info.cpp
    model/listing.cpp model/tab.cpp model/sidebar_layout.cpp model/visit_log.cpp core/places.cpp core/path_probe.cpp)
target_include_directories(prismfs_selftest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(prismfs_selftest PRIVATE strata::strata prism_options shell32.lib ole32.lib user32.lib uuid.lib version.lib bcrypt.lib advapi32.lib shlwapi.lib)
