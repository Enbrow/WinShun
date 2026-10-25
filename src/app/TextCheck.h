#pragma once

#include <QFont>
#include <QGlyphRun>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

// Whether text is drawn as the characters it holds (GitHub #70: after a long
// run, the highlighted characters of the result rows came out as others, 色
// as 柔: their glyph numbers were looked up in SimSun and drawn with Alibaba
// PuHuiTi). Each glyph of a laid-out text against the glyph its run's own
// font gives that character; nothing about the user's text is involved.
namespace ws::textcheck {

// What a check lays out: Chinese, Latin and digits that no default
// OpenType substitution (ccmp, locl, calt, liga...) touches in PuHuiTi,
// Microsoft YaHei UI or SimSun, so each one's glyph is its character map's:
// one glyph per character, left to right.
QString sample();

// `text` laid out in `font` as a result row lays out its highlights
// (Text.StyledText, <b>): [from, from + length) bold. With glyph positions.
QList<QGlyphRun> layOut(const QString& text, const QFont& font, qsizetype from, qsizetype length);

// The glyphs of `runs` (laid out from `text`: one line, one glyph per
// character, left to right, as sample()) that are not what their run's font
// maps the character to, as {char, code, glyph, expected, font, style,
// weight, pixel_size}; `expected` 0 when that font has no such character.
// Glyphs and characters not one to one: a single {note}. Empty: every
// character is drawn as itself.
QJsonArray mismatches(const QString& text, const QList<QGlyphRun>& runs);

// The sample in `font` three ways (a row's highlight, all bold as titles,
// all plain): {ok, font, cases: [{case, ok, runs, mismatches}]}.
QJsonObject check(const QFont& font);

} // namespace ws::textcheck
