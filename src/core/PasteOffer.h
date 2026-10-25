#pragma once

#include "PasteFile.h"

#include <windows.h>

namespace ws::pastefile {

// What the clipboard offers now (PasteFile.h's Offer), by its formats alone:
// IsClipboardFormatAvailable opens nothing and asks no program (Excel makes
// its formats only when they are read), so the keyboard hook can ask at a key
// (platform/TypeToSearch); the stub asks it too (src/stub/PasteAsFile.cpp).
inline Offer clipboardOffer()
{
    static const UINT idList = ::RegisterClipboardFormatW(L"Shell IDList Array");
    static const UINT descriptor = ::RegisterClipboardFormatW(L"FileGroupDescriptorW");
    static const UINT descriptorAnsi = ::RegisterClipboardFormatW(L"FileGroupDescriptor");
    static const UINT png = ::RegisterClipboardFormatW(L"PNG");
    const auto has = [](UINT format) { return format && ::IsClipboardFormatAvailable(format); };
    return {
        .files = has(CF_HDROP) || has(idList),
        .virtualFiles = has(descriptor) || has(descriptorAnsi),
        .text = has(CF_UNICODETEXT),
        .image = has(png) || has(CF_DIB) || has(CF_DIBV5) || has(CF_BITMAP),
    };
}

} // namespace ws::pastefile
