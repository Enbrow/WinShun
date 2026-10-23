#include "CaptureTools.h"

#include "Geometry.h"
#include "WindowSnapshot.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace ws::capture {

// ---- colour formats ---------------------------------------------------------------

QString colorText(QRgb color, ColorFormat format)
{
    const int r = qRed(color);
    const int g = qGreen(color);
    const int b = qBlue(color);
    switch (format) {
    case ColorFormat::Rgb:
        return QStringLiteral("rgb(%1, %2, %3)").arg(r).arg(g).arg(b);
    case ColorFormat::Hsl: {
        const double fr = r / 255.0;
        const double fg = g / 255.0;
        const double fb = b / 255.0;
        const double high = std::max({fr, fg, fb});
        const double low = std::min({fr, fg, fb});
        const double delta = high - low;
        const double l = (high + low) / 2;
        double h = 0;
        double s = 0;
        if (delta > 0) {
            s = delta / (1 - std::abs(2 * l - 1));
            if (high == fr)
                h = std::fmod((fg - fb) / delta, 6.0);
            else if (high == fg)
                h = (fb - fr) / delta + 2;
            else
                h = (fr - fg) / delta + 4;
            h *= 60;
            if (h < 0)
                h += 360;
        }
        int hue = static_cast<int>(std::lround(h));
        if (hue >= 360)
            hue -= 360;
        return QStringLiteral("hsl(%1, %2%, %3%)").arg(hue).arg(std::lround(s * 100)).arg(std::lround(l * 100));
    }
    case ColorFormat::Hex:
        break;
    }
    return colorHex(color);
}

ColorFormat nextColorFormat(ColorFormat format)
{
    switch (format) {
    case ColorFormat::Hex:
        return ColorFormat::Rgb;
    case ColorFormat::Rgb:
        return ColorFormat::Hsl;
    case ColorFormat::Hsl:
        break;
    }
    return ColorFormat::Hex;
}

ColorFormat colorFormatFromName(const QString& name)
{
    const QString n = name.trimmed().toLower();
    return n == u"rgb" ? ColorFormat::Rgb : n == u"hsl" ? ColorFormat::Hsl : ColorFormat::Hex;
}

QString colorFormatName(ColorFormat format)
{
    return format == ColorFormat::Rgb ? QStringLiteral("rgb") : format == ColorFormat::Hsl ? QStringLiteral("hsl") : QStringLiteral("hex");
}

// ---- the delayed capture ------------------------------------------------------------

int normalizedDelay(int seconds)
{
    if (seconds <= 0)
        return 5;
    int best = 3;
    for (const int choice : {3, 5, 10}) {
        if (std::abs(choice - seconds) < std::abs(best - seconds))
            best = choice;
    }
    return best;
}

qint64 countdownRemaining(qint64 elapsedMs, int totalSeconds)
{
    return std::max<qint64>(0, qint64(totalSeconds) * 1000 - elapsedMs);
}

int countdownSeconds(qint64 elapsedMs, int totalSeconds)
{
    return static_cast<int>((countdownRemaining(elapsedMs, totalSeconds) + 999) / 1000);
}

// ---- Shift+R ----------------------------------------------------------------------------

std::optional<RememberedSelection> lastSelectionOn(const QRect& remembered, const std::vector<QRect>& monitors)
{
    if (remembered.isEmpty())
        return std::nullopt;
    int best = -1;
    qint64 bestArea = 0;
    for (std::size_t i = 0; i < monitors.size(); ++i) {
        const QRect common = monitors[i].intersected(remembered);
        const qint64 area = qint64(std::max(0, common.width())) * std::max(0, common.height());
        if (area > bestArea) {
            best = static_cast<int>(i);
            bestArea = area;
        }
    }
    if (best < 0)
        return std::nullopt;
    const QRect monitor = monitors[static_cast<std::size_t>(best)];
    const QRect own(QPoint(0, 0), monitor.size());
    return RememberedSelection {best, moveInto(remembered.translated(-monitor.topLeft()), own)};
}

// ---- the mouse cursor -------------------------------------------------------------------

