find_package(Qt6 REQUIRED COMPONENTS Test QuickTest)

qt_add_executable(tst_windowvisibility tst_windowvisibility.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/WindowEffects.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Foreground.cpp)
target_include_directories(tst_windowvisibility PRIVATE ${PROJECT_SOURCE_DIR}/src/app ${PROJECT_SOURCE_DIR}/src/core)
target_link_libraries(tst_windowvisibility PRIVATE winshun_options Qt6::Gui Qt6::Test dwmapi ole32 shell32 user32)
set_target_properties(tst_windowvisibility PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_windowvisibility COMMAND tst_windowvisibility)
set_tests_properties(tst_windowvisibility PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_scrollalignment tst_scrollalignment.cpp)
target_compile_definitions(tst_scrollalignment PRIVATE WINSHUN_QML_DIR="${PROJECT_SOURCE_DIR}/src/app/qml")
target_link_libraries(tst_scrollalignment PRIVATE Qt6::Quick Qt6::Test Qt6::QuickTest)
set_target_properties(tst_scrollalignment PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE WIN32_EXECUTABLE FALSE)
foreach(scale IN ITEMS 1 1.25 1.5 1.75 2)
    add_test(NAME tst_scrollalignment_${scale} COMMAND tst_scrollalignment -o -,txt)
    set_tests_properties(tst_scrollalignment_${scale} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_SCALE_FACTOR=${scale}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
endforeach()

qt_add_executable(tst_numberhints tst_numberhints.cpp ${PROJECT_SOURCE_DIR}/src/app/Settings.cpp)
target_include_directories(tst_numberhints PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_numberhints PRIVATE wscore Qt6::Quick Qt6::Test)
target_compile_definitions(tst_numberhints PRIVATE WINSHUN_TEST_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(tst_numberhints PROPERTIES WIN32_EXECUTABLE FALSE)
if(QT6_IS_SHARED_LIBS_BUILD)
    set_target_properties(tst_numberhints PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
endif()
add_test(NAME tst_numberhints COMMAND tst_numberhints)
set_tests_properties(tst_numberhints PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The screen capture's geometry, HDR mapping and state machine (src/capture),
# without windows.
qt_add_executable(tst_capture tst_capture.cpp ${PROJECT_SOURCE_DIR}/src/capture/CaptureSession.cpp)
target_link_libraries(tst_capture PRIVATE wscapture Qt6::Qml Qt6::Test)
# Qt6::Qml only for QML_ELEMENT's macros: no QML imports to look for.
set_target_properties(tst_capture PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_capture COMMAND tst_capture)
set_tests_properties(tst_capture PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_appearance tst_appearance.cpp
    ${PROJECT_SOURCE_DIR}/src/app/Settings.cpp
    ${PROJECT_SOURCE_DIR}/src/app/SystemTheme.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/WindowEffects.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Foreground.cpp)
target_include_directories(tst_appearance PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_compile_definitions(tst_appearance PRIVATE WINSHUN_QML_DIR="${PROJECT_SOURCE_DIR}/src/app/qml")
target_link_libraries(tst_appearance PRIVATE wscore Qt6::Test Qt6::Quick dwmapi ole32 shell32 user32)
get_target_property(appearance_qt_type Qt6::Core TYPE)
if(appearance_qt_type STREQUAL "SHARED_LIBRARY")
    set_target_properties(tst_appearance PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
endif()
foreach(scale IN ITEMS 1 1.25 1.5 2)
    add_test(NAME tst_appearance_${scale} COMMAND tst_appearance)
    set_tests_properties(tst_appearance_${scale} PROPERTIES
        ENVIRONMENT "QT_QUICK_BACKEND=software;QT_ENABLE_HIGHDPI_SCALING=0;QT_SCALE_FACTOR=${scale}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endforeach()

qt_add_executable(tst_core tst_core.cpp)
target_link_libraries(tst_core PRIVATE wscore wsextract miniz Qt6::Test)

qt_add_executable(tst_diagnostics tst_diagnostics.cpp ${PROJECT_SOURCE_DIR}/src/app/Diagnostics.cpp
    ${PROJECT_SOURCE_DIR}/src/service/Log.cpp)
target_include_directories(tst_diagnostics PRIVATE ${PROJECT_SOURCE_DIR}/src/app ${PROJECT_SOURCE_DIR}/src/service)
target_link_libraries(tst_diagnostics PRIVATE wscore miniz Qt6::Test)
add_test(NAME tst_diagnostics COMMAND tst_diagnostics)
set_tests_properties(tst_diagnostics PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_clipboard tst_clipboard.cpp)
target_link_libraries(tst_clipboard PRIVATE wscore Qt6::Test)

# The index service (src/service) through its pipe client (ServiceVolumeAccess),
# started as `WinShunService.exe --console --pipe <random>` on a real volume
# (as administrator): the MFT the same as read directly, the journal
# followed, volumes let go of, the service gone and back, the handshake.
qt_add_executable(tst_service tst_service.cpp)
target_link_libraries(tst_service PRIVATE wscore Qt6::Test)
target_compile_definitions(tst_service PRIVATE WINSHUN_SERVICE_EXE="$<TARGET_FILE:WinShunService>")
add_dependencies(tst_service WinShunService)

qt_add_executable(tst_keylistener tst_keylistener.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/KeyListener.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/ServiceKeyListener.cpp)
target_include_directories(tst_keylistener PRIVATE ${PROJECT_SOURCE_DIR}/src/app/platform)
target_link_libraries(tst_keylistener PRIVATE wscore Qt6::Test user32 advapi32)
target_compile_definitions(tst_keylistener PRIVATE WINSHUN_SERVICE_EXE="$<TARGET_FILE:WinShunService>")
add_dependencies(tst_keylistener WinShunService)

# The favorites: their file, the store, and the search's list with nothing typed.
qt_add_executable(tst_favorites tst_favorites.cpp)
target_link_libraries(tst_favorites PRIVATE wscore Qt6::Test)

# Colour values as text: app code that needs nothing but QColor.
qt_add_executable(tst_color tst_color.cpp ${PROJECT_SOURCE_DIR}/src/app/ColorText.cpp)
target_include_directories(tst_color PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_color PRIVATE Qt6::Gui Qt6::Test)

# Long names and paths shortened for a row: extension, end and matches kept.
qt_add_executable(tst_elide tst_elide.cpp ${PROJECT_SOURCE_DIR}/src/app/Elide.cpp)
target_include_directories(tst_elide PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_elide PRIVATE Qt6::Gui Qt6::Test)

# Which side of a box (the text caret, a search box) the clipboard window
# opens on, as Placement decides it (BoxSide.h, GitHub #38).
qt_add_executable(tst_placement tst_placement.cpp)
target_include_directories(tst_placement PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_placement PRIVATE winshun_options Qt6::Core Qt6::Test)

# Backups of the settings: what goes in, unchanged ones skipped, old ones
# removed, and restoring (a path leaving the folder refused).
qt_add_executable(tst_backup tst_backup.cpp ${PROJECT_SOURCE_DIR}/src/app/BackupFiles.cpp)
target_include_directories(tst_backup PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_backup PRIVATE winshun_options miniz Qt6::Core Qt6::Test)

# Sites' icons as read from what a site sends (PNG, ICO), damaged ones too;
# SVG icons (SvgIcon) drawn, and those naming anything outside turned away.
qt_add_executable(tst_siteicon tst_siteicon.cpp ${PROJECT_SOURCE_DIR}/src/app/SiteIconImage.cpp
    ${PROJECT_SOURCE_DIR}/src/app/SvgIcon.cpp)
target_include_directories(tst_siteicon PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_siteicon PRIVATE Qt6::Gui Qt6::Svg Qt6::Test)

# The words in pictures: PP-OCRv6 through ONNX Runtime, the real models.
qt_add_executable(tst_ocr tst_ocr.cpp ${PROJECT_SOURCE_DIR}/src/app/platform/PaddleOcr.cpp)
target_include_directories(tst_ocr PRIVATE ${PROJECT_SOURCE_DIR}/src/app/platform)
target_link_libraries(tst_ocr PRIVATE winshun_options Qt6::Gui Qt6::Test onnxruntime_headers)

# What a drop target gets when results are dragged out of Win顺.
qt_add_executable(tst_dragout tst_dragout.cpp ${PROJECT_SOURCE_DIR}/src/app/platform/DragOut.cpp)
target_include_directories(tst_dragout PRIVATE ${PROJECT_SOURCE_DIR}/src/app/platform)
target_link_libraries(tst_dragout PRIVATE winshun_options Qt6::Core Qt6::Test)

# The MCP server (src/mcp): the protocol, the tools on a hand-made index, the
# pipe, and the relay WinShunMcp.exe, whose path it is told; and the
# plugins' commands as tools (plugin::AiBridge), with plugin_fake.exe through
# the runner as in tst_plugin.
qt_add_executable(tst_mcp tst_mcp.cpp)
target_link_libraries(tst_mcp PRIVATE wsmcp wsplugin Qt6::Test)
target_compile_definitions(tst_mcp PRIVATE
    WINSHUN_MCP_RELAY="$<TARGET_FILE:WinShunMcp>"
    WINSHUN_PLUGIN_HOST="$<TARGET_FILE:WinShunPluginHost>"
    WINSHUN_PLUGIN_FAKE="$<TARGET_FILE:plugin_fake>")
add_dependencies(tst_mcp WinShunMcp WinShunPluginHost plugin_fake)

# The pieces of the tools plugin that compute (src/tools-plugin), built in
# again: the calculator and the conversions have edges (division by zero,
# overflow, bad input) worth pinning down.
qt_add_executable(tst_tools tst_tools.cpp
    ${PROJECT_SOURCE_DIR}/src/tools-plugin/Calc.cpp ${PROJECT_SOURCE_DIR}/src/tools-plugin/Units.cpp
    ${PROJECT_SOURCE_DIR}/src/tools-plugin/Color.cpp ${PROJECT_SOURCE_DIR}/src/tools-plugin/Codec.cpp)
target_include_directories(tst_tools PRIVATE ${PROJECT_SOURCE_DIR}/src/tools-plugin)
target_link_libraries(tst_tools PRIVATE winshun_options Qt6::Test bcrypt)

# winshun.semantic's pieces (src/semantic-plugin), built in again: passages,
# the vector store, the model services' settings.
qt_add_executable(tst_semantic tst_semantic.cpp
    ${PROJECT_SOURCE_DIR}/src/semantic-plugin/Chunker.cpp ${PROJECT_SOURCE_DIR}/src/semantic-plugin/Store.cpp
    ${PROJECT_SOURCE_DIR}/src/semantic-plugin/Embedder.cpp)
target_include_directories(tst_semantic PRIVATE ${PROJECT_SOURCE_DIR}/src/semantic-plugin ${PROJECT_SOURCE_DIR}/src/plugins-sdk)
target_link_libraries(tst_semantic PRIVATE winshun_options Qt6::Test winhttp)

# The plugin system (src/plugin): manifests, items, zero-code, the catalog,
# and plugin processes started through the runner WinShunPluginHost.exe,
# played by plugin_fake.exe (no Qt, as a plugin would be); both paths are
# told to the test.
add_executable(plugin_fake plugin_fake.cpp)
target_link_libraries(plugin_fake PRIVATE wsplugin_lite)
set_target_properties(plugin_fake PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
qt_add_executable(tst_plugin tst_plugin.cpp)
target_link_libraries(tst_plugin PRIVATE wsplugin miniz Qt6::Test) # miniz: the test writes zips to install
target_compile_definitions(tst_plugin PRIVATE
    WINSHUN_PLUGIN_HOST="$<TARGET_FILE:WinShunPluginHost>"
    WINSHUN_PLUGIN_FAKE="$<TARGET_FILE:plugin_fake>"
    WINSHUN_TEST_PLUGINS="${CMAKE_CURRENT_SOURCE_DIR}/plugins")
add_dependencies(tst_plugin WinShunPluginHost plugin_fake)

add_test(NAME tst_core COMMAND tst_core)
add_test(NAME tst_service COMMAND tst_service)
add_test(NAME tst_keylistener COMMAND tst_keylistener)
add_test(NAME tst_clipboard COMMAND tst_clipboard)
add_test(NAME tst_favorites COMMAND tst_favorites)
add_test(NAME tst_color COMMAND tst_color)
add_test(NAME tst_elide COMMAND tst_elide)
add_test(NAME tst_placement COMMAND tst_placement)
add_test(NAME tst_dragout COMMAND tst_dragout)
add_test(NAME tst_siteicon COMMAND tst_siteicon)
add_test(NAME tst_backup COMMAND tst_backup)
add_test(NAME tst_ocr COMMAND tst_ocr)
add_test(NAME tst_tools COMMAND tst_tools)
add_test(NAME tst_semantic COMMAND tst_semantic)
add_test(NAME tst_mcp COMMAND tst_mcp)
add_test(NAME tst_plugin COMMAND tst_plugin)
# Let ctest find the Qt DLLs without a deploy step.
set_tests_properties(tst_core tst_service tst_keylistener tst_clipboard tst_favorites tst_backup tst_color tst_elide tst_placement tst_dragout tst_siteicon tst_ocr tst_tools tst_semantic tst_mcp tst_plugin PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
