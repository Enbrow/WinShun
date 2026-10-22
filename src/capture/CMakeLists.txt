# The screen capture's parts that need no QML (docs/architecture.md, 截图):
# the geometry the tests check (Geometry), the pictures (Grabber: Windows.
# Graphics.Capture through C++/WinRT from the Windows SDK, BitBlt; Hdr), the
# windows on the screen (WindowSnapshot) and the PrtScn takeover
# (PrintScreen), and the annotations' core (Annotation*: the document, its
# commands, the compositor that draws tiles and exports, the project format
# and the tools' styles; docs/design-capture-roadmap.md, 3) and the editing
# window's zoom and tile arithmetic (EditView), and the smart redaction's
# rules (SmartRedact). What the
# overlays bind to (CaptureSession, CaptureImage) and the controller
# (Capture) are in this folder too, but built into WinShun's QML module
# (src/app/CMakeLists.txt), which registers their types.
add_library(wscapture STATIC
    Geometry.cpp Geometry.h
    Hdr.cpp Hdr.h
    PrintScreen.cpp PrintScreen.h
    WindowSnapshot.cpp WindowSnapshot.h
    Grabber.cpp Grabber.h
    EditView.cpp EditView.h
    Annotation.cpp Annotation.h
    AnnotationCommands.cpp AnnotationCommands.h
    AnnotationRender.cpp AnnotationRender.h
    AnnotationFormat.cpp AnnotationFormat.h
    AnnotationStyles.cpp AnnotationStyles.h
    SmartRedact.cpp SmartRedact.h
)
target_include_directories(wscapture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# miniz: the projects' zip (AnnotationFormat).
target_link_libraries(wscapture PUBLIC winshun_options Qt6::Gui
    PRIVATE miniz windowsapp d3d11 dxgi dwmapi user32 gdi32 advapi32)
