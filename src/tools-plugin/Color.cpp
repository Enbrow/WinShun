#include "Color.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tools::color {

namespace {

std::string lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return s;
}

std::string trim(const std::string& s)
{
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t'))
        --b;
    return s.substr(a, b - a);
}

bool isHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int hexValue(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

int clampChannel(double v) { return static_cast<int>(std::lround(std::clamp(v, 0.0, 255.0))); }

// A number with an optional % sign.
struct Number {
    double value = 0;
    bool percent = false;
    bool degrees = false;
};

// The arguments inside the parentheses of rgb(…) and the like: numbers
// separated by commas or spaces, an alpha after a comma or a slash.
std::optional<std::vector<Number>> arguments(const std::string& inside, bool* alphaBySlash)
{
    std::vector<Number> out;
    std::size_t i = 0;
    *alphaBySlash = false;
    while (i < inside.size()) {
        while (i < inside.size() && (inside[i] == ' ' || inside[i] == ',' || inside[i] == '/')) {
            if (inside[i] == '/')
                *alphaBySlash = true;
            ++i;
        }
        if (i >= inside.size())
            break;
        const char* start = inside.c_str() + i;
        char* end = nullptr;
        const double v = std::strtod(start, &end);
        if (end == start)
            return std::nullopt;
        i += static_cast<std::size_t>(end - start);
        Number n;
        n.value = v;
        if (i < inside.size() && inside[i] == '%') {
            n.percent = true;
            ++i;
        } else if (inside.compare(i, 3, "deg") == 0) {
            n.degrees = true;
            i += 3;
        }
        out.push_back(n);
    }
    return out;
}

double alphaOf(const std::vector<Number>& args, std::size_t index)
{
    if (index >= args.size())
        return 1;
    const double v = args[index].percent ? args[index].value / 100 : args[index].value;
    return std::clamp(v, 0.0, 1.0);
}

double hueOf(double h)
{
    h = std::fmod(h, 360.0);
    if (h < 0)
        h += 360;
    return h;
}

} // namespace

std::optional<Color> parse(const std::string& given)
{
    std::string t = trim(given);
    if (!t.empty() && t.back() == ';')
        t = trim(t.substr(0, t.size() - 1));
    if (t.empty() || t.size() > 48)
        return std::nullopt;
    if (t[0] == '#') {
        const std::string digits = t.substr(1);
        for (const char c : digits) {
            if (!isHex(c))
                return std::nullopt;
        }
        std::string full;
        if (digits.size() == 3 || digits.size() == 4) {
            for (const char c : digits)
                full += std::string(2, c);
        } else if (digits.size() == 6 || digits.size() == 8)
            full = digits;
        else
            return std::nullopt;
        const auto channel = [&](std::size_t i) { return hexValue(full[i * 2]) * 16 + hexValue(full[i * 2 + 1]); };
        Color c;
        c.r = channel(0);
        c.g = channel(1);
        c.b = channel(2);
        c.a = full.size() == 8 ? channel(3) / 255.0 : 1.0;
        return c;
    }
    const std::string s = lower(t);
    const std::size_t open = s.find('(');
    if (open == std::string::npos || s.back() != ')')
        return std::nullopt;
    const std::string fn = trim(s.substr(0, open));
    const std::string inside = s.substr(open + 1, s.size() - open - 2);
    bool slash = false;
    const auto args = arguments(inside, &slash);
    if (!args || args->size() < 3 || args->size() > 4)
        return std::nullopt;
    (void)slash;
    if (fn == "rgb" || fn == "rgba") {
        int channels[3];
        for (int i = 0; i < 3; ++i) {
            const Number& n = (*args)[static_cast<std::size_t>(i)];
            const double scaled = n.percent ? n.value * 255 / 100 : n.value;
            if (scaled < 0 || scaled > 255)
                return std::nullopt;
            channels[i] = clampChannel(scaled);
        }
        Color c;
        c.r = channels[0];
        c.g = channels[1];
        c.b = channels[2];
        c.a = alphaOf(*args, 3);
        return c;
    }
    if (fn == "hsl" || fn == "hsla" || fn == "hsv" || fn == "hsva" || fn == "hsb" || fn == "hsba") {
        const double h = hueOf((*args)[0].value);
        const Number& sn = (*args)[1];
        const Number& ln = (*args)[2];
        const double sat = sn.percent || sn.value > 1 ? sn.value / 100 : sn.value;
        const double light = ln.percent || ln.value > 1 ? ln.value / 100 : ln.value;
        if (sat < 0 || sat > 1 || light < 0 || light > 1)
            return std::nullopt;
        return fn[2] == 'l' ? fromHsl(h, sat, light, alphaOf(*args, 3)) : fromHsv(h, sat, light, alphaOf(*args, 3));
    }
    return std::nullopt;
}