bool drawCursor(QImage& image, QPoint hotspot, HCURSOR cursor)
{
    if (image.isNull() || !cursor)
        return false;
    ICONINFO info {};
    if (!::GetIconInfo(cursor, &info))
        return false;
    BITMAP bitmap {};
    const HBITMAP shape = info.hbmColor ? info.hbmColor : info.hbmMask;
    const bool got = shape && ::GetObjectW(shape, sizeof bitmap, &bitmap) != 0;
    const QPoint grip(static_cast<int>(info.xHotspot), static_cast<int>(info.yHotspot));
    if (info.hbmColor)
        ::DeleteObject(info.hbmColor);
    if (info.hbmMask)
        ::DeleteObject(info.hbmMask);
    if (!got)
        return false;
    const int w = bitmap.bmWidth;
    const int h = info.hbmColor ? bitmap.bmHeight : bitmap.bmHeight / 2; // a monochrome cursor: AND mask over XOR mask
    const QRect area(hotspot - grip, QSize(w, h));
    const QRect inside = area.intersected(image.rect());
    if (w <= 0 || h <= 0 || inside.isEmpty())
        return false;
    if (image.format() != QImage::Format_RGB32 && image.format() != QImage::Format_ARGB32_Premultiplied)
        image = image.convertToFormat(QImage::Format_RGB32);

    BITMAPINFO header {};
    header.bmiHeader.biSize = sizeof header.bmiHeader;
    header.bmiHeader.biWidth = w;
    header.bmiHeader.biHeight = -h;
    header.bmiHeader.biPlanes = 1;
    header.bmiHeader.biBitCount = 32;
    header.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = ::CreateDIBSection(nullptr, &header, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib)
        return false;
    HDC dc = ::CreateCompatibleDC(nullptr);
    HGDIOBJ old = ::SelectObject(dc, dib);
    // The picture's pixels under the cursor in, opaque (a grabbed pixel may
    // carry alpha 0), what lies off the picture black.
    auto* target = static_cast<QRgb*>(bits);
    std::fill(target, target + static_cast<std::size_t>(w) * h, 0xFF000000u);
    for (int y = inside.top(); y <= inside.bottom(); ++y) {
        const auto* in = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        QRgb* out = target + static_cast<std::size_t>(y - area.top()) * w + (inside.left() - area.left());
        for (int x = inside.left(); x <= inside.right(); ++x)
            *out++ = in[x] | 0xFF000000u;
    }
    const BOOL drawn = ::DrawIconEx(dc, 0, 0, cursor, w, h, 0, nullptr, DI_NORMAL);
    ::GdiFlush();
    if (drawn) {
        for (int y = inside.top(); y <= inside.bottom(); ++y) {
            auto* out = reinterpret_cast<QRgb*>(image.scanLine(y));
            const QRgb* in = target + static_cast<std::size_t>(y - area.top()) * w + (inside.left() - area.left());
            for (int x = inside.left(); x <= inside.right(); ++x)
                out[x] = *in++ | 0xFF000000u;
        }
    }
    ::SelectObject(dc, old);
    ::DeleteDC(dc);
    ::DeleteObject(dib);
    return drawn != FALSE;
}

// ---- UI Automation ------------------------------------------------------------------------

bool wantsUia(const QString& windowClass, int childWindows)
{
    // Never Chromium (checked first: such a frame may have no child window to
    // stop us). Its accessibility tree is built on the first UI Automation
    // question and stays on, and VS Code then turns "optimized for a screen
    // reader" on for the user until it restarts. Its child windows stand.
    if (WindowSnapshot::isChromiumClass(windowClass.toStdWString()))
        return false;
    if (childWindows <= 0)
        return true;
    static const QStringList own {
        QStringLiteral("MozillaWindowClass"),
        QStringLiteral("ApplicationFrameWindow"),
        QStringLiteral("Windows.UI.Core.CoreWindow"),
        QStringLiteral("WinUIDesktopWin32WindowClass"),
        QStringLiteral("XamlWindow"),
    };
    return own.contains(windowClass);
}

std::vector<QRect> uiaControls(const std::vector<QRect>& chain, const QRect& window)
{
    std::vector<QRect> out;
    for (const QRect& element : chain) {
        const QRect cut = element.intersected(window);
        if (cut.isEmpty() || cut.width() < 8 || cut.height() < 8 || cut == window)
            continue;
        if (std::ranges::find(out, cut) == out.end())
            out.push_back(cut);
    }
    return out;
}

bool preferUiaChain(const std::vector<QRect>& candidate, const std::vector<QRect>& current)
{
    const auto smallest = [](const std::vector<QRect>& chain) {
        qint64 area = std::numeric_limits<qint64>::max();
        for (const QRect& rect : chain) {
            if (rect.width() >= 8 && rect.height() >= 8)
                area = std::min(area, qint64(rect.width()) * rect.height());
        }
        return area;
    };
    const qint64 next = smallest(candidate);
    const qint64 previous = smallest(current);
    return next < previous || (next == previous && candidate.size() > current.size());
}

bool UiaGate::allowed(quintptr window) const
{
    const auto it = m_states.find(window);
    return it == m_states.end() || (!it->second.slow && it->second.empty < kEmptyLimit);
}

void UiaGate::record(quintptr window, qint64 elapsedMs, int controls)
{
    State& state = m_states[window];
    if (elapsedMs > kBudgetMs)
        state.slow = true;
    state.empty = controls > 0 ? 0 : state.empty + 1;
}

bool UiaGate::slow(quintptr window) const
{
    const auto it = m_states.find(window);
    return it != m_states.end() && it->second.slow;
}

} // namespace ws::capture
