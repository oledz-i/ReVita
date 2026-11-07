// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "D3D12Host.h"
#include "GameFormats.h"
#include "VitaLibretroHost.h"
#include "LivingBackdrop.h"
#include "GameInfo.h"
#include <set>
#include <map>
#include "CueAudio.h"
#include <functional>
#include <collection.h>
#include <ppltasks.h>
#include <string>
#include <vector>
#include <cstdint>
#include <atomic>
#include <mutex>

namespace Vita3KUWP {
struct LibraryGame {
    Platform::String^ name;
    Platform::String^ key;
    Platform::String^ sourceType;
    Windows::Storage::StorageFolder^ directory;
    Windows::Storage::StorageFile^ file;
    Windows::Storage::StorageFile^ icon;
    vita3k_uwp::GameFormat format = vita3k_uwp::GameFormat::Unknown;
    Platform::String^ validation = L"Awaiting source validation";
};

// The PS3-style home (XMB): categories across, their items down.
struct XmbEntry {
    std::wstring title;
    std::wstring subtitle;
    std::wstring glyph;      // Segoe MDL2 glyph, when there is no picture
    int game = -1;           // a game of the library (its icon is the picture)
    std::function<void()> action;
    std::function<void()> options;
};
struct XmbCategory {
    std::wstring name;
    std::wstring glyph;
    std::vector<XmbEntry> items;
    int initial = 0;
};

public ref class CatalogShell sealed : Windows::UI::Xaml::Controls::UserControl {
public:
    CatalogShell();

private:
    std::vector<LibraryGame> games_;
    bool booting_ = false;
    // The game screen: a full-window layer over the menus while a game runs.
    Windows::UI::Xaml::Controls::Grid^ gameLayer_ = nullptr;
    Windows::UI::Xaml::Controls::Grid^ loadingLayer_ = nullptr;
    Windows::UI::Xaml::Controls::TextBlock^ loadingStage_ = nullptr;
    Windows::UI::Xaml::Controls::TextBlock^ loadingHint_ = nullptr;
    Windows::UI::Xaml::Controls::ProgressBar^ loadingBar_ = nullptr;
    Windows::UI::Xaml::Controls::ScrollViewer^ contentScroll_ = nullptr;
    Windows::UI::Xaml::Controls::Border^ statusBar_ = nullptr;
    Windows::UI::Xaml::DispatcherTimer^ gameTimer_ = nullptr;
    bool inGame_ = false;
    bool gameStarted_ = false;
    bool loadFailed_ = false;
    bool loadingHidden_ = false;
    unsigned long long gameRunningSince_ = 0;
    std::atomic<bool> pictureSeen_{false};
    std::atomic<unsigned long long> framesShown_{0};
    std::atomic<unsigned> lastLevel_{0};
    std::atomic<unsigned> lastLit_{0};
    std::mutex presentErrorMutex_;
    std::wstring presentError_;
    Windows::UI::Xaml::Controls::TextBlock^ statsLabel_ = nullptr;
    // The performance overlay (quick menu: Off, FPS, Detailed), top left.
    Windows::UI::Xaml::Controls::Border^ perfOverlay_ = nullptr;
    Windows::UI::Xaml::Controls::TextBlock^ perfText_ = nullptr;
    unsigned long long perfLastTick_ = 0;
    unsigned long long perfLastShown_ = 0;
    unsigned long long perfLastGuest_ = 0;
    void ApplyPerfMode();
    void UpdatePerfOverlay();
    // The quick menu (View + Menu): the game paused under it.
    Windows::UI::Xaml::Controls::Grid^ quickMenu_ = nullptr;
    bool quickMenuOpen_ = false;
    Platform::String^ gameTitle_ = nullptr;
    Windows::UI::Xaml::Controls::Grid^ root_;
    Windows::UI::Xaml::Controls::Border^ navigation_;
    Windows::UI::Xaml::Controls::StackPanel^ content_;
    Windows::UI::Xaml::Controls::MediaElement^ musicPlayer_;
    Windows::Storage::Streams::IRandomAccessStream^ customMusicStream_ = nullptr;
    Windows::UI::Xaml::Controls::TextBlock^ musicStatus_ = nullptr;
    Platform::String^ playbackStatus_ = L"Not started";
    // One slot per cue in vita3k_uwp::kInterfaceCueCount.
    Windows::UI::Xaml::Controls::MediaElement^ cuePlayers_[6] = {};
    CueAudio cueAudio_;
    Windows::Storage::Streams::InMemoryRandomAccessStream^ cueStreams_[6] = {};
    bool cueReady_[6] = {};
    bool uiSounds_ = true;
    double uiSoundVolume_ = 0.40;
    Windows::UI::Xaml::Controls::TextBlock^ status_;
    // The moving scene behind the menus, and the clock on Home.
    vita3k_uwp::LivingBackdrop backdrop_;
    vita3k_uwp::XmbRibbons xmbRibbons_;
    Windows::UI::Xaml::Controls::Grid^ backdropHost_ = nullptr;
    Windows::UI::Xaml::Controls::TextBlock^ clock_ = nullptr;
    Windows::UI::Xaml::Controls::TextBlock^ greeting_ = nullptr;
    Windows::UI::Xaml::DispatcherTimer^ clockTimer_ = nullptr;
    void UpdateClock();
    void ApplySurfaceColors();
    Windows::UI::Xaml::Controls::TextBlock^ folderLabel_;
    Windows::UI::Xaml::Controls::TextBlock^ firmwareStatus_[3] = {};
    Windows::UI::Xaml::Controls::Border^ firmwareDot_[3] = {};
    Windows::UI::Xaml::Controls::ProgressBar^ firmwareProgress_ = nullptr;
    Windows::UI::Xaml::Controls::TextBlock^ firmwareMessage_ = nullptr;
    Windows::UI::Xaml::Controls::TextBlock^ dataLabel_ = nullptr;
    bool firmwareBusy_ = false;
    Windows::UI::Xaml::Controls::TextBlock^ rendererResult_;
    Windows::UI::Xaml::Controls::TextBlock^ coreResult_;
    Windows::UI::Xaml::Controls::TextBlock^ libretroResult_;
    Windows::UI::Xaml::Controls::TextBlock^ memoryResult_;
    Windows::UI::Xaml::Controls::SwapChainPanel^ renderSurface_;
    Windows::UI::Xaml::Controls::TextBox^ searchBox_;
    Windows::UI::Xaml::Controls::StackPanel^ libraryResults_;
    Windows::UI::Xaml::Controls::TextBlock^ libraryFolder_;
    void RefreshLibraryItems();
    // A game's page: what the internet knows about it (GameInfo.h).
    void AppendGameInfo(size_t index);
    void FetchGameInfoFor(size_t index, bool force);
    std::wstring GameInfoKey(size_t index) const;
    std::wstring GameTitleId(size_t index) const;
    std::set<std::wstring> infoFetching_;
    D3D12Host renderer_;
    VitaLibretroHost libretro_;
    int page_ = 0;
    int shownPage_ = -1;
    int selectedGame_ = -1;
    int theme_ = 0;
    int track_ = 1;
    double musicVolume_ = 0.22;
    bool artworkScanBusy_ = false;
    bool bootstrapped_ = false;
    unsigned libraryRevision_ = 0;
    Platform::String^ search_ = L"";
    // The dashboard (cinematic layout): a top bar instead of the old rail,
    // and the focused game's picture across the whole screen behind Home and
    // a game's page (the "spotlight").
    Windows::UI::Xaml::Controls::Grid^ spotlight_ = nullptr;
    Windows::UI::Xaml::Controls::Image^ spotlightArt_ = nullptr;
    Windows::UI::Xaml::Controls::TextBlock^ topClock_ = nullptr;
    Windows::UI::Xaml::Controls::TextBlock^ topDate_ = nullptr;
    Windows::UI::Xaml::Controls::TextBlock^ spotKicker_ = nullptr;
    Windows::UI::Xaml::Controls::TextBlock^ spotTitle_ = nullptr;
    Windows::UI::Xaml::Controls::TextBlock^ spotMeta_ = nullptr;
    // ReVita home: the cover flow (cards turn toward the focused one)
    std::vector<Windows::UI::Xaml::Controls::Button^> flowCards_;
    std::vector<size_t> flowItems_;
    Windows::UI::Xaml::Controls::ScrollViewer^ flowScroll_ = nullptr;
    Windows::UI::Xaml::Controls::StackPanel^ flowRow_ = nullptr;
    int flowFocus_ = -1;
    // navigation: re-renders of the same page keep the controller where it was
    bool focusSettingsTab_ = true;
    // the bundled song that goes with the current theme ("Match the theme")
    int ThemeTrack() const;
    // Home: Play, Game page, Library (Down goes to the poster in the middle)
    std::vector<Windows::UI::Xaml::Controls::Button^> homeActions_;
    static void CollectFocusable(Windows::UI::Xaml::DependencyObject^ node,
                                 std::vector<Windows::UI::Xaml::Controls::Control^>& out);
    void MakeFlow(const std::vector<size_t>& items, size_t focus);
    void FocusFlow(int position, bool animate);
    Windows::UI::Xaml::Controls::Button^ MakeFlowCard(size_t index, int position);
    int spotIndex_ = -1;
    int spotVeilTheme_ = -1;
    std::wstring spotArtUri_;
    int librarySort_ = 0;    // 0 A-Z, 1 recently played
    int libraryFilter_ = 0;  // 0 all, 1 folders, 2 archives and packages
    int settingsTab_ = 0;
    // Mods: cheats (VitaCheat database) and patches (Vita3K's patch list) per game
    int modsGame_ = -1;
    bool modsBusy_ = false;
    std::wstring modsMessage_;
    void Mods();
    void FetchMods(size_t index, bool force);
    void ToggleCheat(const std::wstring& titleId, size_t cheatIndex);
    void TogglePatch(const std::string& key);
    // Settings: the category list on the left (part of the navigation layer)
    std::vector<Windows::UI::Xaml::Controls::Button^> settingsRail_;
    void StyleSettingsRail();
    void FocusSettingsRail();
    // The side menu: every page and (on Settings) every category, all
    // reachable with the D-pad. Keys: a page number, or 100 + a settings tab.
    std::vector<std::pair<int, Windows::UI::Xaml::Controls::Button^>> menuItems_;
    int pendingMenuFocus_ = -1;
    unsigned navigationRevision_ = 0;
    bool pendingContentFocus_ = false;   // the next page drawn takes the controller
    int MenuKeyOf(Platform::Object^ element) const;
    void FocusMenu(int key);
    bool FocusContent();
    bool InWindow(Windows::UI::Xaml::DependencyObject^ element) const;
    int CurrentMenuKey() const;
    bool Navigate(int direction);
    bool HandleNavKey(Windows::System::VirtualKey code);
    void EnsureVisible(Windows::UI::Xaml::Controls::Control^ control);
    void FocusContentSoon(int attempts, bool fromMenu = false);
    void CollectNavStops(Windows::UI::Xaml::DependencyObject^ node, std::vector<Windows::UI::Xaml::Controls::Control^>& out);
    void UpdateSpotlight(int index);
    void HideSpotlight();
    std::vector<size_t> RecentGames();
    void NoteRecent(size_t index);
    Platform::String^ GameMeta(size_t index);
    Windows::UI::Xaml::Controls::Button^ Chip(Platform::String^ label, bool active);
    void MakeGrid(const std::vector<size_t>& items, Windows::UI::Xaml::Controls::StackPanel^ target);
    void SwitchTab(int delta);

    Windows::UI::Color Bg() const;
    Windows::UI::Color Card() const;
    Windows::UI::Color Accent() const;
    Windows::UI::Color Glow() const;
    Windows::UI::Color Muted() const;
    Windows::UI::Color RailColor() const;
    Windows::UI::Xaml::Media::Brush^ Solid(Windows::UI::Color value);
    Windows::UI::Xaml::Media::Brush^ Gradient(Windows::UI::Color a, Windows::UI::Color b);
    Windows::UI::Xaml::Controls::TextBlock^ Text(Platform::String^ value, double size,
                                                  bool bold = false, bool muted = false);
    Windows::UI::Xaml::Controls::Button^ Button(Platform::String^ label, bool highlighted = false);
    Windows::UI::Xaml::Controls::Border^ Panel(Windows::UI::Xaml::UIElement^ child,
                                                double pad = 18);
    Windows::UI::Xaml::Controls::ControlTemplate^ ButtonTemplate(double radius, bool filled);
    void Skin(Windows::UI::Xaml::Controls::Button^ button, double radius, bool filled = false);
    void Reveal(Windows::UI::Xaml::UIElement^ element, double delayMs = 0, double rise = 14);
    Windows::UI::Xaml::Controls::StackPanel^ Vertical(double spacing = 0);
    void Banner(Platform::String^ eyebrow, Platform::String^ headline,
                Platform::String^ description);
    void Section(Platform::String^ label, Platform::String^ note = L"");
    void BuildNavigation();
    void ShowPage(int nextPage);
    void Render();
    void Home();
    void Library();
    void GameDetails();
    void Settings();
    void Diagnostics();
    void MakeShelf(Platform::String^ heading, const std::vector<size_t>& items,
        Windows::UI::Xaml::Controls::StackPanel^ target = nullptr);
    Windows::UI::Xaml::Controls::Button^ MakeTile(size_t index);
    Platform::String^ ArtworkUri(size_t index);
    void ConfigureCover(Windows::UI::Xaml::Controls::Border^ border, size_t index);
    void ChooseGameFolder();
    void ChooseGameFile();
    void RestorePinnedGameFiles(unsigned revision);
    void CheckGameFile(size_t index, unsigned revision);
    void CheckGameFolder(size_t index, unsigned revision);
    void InspectLaunch(size_t index);
    void AttemptBoot(size_t index);
    void StartCore(Platform::String^ contentPath);
    void RestartInto(Platform::String^ contentPath);
    bool restartRefused_ = false;
    // SteamGridDB: the game's own art picked by hand (covers and the wide
    // background behind its page), over the page as a full-screen layer.
    void ShowArtPicker(size_t index);
    void CloseArtPicker();
    void ArtPickerSearch(size_t index, Platform::String^ term);
    void ArtPickerLoad(size_t index, unsigned gameId, Platform::String^ name);
    Windows::UI::Xaml::Controls::Grid^ artPicker_ = nullptr;
    Windows::UI::Xaml::Controls::StackPanel^ artResults_ = nullptr;
    Windows::UI::Xaml::Controls::StackPanel^ artCovers_ = nullptr;
    Windows::UI::Xaml::Controls::StackPanel^ artHeroes_ = nullptr;
    Windows::UI::Xaml::Controls::TextBlock^ artNote_ = nullptr;
    unsigned artRequest_ = 0;
    // The wide picture behind a game's page: its SteamGridDB background
    // (hero), or failing that a screenshot taken while it was played.
    Platform::String^ BackdropUri(size_t index);
    // Pictures decoded once at the size they are shown, and kept: a full-size
    // SteamGridDB hero (up to 3840 wide) decoded again on every page build
    // froze the menus for a moment.
    std::map<std::wstring, Windows::UI::Xaml::Media::Imaging::BitmapImage^> bitmapCache_;
    Windows::UI::Xaml::Media::Imaging::BitmapImage^ CachedBitmap(Platform::String^ uri, int decodeWidth);
    std::wstring screenshotPath_;
    bool screenshotTaken_ = false;
    // PS3-style XMB home (Settings, Appearance, Home screen)
    bool xmbMode_ = false;
    int xmbCat_ = -1;
    std::vector<int> xmbItem_;
    double xmbPhase_ = 0;
    Windows::UI::Xaml::Controls::Grid^ xmb_ = nullptr;
    Windows::UI::Xaml::Controls::Canvas^ xmbWaves_ = nullptr;
    Windows::UI::Xaml::Controls::Canvas^ xmbStage_ = nullptr;
    Windows::UI::Xaml::Controls::TextBlock^ xmbClock_ = nullptr;
    Windows::UI::Xaml::DispatcherTimer^ xmbTimer_ = nullptr;
    std::map<std::wstring, Windows::UI::Xaml::Media::Imaging::BitmapImage^> xmbIcons_;
    std::vector<XmbCategory> XmbCategories();
    void ShowXmb();
    void HideXmb();
    void LayoutXmb(int dx, int dy);
    void XmbWaves();
    bool XmbKey(Windows::System::VirtualKey key);
    void SaveLayout(bool xmb);
    int xmbTheme_ = 0;
    void ApplyXmbSky();
    void SaveXmbTheme(int theme);
    void EnterGameView(Platform::String^ title);
    void ShowLoadFailure(Platform::String^ detail);
    void ExitGameView();
    void ShowQuickMenu();
    void HideQuickMenu();
    void GameTick();
    void UpdateGameStats();
    void ChooseLicenseForGame(size_t index);
    void ChooseFirmware();
    void ChooseFirmwareFolder();
    // the system software straight from PlayStation's update server, then installed
    void DownloadFirmware();
    bool firmwareDownloading_ = false;
    void InstallFirmwareFiles(std::vector<Windows::Storage::StorageFile^> files);
    void ShowFirmwareProgress(double value, Platform::String^ message);
    void StorageSettings();
    void ChooseDataFolder();
    // Updates and DLC: a folder of them, installed when new.
    void ChooseAddonsFolder();
    void InstallAddons();
    Windows::UI::Xaml::Controls::TextBlock^ addonsLabel_ = nullptr;
    bool addonsBusy_ = false;
    void ChangeDataRoot(const std::wstring& chosen);
    void CleanUpOldCopies();
    bool SystemFirmwareInstalled();
    void SetupCard();
    void SaveLogToDrive();
    std::wstring RecentLogText();
    void RefreshFirmwareStatus();
    void ResetInstalledFirmware();
    void FirmwareSettings();
    void EmulationSettings();
    void ApplyCoreOptions(const std::wstring& contentPath = std::wstring());
    void RestorePaths();
    void ScanGames();
    void ReadSfo(size_t index, Windows::Storage::StorageFolder^ folder);
    void ChooseCustomArtwork(size_t index);
    void SaveTheme(int theme);
    void SetMusic(int track);
    void PlayBundledMusic(int track);
    void PrepareCues();
    void PlayCue(unsigned kind);
    void SaveCueSettings();
    void PlayCustomMusic();
    void PickCustomMusic();
    void SetVolume(double volume);
    void SaveApiKey(Platform::String^ key);
    Platform::String^ ApiKey();
    void FetchArtwork(size_t index, bool continueBatch = false);
    void FetchMissingArtwork(size_t index = 0);
    void ProbeMemory();
    void ProbeCore();
    void ProbeLibretro(bool initialize);
    void BootRenderer();
    void PresentTestFrame();
    void SetStatus(Platform::String^ message);
};
}
