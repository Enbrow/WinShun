# WinShunSearch.exe: the button the user pins to the taskbar (see main.cpp).
# Plain Win32 with the C runtime linked in: a click starts it, so it must
# load next to nothing. The linker's default manifest runs it as invoker, not
# elevated like WinShun.exe.
add_executable(WinShunSearch WIN32 main.cpp PasteAsFile.cpp PasteAsFile.h
    ${PROJECT_SOURCE_DIR}/src/core/PasteFile.cpp ${PROJECT_SOURCE_DIR}/resources/WinShunSearch.rc)
# core/PasteFile and PasteOffer.h: pure C++ and Win32, no Qt.
target_include_directories(WinShunSearch PRIVATE ${PROJECT_SOURCE_DIR}/src/core)
# windowsapp: TaskbarManager (--pin), through C++/WinRT from the Windows SDK.
# comctl32 (TaskDialog) is for 粘贴为文件's messages alone: loaded when one is
# shown, not at every click on the button. WIC comes through COM.
target_link_libraries(WinShunSearch PRIVATE winshun_options taskschd ole32 oleaut32 shell32 advapi32 user32 windowsapp
    comctl32 windowscodecs uuid delayimp)
target_link_options(WinShunSearch PRIVATE /DELAYLOAD:comctl32.dll)
set_target_properties(WinShunSearch PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

install(TARGETS WinShunSearch RUNTIME DESTINATION .)
