// SPDX-License-Identifier: GPL-2.0-or-later
#include "GameInfo.h"
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <memory>
#include <set>

using namespace concurrency;
using namespace Windows::Data::Json;
using namespace Windows::Foundation;
using namespace Windows::Storage;
using namespace Windows::Web::Http;

namespace vita3k_uwp {
namespace {
constexpr long long kRefreshSeconds = 21LL * 24 * 3600;
constexpr const wchar_t* kVitaPlatform = L"Q188808"; // PlayStation Vita on Wikidata

long long Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

HttpClient^ MakeClient() {
    auto client = ref new HttpClient();
    // Wikimedia and GitHub both ask for a User-Agent that names the app.
    client->DefaultRequestHeaders->UserAgent->TryParseAdd(L"Vita3K-UWP/1.0 (Xbox; +https://github.com/oledz-i/Vita3K-UWP)");
    client->DefaultRequestHeaders->Accept->TryParseAdd(L"application/json");
    return client;
}

std::wstring Str(Platform::String^ s) { return s ? std::wstring(s->Data()) : std::wstring(); }

std::wstring JsonString(JsonObject^ o, const wchar_t* key) {
    if (!o || !o->HasKey(ref new Platform::String(key))) return {};
    auto v = o->GetNamedValue(ref new Platform::String(key));
    return v->ValueType == JsonValueType::String ? Str(v->GetString()) : std::wstring();
}

// A SPARQL binding's value ("" when the row has none).
std::wstring Binding(JsonObject^ row, const wchar_t* name) {
    if (!row->HasKey(ref new Platform::String(name))) return {};
    return JsonString(row->GetNamedObject(ref new Platform::String(name)), L"value");
}

void AddUnique(std::vector<std::wstring>& list, const std::wstring& value) {
    if (value.empty() || value.size() > 80) return;
    // Unlabelled items come back as their Q-number URL: no use to a player.
    if (value.rfind(L"http", 0) == 0 || (value[0] == L'Q' && value.size() > 1 && iswdigit(value[1]))) return;
    if (std::find(list.begin(), list.end(), value) == list.end()) list.push_back(value);
}

std::wstring Join(const std::vector<std::wstring>& list, size_t most) {
    std::wstring out;
    for (size_t i = 0; i < list.size() && i < most; ++i) {
        if (i) out += L", ";
        out += list[i];
    }
    return out;
}

std::wstring PrettyDate(const std::wstring& iso) {
    // "2012-02-09T00:00:00Z"
    static const wchar_t* months[] = { L"January", L"February", L"March", L"April", L"May", L"June", L"July",
        L"August", L"September", L"October", L"November", L"December" };
    if (iso.size() < 10) return iso;
    const int year = _wtoi(iso.substr(0, 4).c_str());
    const int month = _wtoi(iso.substr(5, 2).c_str());
    const int day = _wtoi(iso.substr(8, 2).c_str());
    if (year < 2000 || month < 1 || month > 12) return iso.substr(0, 4);
    if (day < 1) return std::wstring(months[month - 1]) + L" " + std::to_wstring(year);
    return std::wstring(months[month - 1]) + L" " + std::to_wstring(day) + L", " + std::to_wstring(year);
}

// The names to look the game up by: as the game calls itself, without the
// trademark signs and line breaks SFO titles carry, then just the part before
// a subtitle ("Danganronpa: Trigger Happy Havoc" -> "Danganronpa").
std::vector<std::wstring> SearchNames(const std::wstring& title) {
    std::wstring clean;
    for (wchar_t c : title) {
        if (c == L'\u2122' || c == L'\u00AE' || c == L'\u00A9') continue;
        clean.push_back(c == L'\n' || c == L'\r' || c == L'\t' ? L' ' : c);
    }
    std::wstring squeezed;
    for (wchar_t c : clean) {
        if (c == L' ' && (squeezed.empty() || squeezed.back() == L' ')) continue;
        squeezed.push_back(c);
    }
    while (!squeezed.empty() && squeezed.back() == L' ') squeezed.pop_back();
    std::vector<std::wstring> names;
    if (!squeezed.empty()) names.push_back(squeezed);
    for (const wchar_t* cut : { L":", L" - ", L" \u2013 " }) {
        const auto at = squeezed.find(cut);
        if (at != std::wstring::npos && at > 2) {
            auto head = squeezed.substr(0, at);
            while (!head.empty() && head.back() == L' ') head.pop_back();
            if (std::find(names.begin(), names.end(), head) == names.end()) names.push_back(head);
        }
    }
    return names;
}

std::wstring SparqlLiteral(const std::wstring& text) {
    std::wstring out;
    for (wchar_t c : text) {
        if (c == L'"' || c == L'\\') out.push_back(L'\\');
        out.push_back(c);
    }
    return out;
}

std::wstring WikidataQuery(const std::wstring& name) {
    // The search service ranks items by how well their name matches; only
    // PlayStation Vita games are kept, and the best-ranked one is used.
    return L"SELECT ?item ?itemDescription ?date ?devLabel ?pubLabel ?genreLabel ?score ?byLabel ?article WHERE {"
           L" SERVICE wikibase:mwapi { bd:serviceParam wikibase:api \"EntitySearch\"; wikibase:endpoint \"www.wikidata.org\";"
           L" mwapi:search \"" + SparqlLiteral(name) + L"\"; mwapi:language \"en\"."
           L" ?item wikibase:apiOutputItem mwapi:item. ?num wikibase:apiOrdinal true. }"
           L" ?item wdt:P400 wd:" + std::wstring(kVitaPlatform) + L"."
           L" OPTIONAL { ?item wdt:P577 ?date. }"
           L" OPTIONAL { ?item wdt:P178 ?dev. }"
           L" OPTIONAL { ?item wdt:P123 ?pub. }"
           L" OPTIONAL { ?item wdt:P136 ?genre. }"
           L" OPTIONAL { ?item p:P444 ?st. ?st ps:P444 ?score. ?st pq:P447 ?by. }"
           L" OPTIONAL { ?article schema:about ?item; schema:isPartOf <https://en.wikipedia.org/>. }"
           L" SERVICE wikibase:label { bd:serviceParam wikibase:language \"en\". }"
           L" } ORDER BY ?num LIMIT 200";
}

bool ParseWikidata(Platform::String^ body, GameInfo& info) {
    auto root = JsonObject::Parse(body);
    auto rows = root->GetNamedObject(L"results")->GetNamedArray(L"bindings");
    if (rows->Size == 0) return false;
    const auto first = Binding(rows->GetObjectAt(0), L"item");
    std::vector<std::wstring> devs, pubs, genres;
    std::wstring date;
    std::set<std::wstring> seenScores;
    for (unsigned i = 0; i < rows->Size; ++i) {
        auto row = rows->GetObjectAt(i);
        if (Binding(row, L"item") != first) continue;
        AddUnique(devs, Binding(row, L"devLabel"));
        AddUnique(pubs, Binding(row, L"pubLabel"));
        AddUnique(genres, Binding(row, L"genreLabel"));
        const auto d = Binding(row, L"date");
        if (!d.empty() && (date.empty() || d < date)) date = d; // the first release
        const auto score = Binding(row, L"score");
        const auto by = Binding(row, L"byLabel");
        if (!score.empty() && !by.empty() && by[0] != L'Q' && seenScores.insert(by).second && info.scores.size() < 6)
            info.scores.emplace_back(by, score);
        if (info.wikiUrl.empty()) info.wikiUrl = Binding(row, L"article");
        if (info.description.empty()) info.description = Binding(row, L"itemDescription");
    }
    info.developer = Join(devs, 3);
    info.publisher = Join(pubs, 3);
    info.genre = Join(genres, 3);
    if (!date.empty()) info.released = PrettyDate(date);
    // Metacritic first: the score most people know.
    std::stable_sort(info.scores.begin(), info.scores.end(), [](const auto& a, const auto& b) {
        return (a.first.find(L"Metacritic") != std::wstring::npos) > (b.first.find(L"Metacritic") != std::wstring::npos);
    });
    return true;
}

task<void> QueryWikidata(HttpClient^ client, std::shared_ptr<std::vector<std::wstring>> names, size_t at,
                         std::shared_ptr<GameInfo> info) {
    if (at >= names->size()) return task_from_result();
    auto uri = ref new Uri(ref new Platform::String((L"https://query.wikidata.org/sparql?format=json&query=" +
        Str(Uri::EscapeComponent(ref new Platform::String(WikidataQuery((*names)[at]).c_str())))).c_str()));
    return create_task(client->GetStringAsync(uri)).then([client, names, at, info](task<Platform::String^> t) {
        bool found = false;
        try { found = ParseWikidata(t.get(), *info); } catch (...) {}
        if (found) return task_from_result();
        return QueryWikidata(client, names, at + 1, info);
    });
}

task<void> QueryWikipedia(HttpClient^ client, std::shared_ptr<GameInfo> info) {
    // https://en.wikipedia.org/wiki/<Title> -> the article's summary
    const auto at = info->wikiUrl.find(L"/wiki/");
    if (at == std::wstring::npos) return task_from_result();
    const auto page = info->wikiUrl.substr(at + 6);
    auto uri = ref new Uri(ref new Platform::String((L"https://en.wikipedia.org/api/rest_v1/page/summary/" + page).c_str()));
    return create_task(client->GetStringAsync(uri)).then([info](task<Platform::String^> t) {
        try {
            const auto extract = JsonString(JsonObject::Parse(t.get()), L"extract");
            if (extract.size() > 40) info->description = extract;
        } catch (...) {}
    });
}

task<void> QueryCompat(HttpClient^ client, std::wstring titleId, std::shared_ptr<GameInfo> info) {
    if (titleId.empty()) return task_from_result();
    auto uri = ref new Uri(ref new Platform::String(
        (L"https://api.github.com/search/issues?per_page=5&q=" + titleId + L"+repo:Vita3K/compatibility+is:issue").c_str()));
    return create_task(client->GetStringAsync(uri)).then([titleId, info](task<Platform::String^> t) {
        static const wchar_t* known[] = { L"Playable", L"Ingame +", L"Ingame -", L"Ingame", L"Menu", L"Intro",
            L"Bootable", L"Nothing" };
        try {
            auto items = JsonObject::Parse(t.get())->GetNamedArray(L"items");
            for (unsigned i = 0; i < items->Size; ++i) {
                auto item = items->GetObjectAt(i);
                if (JsonString(item, L"title").find(titleId) == std::wstring::npos) continue;
                auto labels = item->GetNamedArray(L"labels");
                for (const wchar_t* status : known) {
                    for (unsigned l = 0; l < labels->Size && info->compat.empty(); ++l)
                        if (JsonString(labels->GetObjectAt(l), L"name") == status) info->compat = status;
                }
                info->compatUrl = JsonString(item, L"html_url");
                break;
            }
        } catch (...) {}
    });
}

std::wstring Field(JsonObject^ o, const wchar_t* key) { return JsonString(o, key); }
}

std::wstring FindTitleId(const std::wstring& path) {
    for (std::size_t i = 0; i + 9 <= path.size(); ++i) {
        bool match = true;
        for (std::size_t k = 0; k < 9 && match; ++k) {
            const wchar_t c = path[i + k];
            match = k < 4 ? (c >= L'A' && c <= L'Z') : (c >= L'0' && c <= L'9');
        }
        const bool edgeBefore = i == 0 || !iswalnum(path[i - 1]);
        const bool edgeAfter = i + 9 == path.size() || !iswalnum(path[i + 9]);
        if (match && edgeBefore && edgeAfter) return path.substr(i, 9);
    }
    return {};
}

bool LoadGameInfo(const std::wstring& cacheKey, GameInfo& out, bool& stale) {
    stale = false;
    try {
        auto values = ApplicationData::Current->LocalSettings->Values;
        auto key = ref new Platform::String((L"info." + cacheKey).c_str());
        if (!values->HasKey(key)) return false;
        auto o = JsonObject::Parse(safe_cast<Platform::String^>(values->Lookup(key)));
        out = GameInfo{};
        out.description = Field(o, L"description");
        out.developer = Field(o, L"developer");
        out.publisher = Field(o, L"publisher");
        out.released = Field(o, L"released");
        out.genre = Field(o, L"genre");
        out.wikiUrl = Field(o, L"wiki");
        out.compat = Field(o, L"compat");
        out.compatUrl = Field(o, L"compatUrl");
        if (o->HasKey(L"scores")) {
            auto scores = o->GetNamedArray(L"scores");
            for (unsigned i = 0; i < scores->Size; ++i) {
                auto s = scores->GetObjectAt(i);
                out.scores.emplace_back(Field(s, L"by"), Field(s, L"score"));
            }
        }
        out.fetchedAt = o->HasKey(L"at") ? static_cast<long long>(o->GetNamedNumber(L"at")) : 0;
        stale = Now() - out.fetchedAt > kRefreshSeconds;
        return true;
    } catch (...) {
        return false;
    }
}

void SaveGameInfo(const std::wstring& cacheKey, const GameInfo& info) {
    try {
        auto o = ref new JsonObject();
        auto put = [&](const wchar_t* key, std::wstring value, size_t most) {
            if (value.size() > most) value = value.substr(0, most - 1) + L"\u2026";
            o->SetNamedValue(ref new Platform::String(key), JsonValue::CreateStringValue(ref new Platform::String(value.c_str())));
        };
        // A setting holds at most 8 KB: the description is the part kept short.
        put(L"description", info.description, 1400);
        put(L"developer", info.developer, 200);
        put(L"publisher", info.publisher, 200);
        put(L"released", info.released, 60);
        put(L"genre", info.genre, 200);
        put(L"wiki", info.wikiUrl, 300);
        put(L"compat", info.compat, 40);
        put(L"compatUrl", info.compatUrl, 300);
        auto scores = ref new JsonArray();
        for (const auto& s : info.scores) {
            auto item = ref new JsonObject();
            item->SetNamedValue(L"by", JsonValue::CreateStringValue(ref new Platform::String(s.first.substr(0, 60).c_str())));
            item->SetNamedValue(L"score", JsonValue::CreateStringValue(ref new Platform::String(s.second.substr(0, 30).c_str())));
            scores->Append(item);
        }
        o->SetNamedValue(L"scores", scores);
        o->SetNamedValue(L"at", JsonValue::CreateNumberValue(static_cast<double>(info.fetchedAt)));
        ApplicationData::Current->LocalSettings->Values->Insert(ref new Platform::String((L"info." + cacheKey).c_str()),
                                                                 o->Stringify());
    } catch (...) {
    }
}

task<GameInfo> FetchGameInfo(std::wstring title, std::wstring titleId) {
    auto client = MakeClient();
    auto info = std::make_shared<GameInfo>();
    auto names = std::make_shared<std::vector<std::wstring>>(SearchNames(title));
    return QueryWikidata(client, names, 0, info)
        .then([client, info](task<void> t) {
            try { t.get(); } catch (...) {}
            return QueryWikipedia(client, info);
        })
        .then([client, info, titleId](task<void> t) {
            try { t.get(); } catch (...) {}
            return QueryCompat(client, titleId, info);
        })
        .then([info](task<void> t) {
            try { t.get(); } catch (...) {}
            info->fetchedAt = Now();
            return *info;
        });
}

std::wstring DescribeCompat(const std::wstring& status) {
    if (status == L"Playable") return L"Can be played from start to finish.";
    if (status == L"Ingame +") return L"Gets in game and plays well, with some issues.";
    if (status == L"Ingame -" || status == L"Ingame") return L"Gets in game, but with major issues.";
    if (status == L"Menu") return L"Reaches its menus, but not into the game.";
    if (status == L"Intro") return L"Shows its intro, then stops.";
    if (status == L"Bootable") return L"Starts, but shows little or nothing.";
    if (status == L"Nothing") return L"Does not start.";
    return L"";
}
}
