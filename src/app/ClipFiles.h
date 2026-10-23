#pragma once

#include "ClipStore.h"

namespace ws::clipfiles {

enum class Error { None, NotImages, WriteFailed };
struct Export {
    QStringList paths;
    Error error = Error::None;
};

// Copies every selected image outside the history, in selection order. A failed
// batch returns no paths: never silently drag/paste just part of the selection.
Export images(
    const ClipStore& store, const std::vector<const Clip*>& clips, const QString& folder, const QString& name);

// The file dialog's quoted list, including full paths for files in different
// folders. Not command-line escaping: backslashes, including UNC, stay literal.
QString fileNames(const QStringList& paths);

} // namespace ws::clipfiles
