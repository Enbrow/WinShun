#include "FolderViewHit.h"

#include <ole2.h> // before oleacc.h: WIN32_LEAN_AND_MEAN keeps it out of windows.h

#include <oleacc.h>
#include <wrl/client.h>

#include <cwchar>
#include <iterator>

using Microsoft::WRL::ComPtr;

namespace ws::folderview {

namespace {

bool hasClass(HWND window, const wchar_t* name)
{
    wchar_t className[64] {};
    ::GetClassNameW(window, className, static_cast<int>(std::size(className)));
    return std::wcscmp(className, name) == 0;
}

long roleOf(IAccessible* object, LONG child)
{
    VARIANT id {};
    id.vt = VT_I4;
    id.lVal = child;
    VARIANT role {};
    if (FAILED(object->get_accRole(id, &role)))
        return 0;
    const long value = role.vt == VT_I4 ? role.lVal : 0;
    ::VariantClear(&role);
    return value;
}

// The first child of a group: a group again for the whole group (its
// header), a button (collapse) for the header itself. Both are groups to
// accessibility, and their class names (UIGroupItem, UIGroupHeader) cannot be
// had: UI Automation will not take these objects (docs/pitfalls.md).
long firstChildRole(IAccessible* object)
{
    VARIANT child {};
    LONG got = 0;
    if (FAILED(::AccessibleChildren(object, 0, 1, &child, &got)) || got != 1)
        return 0;
    long role = 0;
    if (child.vt == VT_DISPATCH && child.pdispVal) {
        ComPtr<IAccessible> accessible;
        if (SUCCEEDED(child.pdispVal->QueryInterface(IID_PPV_ARGS(&accessible))))
            role = roleOf(accessible.Get(), CHILDID_SELF);
    } else if (child.vt == VT_I4) {
        role = roleOf(object, child.lVal);
    }
    ::VariantClear(&child);
    return role;
}

} // namespace

Hit classify(HWND window, POINT pt)
{
    Hit hit;
    if (!window)
        return hit;
    const HWND parent = ::GetParent(window);
    const HWND root = ::GetAncestor(window, GA_ROOT);
    if (!parent || !root || !hasClass(parent, L"SHELLDLL_DefView"))
        return hit;
    if (hasClass(window, L"DirectUIHWND") && (hasClass(root, L"CabinetWClass") || hasClass(root, L"ExploreWClass"))) {
        hit.surface = Surface::Explorer;
        // Windows 11 has a window per tab; the one clicked is in front.
        hit.tab = root;
        for (HWND up = parent; up && up != root; up = ::GetParent(up)) {
            if (hasClass(up, L"ShellTabWindowClass")) {
                hit.tab = up;
                break;
            }
        }
    } else if (hasClass(window, L"SysListView32") && (hasClass(root, L"Progman") || hasClass(root, L"WorkerW"))) {
        hit.surface = Surface::Desktop;
    } else {
        return hit;
    }
    hit.view = window;
    hit.root = root;
    hit.pt = pt;
    return hit;
}

Hit classifyWindow(HWND root)
{
    Hit hit;
    if (!root || ::GetAncestor(root, GA_ROOT) != root)
        return hit;
    if (hasClass(root, L"CabinetWClass") || hasClass(root, L"ExploreWClass")) {
        hit.surface = Surface::Explorer;
        // Windows 11 has a child per tab, the one on top first among them;
        // Windows 10 has none, the window itself is the tab.
        const HWND tab = ::FindWindowExW(root, nullptr, L"ShellTabWindowClass", nullptr);
        hit.tab = tab ? tab : root;
    } else if ((hasClass(root, L"Progman") || hasClass(root, L"WorkerW"))
        && ::FindWindowExW(root, nullptr, L"SHELLDLL_DefView", nullptr)) {
        hit.surface = Surface::Desktop;
    } else {
        return hit;
    }
    hit.root = root;
    return hit;
}

bool isOpusLister(HWND root)
{
    return root && ::GetAncestor(root, GA_ROOT) == root && hasClass(root, L"dopus.lister");
}

bool isOpusFileDisplay(HWND root, HWND focus)
{
    return isOpusLister(root) && focus && ::GetAncestor(focus, GA_ROOT) == root && hasClass(focus, L"dopus.filedisplay")
        && hasClass(::GetParent(focus), L"dopus.filedisplaycontainer");
}

bool isBlank(const Hit& hit)
{
    if (hit.surface == Surface::None || !::IsWindow(hit.view) || ::IsHungAppWindow(hit.root))
        return false;
    ComPtr<IAccessible> object;
    if (FAILED(::AccessibleObjectFromWindow(hit.view, OBJID_CLIENT, IID_PPV_ARGS(&object))) || !object)
        return false;
    // Down through what the point is on, to the innermost part. Measured on
    // Windows 11 (2026-10-13): the list itself in the gaps of a view without
    // groups and below its last row, and on the desktop's empty space; a
    // group where a view with groups has gaps; an item, a cell (text), a
    // header button or a scroll bar's parts elsewhere.
    for (int depth = 0; depth < 6; ++depth) {
        VARIANT at {};
        if (object->accHitTest(hit.pt.x, hit.pt.y, &at) != S_OK)
            return false;
        if (at.vt == VT_DISPATCH && at.pdispVal) {
            ComPtr<IAccessible> inner;
            const HRESULT hr = at.pdispVal->QueryInterface(IID_PPV_ARGS(&inner));
            ::VariantClear(&at);
            if (FAILED(hr) || !inner)
                return false;
            // A window of its own inside (the scroll bars' hosts): not blank.
            const long role = roleOf(inner.Get(), CHILDID_SELF);
            if (role == ROLE_SYSTEM_WINDOW || role == ROLE_SYSTEM_CLIENT || role == ROLE_SYSTEM_SCROLLBAR)
                return false;
            object = std::move(inner);
            continue;
        }
        if (at.vt != VT_I4)
            return false; // outside
        if (at.lVal != CHILDID_SELF)
            return false; // a simple child: an icon on the desktop
        switch (roleOf(object.Get(), CHILDID_SELF)) {
        case ROLE_SYSTEM_LIST:
            return true;
        case ROLE_SYSTEM_GROUPING:
            return firstChildRole(object.Get()) == ROLE_SYSTEM_GROUPING; // the group, not its header
        default:
            return false;
        }
    }
    return false;
}

} // namespace ws::folderview
