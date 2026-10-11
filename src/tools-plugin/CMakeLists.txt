# WinShunTools.exe: the winshun.tools plugin that comes with Win顺 (plugins/
# winshun.tools), a resident C++ plugin on the single-header SDK
# (src/plugins-sdk/winshun.h): calculator, unit conversion, colours,
# timestamps, UUIDs, Base64 and hashes. No Qt, the C runtime linked in, so
# it starts in milliseconds and runs in the strict sandbox (low integrity).
# The pieces that compute (Calc, Units, Color, Codec) are plain C++ that
# tests/tst_tools.cpp builds in again.
add_executable(WinShunTools
    main.cpp
    Calc.cpp Calc.h
    Codec.cpp Codec.h
    Color.cpp Color.h
    Units.cpp Units.h
    ${PROJECT_SOURCE_DIR}/resources/WinShunTools.rc
)
target_include_directories(WinShunTools PRIVATE ${PROJECT_SOURCE_DIR}/src/plugins-sdk)
target_link_libraries(WinShunTools PRIVATE winshun_options bcrypt)
set_target_properties(WinShunTools PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
