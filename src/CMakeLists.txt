file(GLOB_RECURSE PRISM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(prismfs WIN32 ${PRISM_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/prism.manifest" "${CMAKE_CURRENT_SOURCE_DIR}/prismfs.rc")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/prismfs.rc" PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../resources/prismfs.ico")

target_include_directories(prismfs PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(prismfs PRIVATE
    strata::strata prism_options
    d3d11.lib dxgi.lib dcomp.lib dwmapi.lib shell32.lib ole32.lib user32.lib gdi32.lib shlwapi.lib propsys.lib uuid.lib shcore.lib version.lib bcrypt.lib advapi32.lib windowscodecs.lib
)

# translations ship beside the exe (lang/*.lang, plain text); the template and the readme stay in the repo
file(GLOB PRISM_LANG_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../lang/*.lang")
list(FILTER PRISM_LANG_FILES EXCLUDE REGEX "template\\.lang$")
add_custom_command(TARGET prismfs POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:prismfs>/lang"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PRISM_LANG_FILES} "$<TARGET_FILE_DIR:prismfs>/lang")

# logic tests for core/ and model/ (no window, no gpu): build/<preset>/bin/prismfs_selftest.exe
add_executable(prismfs_selftest
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/selftest.cpp"
    core/text.cpp core/path.cpp core/file_kind.cpp core/query.cpp core/directory_loader.cpp core/file_ops.cpp core/file_info.cpp
    model/listing.cpp model/folder_sorts.cpp model/sessions.cpp model/tags.cpp model/tab.cpp model/sidebar_layout.cpp model/visit_log.cpp core/places.cpp core/path_probe.cpp core/drive_watcher.cpp core/shell_menu.cpp core/bitmap.cpp core/default_manager.cpp core/batch_rename.cpp core/folder_sizes.cpp core/duplicates.cpp core/process.cpp core/disk_usage.cpp core/treemap.cpp core/compare.cpp core/transfer.cpp core/file_index.cpp core/links.cpp core/empty_folders.cpp core/image_tools.cpp core/templates.cpp core/archive.cpp core/git_status.cpp core/log.cpp core/i18n.cpp core/recycle.cpp core/crash.cpp core/terminal.cpp core/virtual_files.cpp)
target_include_directories(prismfs_selftest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(prismfs_selftest PRIVATE strata::strata prism_options shell32.lib ole32.lib user32.lib uuid.lib version.lib bcrypt.lib advapi32.lib shlwapi.lib windowscodecs.lib)
