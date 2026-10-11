#pragma once

// Colour values as text for winshun.tools (the launcher's ColorText, done
// again without Qt): #rgb, #rgba, #rrggbb, #rrggbbaa (alpha last, as in
// CSS), rgb() / rgba() with numbers or percentages, hsl() / hsla(), hsv() /
// hsb(); commas or spaces, alpha after a comma or a slash, a trailing
// semicolon as copied from a style sheet.

#include <optional>
#include <string>
#include <vector>

namespace tools::color {

struct Color {
    int r = 0; // 0–255
    int g = 0;
    int b = 0;
    double a = 1; // 0–1
};

std::optional<Color> parse(const std::string& text);

struct Notation {
    std::string label; // "HEX", "RGB", "HSL", "HSV", "ARGB"
    std::string value;
};
// The colour in each notation (alpha included when it has one); a
// translucent colour also as #AARRGGBB, alpha first, as Qt, Android and XAML
// write it.
std::vector<Notation> notations(const Color& color);

std::string hex(const Color& color, bool alpha = true); // "#FF8800", "#FF880080"
void toHsl(const Color& color, double& h, double& s, double& l); // h 0–360, s and l 0–1
void toHsv(const Color& color, double& h, double& s, double& v);
Color fromHsl(double h, double s, double l, double a = 1);
Color fromHsv(double h, double s, double v, double a = 1);

} // namespace tools::color
