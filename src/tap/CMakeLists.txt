# WinShunTap.dll: loaded into explorer.exe by Windows' XAML diagnostics
# (InitializeXamlDiagnosticsEx, called by platform/TaskbarStartGap) to put a
# right margin on the taskbar's Start button, so the search box has room
# there. Plain Win32 and C++/WinRT headers, the C runtime linked in: it runs
# inside Explorer and must not drag anything of ours along. Never loaded from
# the install folder: WinShun copies it to %ProgramData%\WinShun\tap\<hash>\.
add_library(WinShunTap SHARED
    WinShunTap.cpp
    WinShunTap.def
    TapProtocol.h
)
# C++/WinRT finds combase at run time: no windowsapp, no api-set imports.
target_link_libraries(WinShunTap PRIVATE winshun_options kernel32 ole32 oleaut32 user32)
set_target_properties(WinShunTap PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    PREFIX "")

# What the app shares with the DLL: the channel's layout and names.
add_library(wstap_api INTERFACE)
target_include_directories(wstap_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

install(TARGETS WinShunTap RUNTIME DESTINATION .)
