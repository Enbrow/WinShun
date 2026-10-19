# The screen capture's parts that need no QML (docs/architecture.md, 截图):
# the geometry the tests check (Geometry), the pictures (Grabber: Windows.
# Graphics.Capture through C++/WinRT from the Windows SDK, BitBlt; Hdr), the
# windows on the screen (WindowSnapshot) and the PrtScn takeover
# (PrintScreen). What the overlays bind to (CaptureSession, CaptureImage)
# and the controller (Capture) are in this folder too, but built into
# WinShun's QML module (src/app/CMakeLists.txt), which registers their types.
add_library(wscapture STATIC
    Geometry.cpp Geometry.h
    Hdr.cpp Hdr.h
    PrintScreen.cpp PrintScreen.h
    WindowSnapshot.cpp WindowSnapshot.h
    Grabber.cpp Grabber.h
)
target_include_directories(wscapture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wscapture PUBLIC winshun_options Qt6::Gui
    PRIVATE windowsapp d3d11 dxgi dwmapi user32 gdi32 advapi32)
