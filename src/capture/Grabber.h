#pragma once

#include <QImage>
#include <QRect>
#include <QString>

#include <windows.h>

#include <memory>
#include <vector>

namespace ws::capture {

// Pictures of every monitor, as they are on the screen (docs/design-capture.md, 4.1).
//
// Windows.Graphics.Capture (WGC) first: it gets HDR monitors right (BitBlt
// returns them washed out) and, kept warm, is the faster path (stage 0:
// 11–12 ms per 2560x1600 monitor against 30–33 ms for BitBlt). Warm means
// the D3D11 device (120+ ms the first time in a process) and one capture
// item per monitor (CreateForMonitor, 22–26 ms each) made ahead, when the
// capture is turned on and again when the monitors change; each picture
// then only needs a frame pool and a session. No yellow border: an
// unpackaged program is allowed borderless capture
// (GraphicsCaptureAccess::RequestAccessAsync(Borderless) answers Allowed),
// then IsBorderRequired(false). BitBlt is the way out when WGC fails.
//
// Everything WGC runs on a thread of its own (multi-threaded apartment),
// which also does the preparing: turning the capture on never holds up the
// GUI. grab() waits for its pictures.
class Grabber {
public:
    struct Shot {
        HMONITOR monitor = nullptr;
        QRect rect; // physical pixels, virtual desktop
        QImage image; // the monitor's pixels, Format_RGB32
        bool wgc = false; // else BitBlt
        bool hdr = false; // mapped from scRGB (Hdr.h)
    };

    Grabber();
    ~Grabber();

    Grabber(const Grabber&) = delete;
    Grabber& operator=(const Grabber&) = delete;

    // The monitors changed: their capture items are made again.
    void refresh();
    // A picture of every monitor; blocks until they are taken. `report`
    // (may be null) tells how, for the log.
    std::vector<Shot> grab(QString* report = nullptr);

private:
    struct Worker;
    std::unique_ptr<Worker> m_worker;
};

} // namespace ws::capture