void toHsl(const Color& color, double& h, double& s, double& l)
{
    const double r = color.r / 255.0;
    const double g = color.g / 255.0;
    const double b = color.b / 255.0;
    const double max = std::max({r, g, b});
    const double min = std::min({r, g, b});
    l = (max + min) / 2;
    const double d = max - min;
    if (d < 1e-9) {
        h = 0;
        s = 0;
        return;
    }
    s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
    if (max == r)
        h = (g - b) / d + (g < b ? 6 : 0);
    else if (max == g)
        h = (b - r) / d + 2;
    else
        h = (r - g) / d + 4;
    h *= 60;
}

void toHsv(const Color& color, double& h, double& s, double& v)
{
    const double r = color.r / 255.0;
    const double g = color.g / 255.0;
    const double b = color.b / 255.0;
    const double max = std::max({r, g, b});
    const double min = std::min({r, g, b});
    v = max;
    const double d = max - min;
    s = max < 1e-9 ? 0 : d / max;
    if (d < 1e-9) {
        h = 0;
        return;
    }
    if (max == r)
        h = (g - b) / d + (g < b ? 6 : 0);
    else if (max == g)
        h = (b - r) / d + 2;
    else
        h = (r - g) / d + 4;
    h *= 60;
}

Color fromHsl(double h, double s, double l, double a)
{
    const double c = (1 - std::fabs(2 * l - 1)) * s;
    const double hp = hueOf(h) / 60;
    const double x = c * (1 - std::fabs(std::fmod(hp, 2) - 1));
    double r = 0, g = 0, b = 0;
    if (hp < 1) { r = c; g = x; }
    else if (hp < 2) { r = x; g = c; }
    else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; }
    else if (hp < 5) { r = x; b = c; }
    else { r = c; b = x; }
    const double m = l - c / 2;
    Color out;
    out.r = clampChannel((r + m) * 255);
    out.g = clampChannel((g + m) * 255);
    out.b = clampChannel((b + m) * 255);
    out.a = std::clamp(a, 0.0, 1.0);
    return out;
}

Color fromHsv(double h, double s, double v, double a)
{
    const double c = v * s;
    const double hp = hueOf(h) / 60;
    const double x = c * (1 - std::fabs(std::fmod(hp, 2) - 1));
    double r = 0, g = 0, b = 0;
    if (hp < 1) { r = c; g = x; }
    else if (hp < 2) { r = x; g = c; }
    else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; }
    else if (hp < 5) { r = x; b = c; }
    else { r = c; b = x; }
    const double m = v - c;
    Color out;
    out.r = clampChannel((r + m) * 255);
    out.g = clampChannel((g + m) * 255);
    out.b = clampChannel((b + m) * 255);
    out.a = std::clamp(a, 0.0, 1.0);
    return out;
}

std::string hex(const Color& color, bool alpha)
{
    char buffer[16];
    if (alpha && color.a < 1)
        std::snprintf(buffer, sizeof buffer, "#%02X%02X%02X%02X", color.r, color.g, color.b, clampChannel(color.a * 255));
    else
        std::snprintf(buffer, sizeof buffer, "#%02X%02X%02X", color.r, color.g, color.b);
    return buffer;
}

namespace {
std::string alphaText(double a)
{
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "%.2g", a);
    return buffer;
}
std::string percent(double v)
{
    return std::to_string(static_cast<int>(std::lround(v * 100)));
}
} // namespace

std::vector<Notation> notations(const Color& color)
{
    const bool alpha = color.a < 1;
    std::vector<Notation> list {{"HEX", hex(color)}};
    if (alpha) {
        char buffer[16];
        std::snprintf(buffer, sizeof buffer, "#%02X%02X%02X%02X", clampChannel(color.a * 255), color.r, color.g, color.b);
        list.push_back({"ARGB", buffer});
    }
    const std::string channels = std::to_string(color.r) + ", " + std::to_string(color.g) + ", " + std::to_string(color.b);
    list.push_back({"RGB", alpha ? "rgba(" + channels + ", " + alphaText(color.a) + ")" : "rgb(" + channels + ")"});
    double h, s, l;
    toHsl(color, h, s, l);
    const std::string hsl = std::to_string(static_cast<int>(std::lround(h))) + ", " + percent(s) + "%, " + percent(l) + "%";
    list.push_back({"HSL", alpha ? "hsla(" + hsl + ", " + alphaText(color.a) + ")" : "hsl(" + hsl + ")"});
    double hv, sv, v;
    toHsv(color, hv, sv, v);
    const std::string hsv = std::to_string(static_cast<int>(std::lround(hv))) + ", " + percent(sv) + "%, " + percent(v) + "%";
    list.push_back({"HSV", "hsv(" + hsv + ")"});
    return list;
}

} // namespace tools::color
