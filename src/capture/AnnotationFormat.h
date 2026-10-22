#pragma once

#include "Annotation.h"

#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QRect>
#include <QString>

#include <optional>

// Projects: an annotated screenshot that can be edited again
// (docs/design-capture-roadmap.md, 3.12). A project (.wsedit) is a zip of
// doc.json (deflated) and base.png (stored: PNG is compressed already), the
// picture under the annotations, uncropped and unredacted; it stays on this
// computer and goes with its history entry.
namespace ws::capture::annot {

struct Project {
    Document doc;
    QImage base;
    // The base as the PNG it was stored in, when it was read from a project:
    // given back to writeProject(), it is stored again as it is (a long
    // picture takes seconds to encode, and the editor never changes it).
    QByteArray basePng;
};

// doc.json. Each style writes the fields its kind uses and any other that
// differs from Style's defaults, so reading it back gives the same document.
QJsonObject toJson(const Document& doc);
// Reads doc.json: another "format" is refused; an older version goes
// through each step up to this one; a newer one is refused with
// "这张图是新版 Win顺 存的，升级后才能编辑" (its picture still pastes, it is a
// normal PNG); fields this version does not know are ignored. Items that
// make no sense (no id, an unknown kind, too few points, an id taken) are
// left out, and the redactions are put first. Empty with `error` set when
// the document cannot be used.
std::optional<Document> fromJson(const QJsonObject& json, QString* error);
// The zip's bytes; empty when the picture cannot be encoded. With `basePng`
// (the same picture, already encoded) that is stored instead of encoding it.
QByteArray writeProject(const Document& doc, const QImage& base, const QByteArray& basePng = {});
// A project from its bytes, the base as Format_RGB32 of the canvas' size.
std::optional<Project> readProject(const QByteArray& data, QString* error);
// The overlay's document as a project: the base is the selection of the
// screen, the items move with it, and those wholly outside are left out.
// Numbers keep what they showed (a number left out would make the next ones
// count from it): where the count would now differ, the number is pinned.
Project projectFromOverlay(const Document& doc, const QImage& screen, const QRect& selection);
}
