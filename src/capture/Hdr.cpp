#include "Hdr.h"

#include <windows.h>

#include <vector>
#include <cwchar>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace ws::capture {

// Whether the monitor (by its GDI name, "\\.\DISPLAY1") shows HDR, and at
// which SDR white level Windows composes SDR content there.
DisplayColorMode displayColorMode(const wchar_t* device)
{
    DisplayColorMode mode;
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return mode;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
        return mode;
    for (UINT32 i = 0; i < pathCount; ++i) {
        const DISPLAYCONFIG_PATH_INFO& path = paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof source;
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (::DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || std::wcscmp(source.viewGdiDeviceName, device) != 0)
            continue;
        // Windows 11 24H2 tells HDR from wide colour gamut; before it, the
        // older call's "advanced colour" on a non-WCG monitor means HDR.
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 info2 {};
        info2.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
        info2.header.size = sizeof info2;
        info2.header.adapterId = path.targetInfo.adapterId;
        info2.header.id = path.targetInfo.id;
        if (::DisplayConfigGetDeviceInfo(&info2.header) == ERROR_SUCCESS) {
            mode.hdr = info2.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR;
        } else {
            DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info {};
            info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
            info.header.size = sizeof info;
            info.header.adapterId = path.targetInfo.adapterId;
            info.header.id = path.targetInfo.id;
            if (::DisplayConfigGetDeviceInfo(&info.header) == ERROR_SUCCESS)
                mode.hdr = info.advancedColorEnabled && !info.wideColorEnforced;
        }
        // In thousandths of 80 nits (scRGB 1.0).
        DISPLAYCONFIG_SDR_WHITE_LEVEL white {};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof white;
        white.header.adapterId = path.targetInfo.adapterId;
        white.header.id = path.targetInfo.id;
        if (::DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0)
            mode.sdrWhiteNits = white.SDRWhiteLevel / 1000.0 * 80.0;
        break;
    }
    return mode;
}

float halfToFloat(std::uint16_t h)
{
    const std::uint32_t sign = (h & 0x8000u) << 16;
    std::uint32_t exp = (h >> 10) & 0x1Fu;
    std::uint32_t mant = h & 0x3FFu;
    std::uint32_t bits = 0;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else { // a subnormal half: normalised
            exp = 127 - 15 + 1;
            while (!(mant & 0x400u)) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f = 0;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

std::uint8_t encodeSrgb(float c)
{
    c = std::clamp(c, 0.0f, 1.0f);
    const float v = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<std::uint8_t>(std::lround(v * 255.0f));
}

QImage mapScRgbToSdr(const std::uint16_t* pixels, int width, int height, int rowBytes, double sdrWhiteNits)
{
    if (!pixels || width <= 0 || height <= 0)
        return {};
    const float scale = static_cast<float>(80.0 / std::max(sdrWhiteNits, 1.0));
    const auto row = [&](int y) {
        return reinterpret_cast<const std::uint16_t*>(reinterpret_cast<const std::uint8_t*>(pixels) + std::size_t(rowBytes) * y);
    };
    float brightest = 0;
    for (int y = 0; y < height; ++y) {
        const std::uint16_t* p = row(y);
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < 3; ++c)
                brightest = std::max(brightest, halfToFloat(p[x * 4 + c]) * scale);
        }
    }
    constexpr float knee = 0.8f;
    const bool roll = brightest > 1.0f + 1e-4f;
    // The roll-off's white point, in units of the room above the knee.
    const float wp = (brightest - knee) / (1.0f - knee);
    // The sRGB curve's pow per pixel is the cost: about 150 ms for
    // 2560x1600 in stage 0, paid only on HDR monitors.
    QImage out(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y) {
        const std::uint16_t* p = row(y);
        auto* o = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < width; ++x) {
            // Out-of-gamut (negative) components have no sRGB value: clipped.
            float r = std::max(0.0f, halfToFloat(p[x * 4]) * scale);
            float g = std::max(0.0f, halfToFloat(p[x * 4 + 1]) * scale);
            float b = std::max(0.0f, halfToFloat(p[x * 4 + 2]) * scale);
            const float m = std::max({r, g, b});
            if (roll && m > knee) {
                const float t = (m - knee) / (1.0f - knee);
                const float mapped = knee + (1.0f - knee) * (t * (1.0f + t / (wp * wp)) / (1.0f + t));
                const float f = mapped / m;
                r *= f;
                g *= f;
                b *= f;
            }
            o[x] = qRgb(encodeSrgb(r), encodeSrgb(g), encodeSrgb(b));
        }
    }
    return out;
}

} // namespace ws::capture
