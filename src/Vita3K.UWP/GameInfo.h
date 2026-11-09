// SPDX-License-Identifier: GPL-2.0-or-later
// What the internet knows about a Vita game, shown on its page in the library:
//   * Wikidata: developer, publisher, release date, genre and critics' review
//     scores (Metacritic, Famitsu and the like), for the PS Vita entry that
//     best matches the game's name.
//   * Wikipedia: the opening of its article, as the description.
//   * Vita3K's compatibility list (github.com/Vita3K/compatibility): how far
//     the game gets in Vita3K, found by its title ID.
// No accounts or API keys. Results are kept in the app's settings and
// refreshed after a few weeks, so a game page opens at once and works offline.
#pragma once
#include <ppltasks.h>
#include <string>
#include <utility>
#include <vector>

namespace vita3k_uwp {
struct GameInfo {
    std::wstring description;
    std::wstring developer;
    std::wstring publisher;
    std::wstring released; // "February 9, 2012"
    std::wstring genre;
    std::wstring wikiUrl;
    std::vector<std::pair<std::wstring, std::wstring>> scores; // who, score
    std::wstring compat;    // Vita3K status label ("Playable", "Ingame +", ...)
    std::wstring compatUrl; // its compatibility report
    long long fetchedAt = 0; // seconds since 1970
    bool Empty() const {
        return description.empty() && developer.empty() && publisher.empty() && released.empty() && scores.empty() &&
               compat.empty();
    }
};

// The title ID (four letters, five digits) in a folder or file name, or "".
std::wstring FindTitleId(const std::wstring& text);
// What was fetched before for this game (cacheKey: its title ID, else its
// name). stale: older than the refresh period (shown, but fetched again).
bool LoadGameInfo(const std::wstring& cacheKey, GameInfo& out, bool& stale);
void SaveGameInfo(const std::wstring& cacheKey, const GameInfo& info);
// Never throws: whatever could not be found stays empty.
concurrency::task<GameInfo> FetchGameInfo(std::wstring title, std::wstring titleId);
// One line for each Vita3K status, for the game page.
std::wstring DescribeCompat(const std::wstring& status);
}
