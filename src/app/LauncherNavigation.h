#pragma once

#include "SearchTypes.h"

namespace ws::navigation {

enum class TabAction { SwitchScope, Complete, BrowseFolder, None };

inline bool canBrowseFolder(const SearchResult& result) noexcept
{
    return result.isDir && !result.isApp() && !result.offline && !result.missing;
}

// The action and the footer use the same policy. Favorites only change
// Tab when explicitly enabled; keyword and web completion stay independent.
inline TabAction tabAction(
    Scope scope, bool keywordMode, bool enterFavoriteFolder, const SearchResult* row, bool hasCompletion) noexcept
{
    if (!keywordMode && scope == Scope::All && enterFavoriteFolder && row && row->favorite && canBrowseFolder(*row))
        return TabAction::BrowseFolder;
    if (hasCompletion)
        return TabAction::Complete;
    return keywordMode ? TabAction::None : TabAction::SwitchScope;
}

} // namespace ws::navigation
