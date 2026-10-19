#include "Hdr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace ws::capture {

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
