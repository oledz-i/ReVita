// SPDX-License-Identifier: GPL-2.0-or-later
// Xbox-first, streaming-catalog frontend. UI is not a claim of game compatibility.
#include "CatalogShell.h"
#include "MenuMusic.h"
#include "SfoReader.h"
#include "GameFormats.h"
#include "VulkanDznProbe.h"
#include "VitaGuestCpuUwpProbe.h"
#include "CoreOptions.h"
#include "UwpFileRedirect.h"
#include "CrashLog.h"
#include "GameInfo.h"
#include <fileapifromapp.h>
#include <share.h>
#include <cstdio>
#include "UwpJitProbe.h"
#include <mem/allocator.h>
#include <windows.h>
#include <collection.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <system_error>
#include <string>
#include <stdexcept>
#include <vector>
#include <windows.foundation.h>
#include <windows.ui.xaml.media.imaging.h>
#include <windows.ui.xaml.shapes.h>

using namespace concurrency;
using namespace Windows::ApplicationModel;
using namespace Windows::Foundation;
using namespace Windows::Storage;
using namespace Windows::Storage::AccessCache;
using namespace Windows::Storage::Pickers;
using namespace Windows::Storage::Streams;
using namespace Windows::UI;
using namespace Windows::UI::Text;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Media::Imaging;
using namespace Windows::UI::Xaml::Media::Animation;
using namespace Windows::UI::Xaml::Markup;
using namespace Windows::UI::Xaml::Shapes;

namespace Vita3KUWP {
namespace {
Color RgbColor(unsigned r, unsigned g, unsigned b) {
    return ColorHelper::FromArgb(255, static_cast<unsigned char>(r),
        static_cast<unsigned char>(g), static_cast<unsigned char>(b));
}
Platform::String^ Pref(Platform::String^ key, Platform::String^ fallback = L"") {
    auto values = ApplicationData::Current->LocalSettings->Values;
    if (!values->HasKey(key)) return fallback;
    return safe_cast<Platform::String^>(values->Lookup(key));
}
void BackupSettings();
void RestoreSettingsFromDrives(std::function<void()> done);
void PutPref(Platform::String^ key, Platform::String^ value) {
    ApplicationData::Current->LocalSettings->Values->Insert(key, value);
    BackupSettings();
}
// Defined with the firmware helpers further down.
std::wstring FirmwareRoot();
bool HasFiles(const std::wstring& directory);
std::string NarrowUtf8(const std::wstring& text);
std::wstring WidenUtf8(const std::string& text);
TimeSpan Millis(double ms) {
    TimeSpan span;
    span.Duration = static_cast<long long>(ms * 10000.0);
    return span;
}
std::wstring HexOf(Color c) {
    wchar_t buffer[16] = {};
    swprintf_s(buffer, L"#%02X%02X%02X%02X", c.A, c.R, c.G, c.B);
    return std::wstring(buffer);
}
Color Alpha(unsigned a, unsigned r, unsigned g, unsigned b) {
    return ColorHelper::FromArgb(static_cast<unsigned char>(a), static_cast<unsigned char>(r),
        static_cast<unsigned char>(g), static_cast<unsigned char>(b));
}
// ReVita's ribbon: cyan, violet, pink (the mark's own gradient).
LinearGradientBrush^ BrandBrush(double alpha = 1.0, bool vertical = false) {
    auto brush = ref new LinearGradientBrush();
    brush->StartPoint = Point(0, vertical ? 0.0 : 0.5);
    brush->EndPoint = Point(vertical ? 0.0 : 1.0, vertical ? 1.0 : 0.5);
    const unsigned a = static_cast<unsigned>(std::max(0.0, std::min(1.0, alpha)) * 255.0);
    const unsigned stops[][4] = { { 47, 230, 255, 0 }, { 124, 92, 255, 48 }, { 255, 79, 180, 100 } };
    for (const auto& c : stops) {
        auto stop = ref new GradientStop();
        stop->Color = Alpha(a, c[0], c[1], c[2]);
        stop->Offset = c[3] / 100.0;
        brush->GradientStops->Append(stop);
    }
    return brush;
}
VariableSizedWrapGrid^ ActionRow(double itemWidth) {
    // Wraps onto a second line instead of running off the edge of the screen.
    auto row = ref new VariableSizedWrapGrid();
    row->Orientation = Orientation::Horizontal;
    row->ItemWidth = itemWidth;
    row->ItemHeight = 46;
    row->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
    return row;
}
// The PS3's background takes a colour for each month.
Color XmbMonthColor(int month) {
    static const unsigned colors[12][3] = {
        { 160, 164, 172 }, // January: silver
        { 190, 152, 46 },  // February: gold
        { 118, 162, 44 },  // March: lime
        { 200, 104, 150 }, // April: pink
        { 40, 136, 66 },   // May: green
        { 146, 78, 164 },  // June: purple
        { 32, 146, 156 },  // July: teal
        { 40, 106, 200 },  // August: blue
        { 84, 60, 164 },   // September: violet
        { 180, 106, 40 },  // October: amber
        { 122, 84, 54 },   // November: brown
        { 172, 38, 44 },   // December: red
    };
    const auto& c = colors[(month >= 1 && month <= 12) ? month - 1 : 0];
    return RgbColor(c[0], c[1], c[2]);
}
Color Shade(Color c, double f) {
    auto ch = [f](unsigned char v) { return static_cast<unsigned>(std::min(255.0, std::max(0.0, v * f))); };
    return RgbColor(ch(c.R), ch(c.G), ch(c.B));
}
// The XMB's colours: the PS3's monthly one, or one kept all year.
struct XmbTheme {
    const wchar_t* name;
    unsigned char top[3];
    unsigned char bottom[3];
    bool monthly;
};
const XmbTheme kXmbThemes[] = {
    { L"Monthly (PS3)", { 0, 0, 0 }, { 0, 0, 0 }, true },
    { L"Classic Blue", { 6, 22, 70 }, { 34, 104, 196 }, false },
    { L"Vita Blue", { 2, 16, 44 }, { 0, 112, 204 }, false },
    { L"Crimson", { 44, 4, 10 }, { 168, 28, 42 }, false },
    { L"Emerald", { 4, 34, 20 }, { 26, 136, 78 }, false },
    { L"Royal Purple", { 24, 8, 52 }, { 116, 58, 178 }, false },
    { L"Ocean", { 2, 30, 40 }, { 20, 146, 166 }, false },
    { L"Rose", { 56, 14, 36 }, { 204, 104, 146 }, false },
    { L"Sunset", { 36, 10, 46 }, { 226, 104, 58 }, false },
    { L"Silver", { 46, 48, 56 }, { 164, 168, 180 }, false },
    { L"Midnight", { 2, 2, 6 }, { 24, 26, 36 }, false },
    { L"Sakura", { 46, 14, 34 }, { 232, 140, 178 }, false },
    { L"Synthwave", { 20, 4, 44 }, { 236, 64, 180 }, false },
    { L"Forest", { 4, 24, 12 }, { 84, 170, 92 }, false },
    { L"Gold", { 30, 20, 4 }, { 214, 166, 64 }, false },
    { L"Ice", { 12, 30, 48 }, { 164, 214, 246 }, false },
};
constexpr int kXmbThemeCount = static_cast<int>(sizeof(kXmbThemes) / sizeof(kXmbThemes[0]));
Platform::String^ StringOf(const std::wstring& value) {
    return ref new Platform::String(value.c_str());
}
bool ContainsIgnoreCase(Platform::String^ source, Platform::String^ search) {
    if (search == nullptr || search->IsEmpty()) return true;
    std::wstring a(source->Data()), b(search->Data());
    std::transform(a.begin(), a.end(), a.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    std::transform(b.begin(), b.end(), b.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return a.find(b) != std::wstring::npos;
}
Platform::String^ StableKey(Platform::String^ path) {
    // Stable FNV-1a over UTF-16 code units; no user paths sent to artwork services.
    std::uint64_t hash = 14695981039346656037ULL;
    for (const wchar_t* p = path->Data(); *p; ++p) {
        hash ^= static_cast<unsigned short>(*p);
        hash *= 1099511628211ULL;
    }
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"%016llx", static_cast<unsigned long long>(hash));
    return ref new Platform::String(buffer);
}
bool LooksLikeGameFile(StorageFile^ file) {
    return vita3k_uwp::ClassifyGameFile(std::wstring(file->Name->Data())) !=
           vita3k_uwp::GameFormat::Unknown;
}
Platform::String^ PrettyFileName(Platform::String^ value) {
    std::wstring result(value->Data());
    const auto dot = result.find_last_of(L'.');
    if (dot != std::wstring::npos) result.erase(dot);
    std::replace(result.begin(), result.end(), L'_', L' ');
    return StringOf(result);
}
}
Color CatalogShell::Bg() const {
    switch (theme_) {
    case 1: return RgbColor(14, 6, 24);       // Synthwave: neon dusk
    case 2: return RgbColor(6, 10, 22);       // Miami Night: night sea
    case 3: return RgbColor(20, 14, 30);      // Pastel Dream: lavender haze
    case 4: return RgbColor(10, 4, 22);       // Ultraviolet: deep violet
    case 5: return RgbColor(14, 12, 30);      // Cotton Sky: dusk blue
    case 6: return RgbColor(6, 8, 24);        // Midnight Arcade: navy
    case 7: return RgbColor(8, 8, 20);        // Starfall: night sky
    default: return RgbColor(8, 7, 16);       // ReVita: violet night
    }
}
Color CatalogShell::Card() const {
    // Glass: the moving scene shows through every card a little.
    switch (theme_) {
    case 1: return Alpha(206, 30, 16, 46);
    case 2: return Alpha(206, 14, 24, 46);
    case 3: return Alpha(206, 36, 28, 52);
    case 4: return Alpha(206, 26, 14, 50);
    case 5: return Alpha(206, 28, 26, 56);
    case 6: return Alpha(206, 14, 20, 50);
    case 7: return Alpha(206, 22, 22, 44);
    default: return Alpha(206, 22, 19, 40);
    }
}
Color CatalogShell::Accent() const {
    switch (theme_) {
    case 1: return RgbColor(255, 92, 205);    // hot magenta
    case 2: return RgbColor(64, 224, 255);    // neon cyan
    case 3: return RgbColor(255, 170, 222);   // pastel pink
    case 4: return RgbColor(176, 102, 255);   // ultraviolet
    case 5: return RgbColor(130, 200, 255);   // baby blue
    case 6: return RgbColor(255, 84, 170);    // arcade pink
    case 7: return RgbColor(184, 170, 255);   // periwinkle
    default: return RgbColor(139, 108, 255);  // ReVita violet
    }
}
Color CatalogShell::Glow() const {
    switch (theme_) {
    case 1: return RgbColor(92, 26, 120);
    case 2: return RgbColor(130, 30, 110);    // the pink against the cyan
    case 3: return RgbColor(90, 70, 150);
    case 4: return RgbColor(40, 30, 150);     // electric blue under violet
    case 5: return RgbColor(120, 60, 130);
    case 6: return RgbColor(20, 70, 150);
    case 7: return RgbColor(44, 40, 120);
    default: return RgbColor(58, 30, 112);
    }
}
Color CatalogShell::Muted() const {
    return RgbColor(153, 157, 169);
}
Color CatalogShell::RailColor() const {
    // The rail sits a step darker than the page so the content reads as one raised surface.
    const Color page = Bg();
    return RgbColor(page.R * 7 / 10, page.G * 7 / 10, page.B * 7 / 10);
}
Brush^ CatalogShell::Solid(Color value) {
    return ref new SolidColorBrush(value);
}
Brush^ CatalogShell::Gradient(Color a, Color b) {
    auto brush = ref new LinearGradientBrush();
    brush->StartPoint = Point(0, 0);
    brush->EndPoint = Point(1, 1);
    auto x = ref new GradientStop();
    x->Color = a;
    x->Offset = 0;
    auto y = ref new GradientStop();
    y->Color = b;
    y->Offset = 1;
    brush->GradientStops->Append(x);
    brush->GradientStops->Append(y);
    return brush;
}
TextBlock^ CatalogShell::Text(Platform::String^ value, double size, bool bold, bool muted) {
    auto t = ref new TextBlock();
    t->Text = value;
    t->FontSize = size;
    t->FontFamily = ref new Windows::UI::Xaml::Media::FontFamily(size >= 28 ? L"Bahnschrift" : L"Segoe UI");
    t->FontWeight = bold ? FontWeights::SemiBold : FontWeights::Normal;
    t->Foreground = Solid(muted ? Muted() : Colors::White);
    t->TextWrapping = TextWrapping::Wrap;
    t->TextTrimming = TextTrimming::CharacterEllipsis;
    t->Margin = Thickness(0);
    return t;
}
ControlTemplate^ CatalogShell::ButtonTemplate(double radius, bool filled) {
    // One template for every focusable surface. Focus lifts the control with a soft
    // spring and draws an accent ring; a press squeezes it slightly. Both run as
    // transform/opacity animations, so they stay smooth on the console.
    const std::wstring ring = filled ? L"#FFFFFFFF" : HexOf(Accent());
    const std::wstring r = std::to_wstring(radius);
    std::wstring xaml =
        L"<ControlTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' "
        L"xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml' TargetType='Button'>"
        L"<Grid x:Name='Root' RenderTransformOrigin='0.5,0.5' Background='Transparent'>"
        L"<Grid.RenderTransform><ScaleTransform x:Name='FocusScale'/></Grid.RenderTransform>"
        L"<Grid x:Name='Inner' RenderTransformOrigin='0.5,0.5'>"
        L"<Grid.RenderTransform><ScaleTransform x:Name='PressScale'/></Grid.RenderTransform>"
        L"<Border x:Name='Plate' CornerRadius='" + r + L"' Background='{TemplateBinding Background}' "
        L"BorderBrush='{TemplateBinding BorderBrush}' BorderThickness='{TemplateBinding BorderThickness}'/>"
        L"<Border x:Name='Sheen' CornerRadius='" + r + L"' Background='#FFFFFFFF' Opacity='0' IsHitTestVisible='False'/>"
        L"<Border x:Name='Ring' CornerRadius='" + r + L"' BorderBrush='" + ring + L"' BorderThickness='2' "
        L"Opacity='0' IsHitTestVisible='False'/>"
        L"<ContentPresenter x:Name='Presenter' Content='{TemplateBinding Content}' "
        L"ContentTemplate='{TemplateBinding ContentTemplate}' Padding='{TemplateBinding Padding}' "
        L"Foreground='{TemplateBinding Foreground}' FontSize='{TemplateBinding FontSize}' "
        L"FontFamily='{TemplateBinding FontFamily}' FontWeight='{TemplateBinding FontWeight}' "
        L"HorizontalContentAlignment='{TemplateBinding HorizontalContentAlignment}' "
        L"VerticalContentAlignment='{TemplateBinding VerticalContentAlignment}'/>"
        L"</Grid>"
        L"<VisualStateManager.VisualStateGroups>"
        L"<VisualStateGroup x:Name='CommonStates'>"
        L"<VisualStateGroup.Transitions><VisualTransition GeneratedDuration='0:0:0.16'>"
        L"<VisualTransition.GeneratedEasingFunction><CubicEase EasingMode='EaseOut'/></VisualTransition.GeneratedEasingFunction>"
        L"</VisualTransition></VisualStateGroup.Transitions>"
        L"<VisualState x:Name='Normal'/>"
        L"<VisualState x:Name='PointerOver'><Storyboard>"
        L"<DoubleAnimation Storyboard.TargetName='Sheen' Storyboard.TargetProperty='Opacity' To='0.08' Duration='0:0:0.12'/>"
        L"</Storyboard></VisualState>"
        L"<VisualState x:Name='Pressed'><Storyboard>"
        L"<DoubleAnimation Storyboard.TargetName='Sheen' Storyboard.TargetProperty='Opacity' To='0.16' Duration='0:0:0.06'/>"
        L"<DoubleAnimation Storyboard.TargetName='PressScale' Storyboard.TargetProperty='ScaleX' To='0.965' Duration='0:0:0.08'/>"
        L"<DoubleAnimation Storyboard.TargetName='PressScale' Storyboard.TargetProperty='ScaleY' To='0.965' Duration='0:0:0.08'/>"
        L"</Storyboard></VisualState>"
        L"<VisualState x:Name='Disabled'><Storyboard>"
        L"<DoubleAnimation Storyboard.TargetName='Inner' Storyboard.TargetProperty='Opacity' To='0.38' Duration='0:0:0.01'/>"
        L"</Storyboard></VisualState>"
        L"</VisualStateGroup>"
        L"<VisualStateGroup x:Name='FocusStates'>"
        L"<VisualStateGroup.Transitions><VisualTransition GeneratedDuration='0:0:0.2'>"
        L"<VisualTransition.GeneratedEasingFunction><CubicEase EasingMode='EaseOut'/></VisualTransition.GeneratedEasingFunction>"
        L"</VisualTransition></VisualStateGroup.Transitions>"
        L"<VisualState x:Name='Focused'><Storyboard>"
        L"<DoubleAnimation Storyboard.TargetName='Ring' Storyboard.TargetProperty='Opacity' To='1' Duration='0:0:0.16'/>"
        L"<DoubleAnimation Storyboard.TargetName='FocusScale' Storyboard.TargetProperty='ScaleX' To='1.045' Duration='0:0:0.2'/>"
        L"<DoubleAnimation Storyboard.TargetName='FocusScale' Storyboard.TargetProperty='ScaleY' To='1.045' Duration='0:0:0.2'/>"
        L"</Storyboard></VisualState>"
        L"<VisualState x:Name='Unfocused'/>"
        L"<VisualState x:Name='PointerFocused'/>"
        L"</VisualStateGroup>"
        L"</VisualStateManager.VisualStateGroups>"
        L"</Grid></ControlTemplate>";
    try {
        return dynamic_cast<ControlTemplate^>(XamlReader::Load(StringOf(xaml)));
    } catch (Platform::Exception^) {
        return nullptr;   // fall back to the stock template rather than fail to launch
    }
}
void CatalogShell::Skin(Windows::UI::Xaml::Controls::Button^ button, double radius, bool filled) {
    auto custom = ButtonTemplate(radius, filled);
    if (custom != nullptr) button->Template = custom;
    button->UseSystemFocusVisuals = false;
}
void CatalogShell::Reveal(UIElement^ element, double delayMs, double rise) {
    if (element == nullptr) return;
    element->Opacity = 0;
    auto board = ref new Storyboard();
    auto ease = ref new CubicEase();
    ease->EasingMode = EasingMode::EaseOut;
    auto fade = ref new DoubleAnimation();
    fade->From = 0.0;
    fade->To = 1.0;
    fade->Duration = DurationHelper::FromTimeSpan(Millis(380));
    fade->BeginTime = Millis(delayMs);
    fade->EasingFunction = ease;
    Storyboard::SetTarget(fade, element);
    Storyboard::SetTargetProperty(fade, L"Opacity");
    board->Children->Append(fade);
    if (rise > 0 && element->RenderTransform == nullptr) {
        auto shift = ref new TranslateTransform();
        shift->Y = rise;
        element->RenderTransform = shift;
        auto lift = ref new DoubleAnimation();
        lift->From = rise;
        lift->To = 0.0;
        lift->Duration = DurationHelper::FromTimeSpan(Millis(460));
        lift->BeginTime = Millis(delayMs);
        auto liftEase = ref new CubicEase();
        liftEase->EasingMode = EasingMode::EaseOut;
        lift->EasingFunction = liftEase;
        Storyboard::SetTarget(lift, shift);
        Storyboard::SetTargetProperty(lift, L"Y");
        board->Children->Append(lift);
    }
    board->Begin();
}
Button^ CatalogShell::Button(Platform::String^ label, bool highlighted) {
    auto button = ref new Windows::UI::Xaml::Controls::Button();
    button->Content = label;
    button->Padding = Thickness(18, 8, 18, 9);
    button->Margin = Thickness(0, 0, 10, 0);
    button->MinHeight = 38;
    button->FontFamily = ref new Windows::UI::Xaml::Media::FontFamily(L"Segoe UI");
    button->FontWeight = FontWeights::SemiBold;
    button->FontSize = 13;
    button->BorderThickness = Thickness(1);
    button->BorderBrush = Solid(highlighted ? Alpha(0, 0, 0, 0) : Alpha(34, 255, 255, 255));
    button->Background = Solid(highlighted ? Accent() : Alpha(22, 255, 255, 255));
    button->Foreground = Solid(highlighted ? RgbColor(14, 14, 17) : Colors::White);
    Skin(button, 10, highlighted);
    button->GotFocus += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) {
        PlayCue(0);
    });
    button->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) {
        PlayCue(1);
    });
    return button;
}
StackPanel^ CatalogShell::Vertical(double spacing) {
    auto panel = ref new StackPanel();
    panel->Orientation = Orientation::Vertical;
    panel->Margin = Thickness(0, spacing, 0, spacing);
    return panel;
}
Border^ CatalogShell::Panel(UIElement^ child, double pad) {
    auto border = ref new Border();
    border->CornerRadius = Windows::UI::Xaml::CornerRadius(18);
    border->Padding = Thickness(pad);
    border->Margin = Thickness(0, 8, 0, 10);
    border->Background = Solid(Card());
    border->BorderBrush = BrandBrush(0.22);
    border->BorderThickness = Thickness(1);
    border->Child = child;
    return border;
}
void CatalogShell::SetStatus(Platform::String^ message) {
    if (status_ != nullptr) status_->Text = message;
}
CatalogShell::CatalogShell() {
    {
        // ReVita's eight themes. A theme saved before them maps to the closest one.
        const auto saved = Pref(L"revita.theme", L"");
        if (!saved->IsEmpty()) {
            theme_ = _wtoi(saved->Data());
        } else {
            const int old = _wtoi(Pref(L"theme", L"0")->Data());
            //                  Onyx Aurora Ember Glacier Orbit Sakura Synth Verdant Gilded Abyss Crimson Prism
            const int map[12] = { 0,   4,     1,    5,      7,    3,     1,    0,      2,     6,    1,      7 };
            theme_ = (old >= 0 && old < 12) ? map[old] : 0;
            PutPref(L"revita.theme", StringOf(std::to_wstring(theme_)));
        }
        if (theme_ < 0 || theme_ > 7) theme_ = 0;
    }
    xmbMode_ = Pref(L"ui.layout", L"cinematic")->Equals(L"xmb");
    xmbTheme_ = _wtoi(Pref(L"xmb.theme", L"1")->Data());
    if (xmbTheme_ < 0 || xmbTheme_ >= kXmbThemeCount) xmbTheme_ = 0;
    const auto savedTrack = Pref(L"music.track", L"10");
    // 0 off, 1-4 and 6-9 bundled songs, 5 the player's own, 10 the theme's song
    track_ = _wtoi(savedTrack->Data());
    if (track_ < 0 || track_ > 10 || (track_ == 0 && !savedTrack->Equals(L"0"))) track_ = 10;
    try {
        musicVolume_ = std::stod(Pref(L"music.volume", L"0.45")->Data());
    } catch (...) { musicVolume_ = 0.45; }
    musicVolume_ = std::max(0.0, std::min(1.0, musicVolume_));
    uiSounds_ = !Pref(L"ui.sounds", L"1")->Equals(L"0");
    librarySort_ = Pref(L"library.sort", L"0")->Equals(L"1") ? 1 : 0;
    libraryFilter_ = std::max(0, std::min(2, _wtoi(Pref(L"library.filter", L"0")->Data())));
    settingsTab_ = std::max(0, std::min(5, _wtoi(Pref(L"settings.tab", L"0")->Data())));
    try { uiSoundVolume_ = std::stod(Pref(L"ui.sound.volume", L"0.40")->Data()); }
    catch (...) { uiSoundVolume_ = 0.40; }
    uiSoundVolume_ = std::max(0.0, std::min(1.0, uiSoundVolume_));

    // Paths in the app's own storage keep the fast plain file functions;
    // everything else (USB drives) goes through the FromApp ones.
    {
        std::wstring local = ApplicationData::Current->LocalFolder->Path->Data();
        const auto slash = local.find_last_of(L'\\');
        std::vector<std::wstring> roots;
        roots.push_back(slash == std::wstring::npos ? local : local.substr(0, slash));
        try { roots.push_back(Windows::ApplicationModel::Package::Current->InstalledLocation->Path->Data()); }
        catch (Platform::Exception^) {}
        vita3k_uwp::SetAppStorageRoots(roots);
    }
    // The app's own file calls (std::filesystem in Settings) reach USB drives too.
    vita3k_uwp::RedirectCoreFileAccess(vita3k_uwp::ModuleFromAddress(reinterpret_cast<const void*>(&Pref)));
    {
        // A log from an earlier run that grew huge is cleared, not appended to.
        const std::wstring logPath = std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\vita3k-runtime.log";
        std::error_code sizeEc;
        const auto logSize = std::filesystem::file_size(logPath, sizeEc);
        if (!sizeEc && logSize > (80ull << 20)) DeleteFileW(logPath.c_str());
    }
    libretro_.SetLogPath(std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\vita3k-runtime.log");
    {
        // A game was running when the app last ended and nothing cleared the
        // marker: the app was closed or terminated mid-game. A crash the
        // crash log caught is above in the log; a fail-fast or the console
        // ending the app leaves only this.
        const std::wstring marker = std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\vita3k-session.txt";
        FILE* file = nullptr;
        if (_wfopen_s(&file, marker.c_str(), L"rb") == 0 && file) {
            char text[400] = {};
            const size_t got = fread(text, 1, sizeof(text) - 1, file);
            fclose(file);
            text[got] = 0;
            libretro_.LogLine(std::string("Previous session ended while a game was running, without returning to the library (") + text +
                "). If no CRASH: lines precede this, the app was closed from the console or ended by a fail-fast.");
            DeleteFileW(marker.c_str());
        }
    }
    CleanUpOldCopies();
    RestoreSettingsFromDrives([this]() {
        SetStatus(L"Found your saved settings on the drive and restored them.");
        RestorePaths();
        ScanGames();
        Render();
    });

    // The dashboard: a top bar with the tabs and the time, the page under it
    // over the living scene (and the focused game's picture on Home), and an
    // understated status line. Column 0 (the old rail) is kept at no width so
    // every full-screen layer still spans both columns.
    root_ = ref new Grid();
    // The controller moves to the nearest control in the direction pressed,
    // even when nothing lines up exactly (rows of different lengths, a button
    // on the far right): focus never just stays put.
    root_->XYFocusUpNavigationStrategy = Windows::UI::Xaml::Input::XYFocusNavigationStrategy::NavigationDirectionDistance;
    root_->XYFocusDownNavigationStrategy = Windows::UI::Xaml::Input::XYFocusNavigationStrategy::NavigationDirectionDistance;
    root_->XYFocusLeftNavigationStrategy = Windows::UI::Xaml::Input::XYFocusNavigationStrategy::NavigationDirectionDistance;
    root_->XYFocusRightNavigationStrategy = Windows::UI::Xaml::Input::XYFocusNavigationStrategy::NavigationDirectionDistance;
    root_->Background = Solid(Bg());
    auto railColumn = ref new ColumnDefinition();
    railColumn->Width = GridLength(0);   // kept at no width so every full-screen layer spans both columns
    auto contentColumn = ref new ColumnDefinition();
    contentColumn->Width = GridLength(1, GridUnitType::Star);
    root_->ColumnDefinitions->Append(railColumn);
    root_->ColumnDefinitions->Append(contentColumn);
    auto mainRow = ref new RowDefinition();
    mainRow->Height = GridLength(1, GridUnitType::Star);
    auto footerRow = ref new RowDefinition();
    footerRow->Height = GridLength(26);
    root_->RowDefinitions->Append(mainRow);
    root_->RowDefinitions->Append(footerRow);

    // ReVita: the bar across the top (mark, dock, time). The page starts
    // under it, so the bar never covers anything.
    navigation_ = ref new Border();
    navigation_->Background = nullptr;
    Grid::SetColumn(navigation_, 0);
    Grid::SetColumnSpan(navigation_, 2);
    Grid::SetRow(navigation_, 0);

    auto scroll = ref new ScrollViewer();
    scroll->IsTabStop = false;
    contentScroll_ = scroll;
    scroll->VerticalScrollBarVisibility = ScrollBarVisibility::Hidden;
    scroll->HorizontalScrollBarVisibility = ScrollBarVisibility::Disabled;
    content_ = Vertical();
    content_->Margin = Thickness(56, 10, 56, 28);
    scroll->Content = content_;
    // under the bar (BuildNavigation sets the height for the page)
    scroll->Margin = Thickness(0, 70, 0, 0);
    Grid::SetColumn(scroll, 1);
    Grid::SetRow(scroll, 0);
    root_->Children->Append(scroll);
    // over the page, so its tabs stay on top while the page scrolls
    root_->Children->Append(navigation_);

    auto statusBar = ref new Border();
    statusBar_ = statusBar;
    statusBar->Background = Solid(Bg());
    statusBar->Padding = Thickness(32, 3, 48, 2);
    status_ = Text(L"REVITA  /  BETA  /  POWERED BY VITA3K", 10, false, true);
    status_->CharacterSpacing = 80;
    status_->TextTrimming = TextTrimming::CharacterEllipsis;
    status_->TextWrapping = TextWrapping::NoWrap;
    statusBar->Child = status_;
    Grid::SetColumn(statusBar, 1);
    Grid::SetRow(statusBar, 1);
    root_->Children->Append(statusBar);

    // The living backdrop, behind everything (z-index), over the page colour.
    backdropHost_ = ref new Grid();
    backdropHost_->IsHitTestVisible = false;
    Grid::SetColumnSpan(backdropHost_, 2);
    Grid::SetRowSpan(backdropHost_, 2);
    Canvas::SetZIndex(backdropHost_, -10);
    root_->Children->Append(backdropHost_);
    backdrop_.Attach(backdropHost_);
    backdrop_.SetTheme(theme_, Bg(), Accent(), Glow());
    // The spotlight: the focused game's wide picture over the scene, faded
    // into the page colour where the text sits (left) and at the bottom.
    spotlight_ = ref new Grid();
    spotlight_->IsHitTestVisible = false;
    spotlight_->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    Grid::SetColumnSpan(spotlight_, 2);
    Grid::SetRowSpan(spotlight_, 2);
    Canvas::SetZIndex(spotlight_, -5);
    spotlightArt_ = ref new Image();
    spotlightArt_->Stretch = Stretch::UniformToFill;
    spotlightArt_->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Right;
    spotlightArt_->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Top;
    spotlightArt_->Opacity = 0;
    spotlight_->Children->Append(spotlightArt_);
    root_->Children->Append(spotlight_);
    ApplySurfaceColors();
    clockTimer_ = ref new DispatcherTimer();
    clockTimer_->Interval = Millis(15000);
    clockTimer_->Tick += ref new EventHandler<Platform::Object^>([this](Platform::Object^, Platform::Object^) { UpdateClock(); });
    clockTimer_->Start();

    musicPlayer_ = ref new MediaElement();
    musicPlayer_->AutoPlay = false;
    musicPlayer_->IsLooping = true;
    musicPlayer_->Volume = musicVolume_;
    // Xbox Media Foundation is more reliable with a loaded media element.
    // A 1x1, nearly transparent element keeps the audio pipeline initialized.
    musicPlayer_->Width = 1;
    musicPlayer_->Height = 1;
    musicPlayer_->Opacity = 0.01;
    musicPlayer_->IsHitTestVisible = false;
    Grid::SetRow(musicPlayer_, 1);
    Grid::SetColumn(musicPlayer_, 1);
    root_->Children->Append(musicPlayer_);
    musicPlayer_->MediaOpened += ref new RoutedEventHandler(
        [this](Platform::Object^, RoutedEventArgs^) {
            if (track_ == 0) return;
            musicPlayer_->Volume = musicVolume_;
            musicPlayer_->Play();
            libretro_.LogLine("Menu music opened: " + NarrowUtf8(musicPlayer_->Source ? musicPlayer_->Source->AbsoluteUri->Data() : L"custom track"));
            playbackStatus_ = L"Track opened. Starting playback...";
            if (musicStatus_ != nullptr) musicStatus_->Text = playbackStatus_;
        });
    musicPlayer_->CurrentStateChanged += ref new RoutedEventHandler(
        [this](Platform::Object^, RoutedEventArgs^) {
            if (musicPlayer_->CurrentState == MediaElementState::Playing) {
                playbackStatus_ = L"Playing";
                if (musicStatus_ != nullptr) musicStatus_->Text = playbackStatus_;
            }
        });
    musicPlayer_->MediaFailed += ref new ExceptionRoutedEventHandler(
        [this](Platform::Object^, ExceptionRoutedEventArgs^ e) {
            playbackStatus_ = L"Audio playback failed: " + e->ErrorMessage;
            if (musicStatus_ != nullptr) musicStatus_->Text = playbackStatus_;
            SetStatus(playbackStatus_);
            libretro_.LogLine("Menu music failed: " + NarrowUtf8(e->ErrorMessage ? e->ErrorMessage->Data() : L"") +
                " (" + NarrowUtf8(musicPlayer_->Source ? musicPlayer_->Source->AbsoluteUri->Data() : L"no source") + ")");
        });
    for (unsigned cue = 0; cue < vita3k_uwp::kInterfaceCueCount; ++cue) {
        cuePlayers_[cue] = ref new MediaElement();
        cuePlayers_[cue]->AutoPlay = false;
        cuePlayers_[cue]->IsLooping = false;
        cuePlayers_[cue]->Volume = uiSoundVolume_;
        cuePlayers_[cue]->Width = 1;
        cuePlayers_[cue]->Height = 1;
        cuePlayers_[cue]->Opacity = 0.01;
        cuePlayers_[cue]->IsHitTestVisible = false;
        Grid::SetRow(cuePlayers_[cue], 1);
        Grid::SetColumn(cuePlayers_[cue], 1);
        root_->Children->Append(cuePlayers_[cue]);
        const unsigned cueIndex = cue;
        cuePlayers_[cue]->MediaOpened += ref new RoutedEventHandler(
            [this,cueIndex](Platform::Object^, RoutedEventArgs^) {
                cueReady_[cueIndex] = true;
            });
        cuePlayers_[cue]->MediaFailed += ref new ExceptionRoutedEventHandler(
            [this,cueIndex](Platform::Object^, ExceptionRoutedEventArgs^) {
                cueReady_[cueIndex] = false;
            });
    }

    Application::Current->Suspending += ref new SuspendingEventHandler(
        [this](Platform::Object^, SuspendingEventArgs^ args) {
            auto deferral = args->SuspendingOperation->GetDeferral();
            create_task([this,deferral] {
                libretro_.StopRunning();
                deferral->Complete();
            });
        });
    Content = root_;
    Window::Current->CoreWindow->KeyDown +=
        ref new TypedEventHandler<Windows::UI::Core::CoreWindow^, Windows::UI::Core::KeyEventArgs^>(
            [this](Windows::UI::Core::CoreWindow^, Windows::UI::Core::KeyEventArgs^ e) {
                if (inGame_) {
                    // A failed load shows a normal Back button, not an active
                    // game. Let XAML activate it and let B leave this screen.
                    if (loadFailed_) {
                        if (e->VirtualKey == Windows::System::VirtualKey::GamepadB ||
                            e->VirtualKey == Windows::System::VirtualKey::Escape) {
                            ExitGameView();
                            e->Handled = true;
                        }
                        return;
                    }
                    if (quickMenuOpen_) {
                        // The menu's buttons take the controller; B (or Escape)
                        // goes back to the game.
                        if (e->VirtualKey == Windows::System::VirtualKey::GamepadB ||
                            e->VirtualKey == Windows::System::VirtualKey::Escape) {
                            HideQuickMenu();
                            e->Handled = true;
                        }
                        return;
                    }
                    // The controller belongs to the game. View + Menu (read by
                    // the host) opens the quick menu; Escape does on a keyboard.
                    if (e->VirtualKey == Windows::System::VirtualKey::Escape && gameStarted_ && !loadFailed_) {
                        libretro_.SetPaused(true);
                        ShowQuickMenu();
                    }
                    e->Handled = true;
                    return;
                }
                if (libretro_.Running()) {
                    // B belongs to the game, not frontend navigation. Escape
                    // is an explicit desktop stop action while testing a core.
                    if (e->VirtualKey == Windows::System::VirtualKey::Escape) {
                        libretro_.StopRunning();
                        SetStatus(L"Game execution stopped.");
                    }
                    return;
                }
                if (artPicker_ != nullptr) {
                    if (e->VirtualKey == Windows::System::VirtualKey::GamepadB ||
                        e->VirtualKey == Windows::System::VirtualKey::Escape) {
                        PlayCue(2);
                        CloseArtPicker();
                        e->Handled = true;
                    }
                    return;
                }
                if (page_ == 0 && xmb_ != nullptr && xmb_->Visibility == Windows::UI::Xaml::Visibility::Visible) {
                    if (XmbKey(e->VirtualKey)) e->Handled = true;
                    return;
                }
                // A dialog, a drop-down or a flyout is open: it owns the controller.
                if (Windows::UI::Xaml::Media::VisualTreeHelper::GetOpenPopups(Window::Current)->Size > 0) return;
                // LB and RB move between the tabs, from any page.
                if (e->VirtualKey == Windows::System::VirtualKey::GamepadLeftShoulder ||
                    e->VirtualKey == Windows::System::VirtualKey::GamepadRightShoulder) {
                    SwitchTab(e->VirtualKey == Windows::System::VirtualKey::GamepadLeftShoulder ? -1 : 1);
                    e->Handled = true;
                    return;
                }
                // everything else that moves around the menus
                if (HandleNavKey(e->VirtualKey)) e->Handled = true;
            });
    // B on Xbox also raises a system back request; one left unhandled closes
    // the app (B at Home, or B in a game, dropped you to the dashboard). The
    // key handler above does all the going back, so every request is taken.
    Windows::UI::Core::SystemNavigationManager::GetForCurrentView()->BackRequested +=
        ref new EventHandler<Windows::UI::Core::BackRequestedEventArgs^>(
            [](Platform::Object^, Windows::UI::Core::BackRequestedEventArgs^ e) { e->Handled = true; });
    BuildNavigation();
    Render();
    RestorePaths();
    Loaded += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) {
        if (bootstrapped_) return;
        bootstrapped_ = true;
        PrepareCues();
        if (track_ > 0) {
            if (track_ == 5) PlayCustomMusic();
            else PlayBundledMusic(track_);
        }
        ScanGames();
        // Restarted to start a game on a fresh emulator (RestartInto)
        auto pending = Pref(L"pendingBoot", L"");
        if (pending->Length()) {
            PutPref(L"pendingBoot", L"");
            booting_ = true;
            SetStatus(L"Starting " + Pref(L"lastPlayed", L"your game") + L"...");
            StartCore(pending);
        }
    });
}
void CatalogShell::BuildNavigation() {
    // ReVita's frame: the mark, the tabs in a glass dock, and the time, in a
    // bar across the top. The page starts under the bar (it is not laid over
    // the page), so nothing is ever hidden behind it. On Settings a second
    // row of pills under the dock holds its categories.
    // The controller treats the bar as one zone and the page as another:
    // Down from the bar enters the page, Up from the top of the page returns
    // to the bar, B always lands on the bar. See Navigate().
    int keepKey = pendingMenuFocus_;
    pendingMenuFocus_ = -1;
    if (pendingContentFocus_) {
        pendingContentFocus_ = false;
        keepKey = -2;   // the page takes the focus once it is drawn
    } else if (keepKey < 0) {
        keepKey = MenuKeyOf(Windows::UI::Xaml::Input::FocusManager::GetFocusedElement());
    }
    if (keepKey >= 100 && page_ != 2) keepKey = 2;

    menuItems_.clear();
    settingsRail_.clear();
    auto layer = ref new Grid();
    layer->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Top;

    auto header = ref new Grid();
    header->Height = 64;
    header->Margin = Thickness(44, 4, 48, 0);
    header->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Top;
    for (int c = 0; c < 3; ++c) {
        auto column = ref new ColumnDefinition();
        column->Width = c == 1 ? GridLength(1, GridUnitType::Star) : GridLength(1, GridUnitType::Auto);
        header->ColumnDefinitions->Append(column);
    }

    // mark + wordmark
    auto brand = ref new StackPanel();
    brand->Orientation = Orientation::Horizontal;
    brand->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
    brand->IsHitTestVisible = false;
    auto mark = ref new Image();
    mark->Width = 34;
    mark->Height = 34;
    mark->Stretch = Stretch::Uniform;
    mark->Source = ref new BitmapImage(ref new Uri(L"ms-appx:///Assets/ReVitaMark.png"));
    brand->Children->Append(mark);
    auto word = ref new Image();
    word->Height = 20;
    word->Stretch = Stretch::Uniform;
    word->Margin = Thickness(9, 1, 0, 0);
    word->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
    word->Source = ref new BitmapImage(ref new Uri(L"ms-appx:///Assets/ReVitaWordmark.png"));
    brand->Children->Append(word);
    auto badge = ref new Border();
    badge->CornerRadius = Windows::UI::Xaml::CornerRadius(8);
    badge->Padding = Thickness(6, 1, 6, 2);
    badge->Margin = Thickness(9, 2, 0, 0);
    badge->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
    badge->BorderBrush = BrandBrush(0.9);
    badge->BorderThickness = Thickness(1);
    auto badgeText = Text(L"BETA", 9, true);
    badgeText->CharacterSpacing = 140;
    badge->Child = badgeText;
    brand->Children->Append(badge);
    Grid::SetColumn(brand, 0);
    header->Children->Append(brand);

    // the dock
    auto dock = ref new Border();
    dock->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Center;
    dock->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
    dock->Padding = Thickness(6, 5, 6, 5);
    dock->CornerRadius = Windows::UI::Xaml::CornerRadius(28);
    {
        const Color card = Card();
        dock->Background = Solid(Alpha(232, card.R, card.G, card.B));
    }
    dock->BorderBrush = BrandBrush(0.55);
    dock->BorderThickness = Thickness(1);
    auto tabs = ref new StackPanel();
    tabs->Orientation = Orientation::Horizontal;
    auto bumper = [this](const wchar_t* label) {
        auto hint = ref new Border();
        hint->CornerRadius = Windows::UI::Xaml::CornerRadius(7);
        hint->Padding = Thickness(7, 2, 7, 3);
        hint->Margin = Thickness(6, 0, 6, 0);
        hint->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        hint->Background = Solid(Alpha(26, 255, 255, 255));
        hint->IsHitTestVisible = false;
        hint->Child = Text(ref new Platform::String(label), 9, true, true);
        return hint;
    };
    tabs->Children->Append(bumper(L"LB"));
    const wchar_t* glyphs[] = { L"\xE80F", L"\xE8F1", L"\xE945", L"\xE713", L"\xE90F" };
    const wchar_t* labels[] = { L"Home", L"Library", L"Mods", L"Settings", L"Tools" };
    const int tabPage[] = { 0, 1, 5, 2, 3 };
    // Tools holds developer diagnostics. Release builds (VITA3K_RELEASE_BUILD)
    // leave it out; development builds keep it. See docs/RELEASING.md.
#if defined(VITA3K_RELEASE_BUILD)
    constexpr int visibleTabs = 4;
#else
    constexpr int visibleTabs = 5;
#endif
    const Color dim = RgbColor(176, 178, 194);
    for (int tab = 0; tab < visibleTabs; ++tab) {
        const int i = tabPage[tab];
        const bool selected = page_ == i || (i == 1 && page_ == 4);
        auto nav = ref new Windows::UI::Xaml::Controls::Button();
        nav->Margin = Thickness(2, 0, 2, 0);
        nav->Height = 44;
        nav->Padding = Thickness(15, 0, 15, 0);
        nav->BorderThickness = Thickness(0);
        nav->Background = selected ? static_cast<Brush^>(BrandBrush(0.92)) : Solid(Colors::Transparent);
        Skin(nav, 22, selected);
        Windows::UI::Xaml::Automation::AutomationProperties::SetName(nav, ref new Platform::String(labels[tab]));
        auto inside = ref new StackPanel();
        inside->Orientation = Orientation::Horizontal;
        auto symbol = Text(ref new Platform::String(glyphs[tab]), 16, false);
        symbol->FontFamily = ref new Windows::UI::Xaml::Media::FontFamily(L"Segoe MDL2 Assets");
        symbol->Foreground = Solid(selected ? Colors::White : dim);
        symbol->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        inside->Children->Append(symbol);
        // The tab you are on, and the one the controller is on, spell their name.
        auto name = Text(ref new Platform::String(labels[tab]), 14, true);
        name->Foreground = Solid(Colors::White);
        name->Margin = Thickness(9, 0, 0, 0);
        name->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        name->Visibility = selected ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
        inside->Children->Append(name);
        nav->Content = inside;
        nav->GotFocus += ref new RoutedEventHandler([this,name](Platform::Object^, RoutedEventArgs^) {
            PlayCue(0);
            name->Visibility = Windows::UI::Xaml::Visibility::Visible;
        });
        nav->LostFocus += ref new RoutedEventHandler([name,selected](Platform::Object^, RoutedEventArgs^) {
            if (!selected) name->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        });
        nav->Click += ref new RoutedEventHandler([this,i](Platform::Object^, RoutedEventArgs^) {
            PlayCue(1);
            const int shown = page_ == 4 ? 1 : page_;
            if (shown == i) {
                // A on the tab you are on goes into the page
                FocusContent();
                return;
            }
            // switching pages: the controller stays on the dock, so the next
            // press (Down) goes into the new page
            pendingMenuFocus_ = i;
            ShowPage(i);
        });
        menuItems_.push_back({ i, nav });
        tabs->Children->Append(nav);
    }
    tabs->Children->Append(bumper(L"RB"));
    dock->Child = tabs;
    Grid::SetColumn(dock, 1);
    header->Children->Append(dock);

    // the time
    auto time = ref new StackPanel();
    time->Orientation = Orientation::Horizontal;
    time->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
    time->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Right;
    time->IsHitTestVisible = false;
    topDate_ = Text(L"", 10, true, true);
    topDate_->CharacterSpacing = 140;
    topDate_->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
    topDate_->TextWrapping = TextWrapping::NoWrap;
    topDate_->Margin = Thickness(0, 2, 12, 0);
    time->Children->Append(topDate_);
    topClock_ = Text(L"", 22, false);
    topClock_->FontWeight = FontWeights::Light;
    topClock_->FontFamily = ref new Windows::UI::Xaml::Media::FontFamily(L"Bahnschrift");
    topClock_->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
    topClock_->TextWrapping = TextWrapping::NoWrap;
    time->Children->Append(topClock_);
    Grid::SetColumn(time, 2);
    header->Children->Append(time);
    layer->Children->Append(header);

    // Settings: its categories in a second row under the dock. Moving onto one
    // shows it at once; A or Down goes into its options.
    if (page_ == 2) {
        auto sub = ref new Border();
        sub->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Center;
        sub->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Top;
        sub->Margin = Thickness(0, 72, 0, 0);
        sub->Padding = Thickness(6, 4, 6, 4);
        sub->CornerRadius = Windows::UI::Xaml::CornerRadius(24);
        {
            const Color card = Card();
            sub->Background = Solid(Alpha(225, card.R, card.G, card.B));
        }
        sub->BorderBrush = BrandBrush(0.4);
        sub->BorderThickness = Thickness(1);
        auto row = ref new StackPanel();
        row->Orientation = Orientation::Horizontal;
        const wchar_t* groups[] = { L"System", L"Emulation", L"Look", L"Sound", L"Artwork", L"Help" };
        const wchar_t* groupGlyphs[] = { L"\xE7F8", L"\xE7FC", L"\xE790", L"\xE767", L"\xEB9F", L"\xE897" };
        for (int g = 0; g < 6; ++g) {
            auto item = ref new Windows::UI::Xaml::Controls::Button();
            item->Height = 38;
            item->Margin = Thickness(2, 0, 2, 0);
            item->Padding = Thickness(14, 0, 14, 0);
            item->BorderThickness = Thickness(0);
            Skin(item, 19);
            Windows::UI::Xaml::Automation::AutomationProperties::SetName(item, ref new Platform::String(groups[g]));
            auto inside = ref new StackPanel();
            inside->Orientation = Orientation::Horizontal;
            auto symbol = Text(ref new Platform::String(groupGlyphs[g]), 14, false);
            symbol->FontFamily = ref new Windows::UI::Xaml::Media::FontFamily(L"Segoe MDL2 Assets");
            symbol->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
            symbol->Margin = Thickness(0, 1, 9, 0);
            inside->Children->Append(symbol);
            auto label = Text(ref new Platform::String(groups[g]), 13, true);
            label->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
            inside->Children->Append(label);
            item->Content = inside;
            item->GotFocus += ref new RoutedEventHandler([this,g](Platform::Object^, RoutedEventArgs^) {
                PlayCue(0);
                if (settingsTab_ == g) return;
                settingsTab_ = g;
                PutPref(L"settings.tab", StringOf(std::to_wstring(g)));
                StyleSettingsRail();
                Render();   // only the page under the pills is drawn again
            });
            item->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) {
                PlayCue(1);
                FocusContent();
            });
            menuItems_.push_back({ 100 + g, item });
            settingsRail_.push_back(item);
            row->Children->Append(item);
        }
        sub->Child = row;
        layer->Children->Append(sub);
    }
    StyleSettingsRail();

    navigation_->Child = layer;
    // the page begins under the bar
    if (contentScroll_ != nullptr)
        contentScroll_->Margin = Thickness(0, page_ == 2 ? 124 : 70, 0, 0);
    UpdateClock();
    if (keepKey >= 0) FocusMenu(keepKey);
}
void CatalogShell::StyleSettingsRail() {
    const Color dim = RgbColor(176, 178, 194);
    for (int i = 0; i < static_cast<int>(settingsRail_.size()); ++i) {
        const bool on = i == settingsTab_;
        auto item = settingsRail_[i];
        item->Background = on ? static_cast<Brush^>(BrandBrush(0.7)) : Solid(Colors::Transparent);
        item->Foreground = Solid(on ? Colors::White : dim);
        auto inside = dynamic_cast<StackPanel^>(item->Content);
        if (inside == nullptr) continue;
        for (unsigned k = 0; k < inside->Children->Size; ++k) {
            auto label = dynamic_cast<TextBlock^>(inside->Children->GetAt(k));
            if (label != nullptr)
                label->Foreground = Solid(on ? Colors::White : dim);
        }
    }
}
void CatalogShell::FocusSettingsRail() {
    FocusMenu(100 + settingsTab_);
}
int CatalogShell::CurrentMenuKey() const {
    if (page_ == 2) return 100 + std::max(0, std::min(5, settingsTab_));
    if (page_ == 4) return 1;
    return page_;
}
int CatalogShell::MenuKeyOf(Platform::Object^ element) const {
    if (element == nullptr) return -1;
    for (const auto& item : menuItems_)
        if (Platform::Object::ReferenceEquals(item.second, element)) return item.first;
    return -1;
}
void CatalogShell::FocusMenu(int key) {
    Windows::UI::Xaml::Controls::Button^ target = nullptr;
    for (const auto& item : menuItems_)
        if (item.first == key) target = item.second;
    if (target == nullptr) {
        // a category that is not shown: its page instead
        const int page = key >= 100 ? 2 : key;
        for (const auto& item : menuItems_)
            if (item.first == page) target = item.second;
    }
    if (target == nullptr) return;
    if (!target->Focus(Windows::UI::Xaml::FocusState::Keyboard)) {
        // not in the window yet: as soon as it is
        target->Loaded += ref new RoutedEventHandler([this,target](Platform::Object^, RoutedEventArgs^) {
            if (!InWindow(target)) return;
            target->Focus(Windows::UI::Xaml::FocusState::Keyboard);
        });
    }
}
bool CatalogShell::FocusContent() {
    if (content_ == nullptr) return false;
    // Home starts on the poster in the middle
    if (page_ == 0 && flowFocus_ >= 0 && static_cast<size_t>(flowFocus_) < flowCards_.size()
        && flowCards_[flowFocus_]->Focus(Windows::UI::Xaml::FocusState::Keyboard))
        return true;
    std::vector<Control^> stops;
    CollectFocusable(content_, stops);
    for (auto stop : stops)
        if (stop->Focus(Windows::UI::Xaml::FocusState::Keyboard)) {
            EnsureVisible(stop);
            return true;
        }
    return false;
}
void CatalogShell::FocusContentSoon(int attempts, bool fromMenu) {
    // A page that was just drawn is not laid out yet, and a control that is
    // not laid out cannot take focus. Try again shortly instead of giving up,
    // so the controller is never left with nothing to move from.
    // Something already holds the focus: leave it alone. The exception is a
    // call that comes from the dock itself (Down on a tab or a pill): the dock
    // holds the focus, and that is exactly what this call is here to move.
    auto focused = dynamic_cast<DependencyObject^>(Windows::UI::Xaml::Input::FocusManager::GetFocusedElement());
    if (focused != nullptr && InWindow(focused) && !(fromMenu && MenuKeyOf(focused) >= 0)) return;
    if (FocusContent()) return;
    if (attempts <= 0) {
        libretro_.LogLine("nav: no control on this page could take the focus; using the dock");
        FocusMenu(CurrentMenuKey());
        return;
    }
    auto timer = ref new DispatcherTimer();
    timer->Interval = Millis(60);
    timer->Tick += ref new EventHandler<Platform::Object^>([this,timer,attempts](Platform::Object^, Platform::Object^) {
        timer->Stop();
        FocusContentSoon(attempts - 1, fromMenu);
    });
    timer->Start();
}
bool CatalogShell::InWindow(DependencyObject^ element) const {
    // Attached to the window, or left behind by a page that was drawn again?
    if (element == nullptr || root_ == nullptr) return false;
    auto top = [](DependencyObject^ node) {
        for (;;) {
            auto parent = Windows::UI::Xaml::Media::VisualTreeHelper::GetParent(node);
            if (parent == nullptr) return node;
            node = parent;
        }
    };
    return Platform::Object::ReferenceEquals(top(element), top(root_));
}
void CatalogShell::CollectNavStops(DependencyObject^ node, std::vector<Control^>& out) {
    const int count = Windows::UI::Xaml::Media::VisualTreeHelper::GetChildrenCount(node);
    for (int i = 0; i < count; ++i) {
        auto child = Windows::UI::Xaml::Media::VisualTreeHelper::GetChild(node, i);
        auto element = dynamic_cast<UIElement^>(child);
        if (element != nullptr && (element->Visibility != Windows::UI::Xaml::Visibility::Visible || !element->IsHitTestVisible)) continue;
        auto control = dynamic_cast<Control^>(child);
        if (control != nullptr && control->IsTabStop) {
            if (control->IsEnabled && control->ActualWidth > 1 && control->ActualHeight > 1)
                out.push_back(control);
            continue;   // a control's own insides are not separate stops
        }
        CollectNavStops(child, out);
    }
}
namespace {
bool BoundsIn(UIElement^ element, UIElement^ relativeTo, Windows::Foundation::Rect& out) {
    try {
        auto sized = dynamic_cast<FrameworkElement^>(element);
        if (sized == nullptr) return false;
        out = element->TransformToVisual(relativeTo)->TransformBounds(Windows::Foundation::Rect(
            0.0f, 0.0f, static_cast<float>(sized->ActualWidth), static_cast<float>(sized->ActualHeight)));
        return true;
    } catch (Platform::Exception^) {
        return false;
    }
}
std::string NameOf(Control^ control) {
    if (control == nullptr) return "none";
    auto name = Windows::UI::Xaml::Automation::AutomationProperties::GetName(control);
    if (name != nullptr && !name->IsEmpty()) return NarrowUtf8(name->Data());
    auto type = control->GetType();
    return type != nullptr ? NarrowUtf8(type->FullName->Data()) : std::string("?");
}
}
void CatalogShell::EnsureVisible(Control^ control) {
    // Scrolls the page just enough that the focused control, and a little
    // above it (its heading), is on screen.
    if (control == nullptr || contentScroll_ == nullptr || MenuKeyOf(control) >= 0) return;
    Windows::Foundation::Rect box;
    if (!BoundsIn(control, contentScroll_, box)) return;
    const double view = contentScroll_->ViewportHeight;
    const double offset = contentScroll_->VerticalOffset;
    double want = offset;
    if (box.Y < 96.0)
        want = offset + box.Y - 96.0;
    else if (box.Y + box.Height > view - 48.0)
        want = offset + (box.Y + box.Height) - (view - 48.0);
    // near the top of the page: show its heading too
    if (offset + box.Y < 240.0) want = 0.0;
    want = std::max(0.0, std::min(contentScroll_->ScrollableHeight, want));
    if (std::abs(want - offset) > 1.0)
        contentScroll_->ChangeView(nullptr, ref new Platform::Box<double>(want), nullptr, false);
}
bool CatalogShell::Navigate(int dir) {
    // dir: 0 up, 1 down, 2 left, 3 right.
    // The nearest control in that direction wins; controls that line up with
    // the one you are on count for more than ones off to the side.
    static const char* const names[] = { "up", "down", "left", "right" };
    auto current = dynamic_cast<Control^>(Windows::UI::Xaml::Input::FocusManager::GetFocusedElement());
    if (current == nullptr || !InWindow(current)) {
        libretro_.LogLine(std::string("nav: ") + names[dir] + " with nothing focused; entering the page");
        FocusContentSoon(8);
        return true;
    }
    const bool inMenu = MenuKeyOf(current) >= 0;
    Windows::Foundation::Rect from;
    if (!BoundsIn(current, root_, from)) {
        libretro_.LogLine("nav: could not measure " + NameOf(current));
        return true;
    }
    Control^ pick = nullptr;
    // a page may name where a direction goes (Home: Down from Play is the poster in the middle)
    {
        DependencyObject^ hint = dir == 0 ? current->XYFocusUp : dir == 1 ? current->XYFocusDown
            : dir == 2 ? current->XYFocusLeft : current->XYFocusRight;
        auto named = dynamic_cast<Control^>(hint);
        if (named != nullptr && named->IsTabStop && named->IsEnabled &&
            named->Visibility == Windows::UI::Xaml::Visibility::Visible && InWindow(named))
            pick = named;
    }
    // The controls on this side of the screen, in rows. Controls on the same
    // line are one row. Left and Right step along the row one control at a
    // time; Up and Down go to the next row and land on the control closest
    // above or below. Every control is in a row and every row has a
    // neighbour, so nothing can be skipped or left out of reach.
    int considered = 0;
    if (pick == nullptr) {
        struct Stop {
            Control^ control;
            float cx;
            float cy;
            float height;
        };
        std::vector<Control^> found;
        CollectNavStops(root_, found);
        std::vector<Stop> stops;
        for (auto candidate : found) {
            if ((MenuKeyOf(candidate) >= 0) != inMenu) continue;   // the dock and the page are separate
            Windows::Foundation::Rect r;
            if (candidate == current) r = from;
            else if (!BoundsIn(candidate, root_, r)) continue;
            stops.push_back({ candidate, r.X + r.Width / 2.0f, r.Y + r.Height / 2.0f, r.Height });
        }
        considered = static_cast<int>(stops.size());
        std::stable_sort(stops.begin(), stops.end(), [](const Stop& l, const Stop& r) { return l.cy < r.cy; });
        std::vector<std::vector<Stop>> rows;
        for (const auto& stop : stops) {
            if (!rows.empty()) {
                auto& row = rows.back();
                float sum = 0.0f, tallest = 0.0f;
                for (const auto& member : row) {
                    sum += member.cy;
                    tallest = std::max(tallest, member.height);
                }
                if (std::abs(stop.cy - sum / static_cast<float>(row.size())) <= std::max(tallest, stop.height) * 0.5f) {
                    row.push_back(stop);
                    continue;
                }
            }
            rows.push_back(std::vector<Stop>{ stop });
        }
        for (auto& row : rows)
            std::stable_sort(row.begin(), row.end(), [](const Stop& l, const Stop& r) { return l.cx < r.cx; });
        int rowIndex = -1, column = -1;
        for (size_t r = 0; r < rows.size() && rowIndex < 0; ++r)
            for (size_t c = 0; c < rows[r].size(); ++c)
                if (rows[r][c].control == current) { rowIndex = static_cast<int>(r); column = static_cast<int>(c); break; }
        if (rowIndex >= 0) {
            if (dir >= 2) {
                const int next = column + (dir == 3 ? 1 : -1);
                if (next >= 0 && next < static_cast<int>(rows[rowIndex].size()))
                    pick = rows[rowIndex][next].control;
            } else {
                const int next = rowIndex + (dir == 1 ? 1 : -1);
                if (next >= 0 && next < static_cast<int>(rows.size())) {
                    const float fx = from.X + from.Width / 2.0f;
                    float best = 1e9f;
                    for (const auto& stop : rows[next]) {
                        const float d = std::abs(stop.cx - fx);
                        if (d < best) { best = d; pick = stop.control; }
                    }
                }
            }
        }
    }
    if (pick != nullptr) {
        const bool moved = pick->Focus(Windows::UI::Xaml::FocusState::Keyboard);
        if (moved) EnsureVisible(pick);
        libretro_.LogLine(std::string("nav: ") + names[dir] + " " + NameOf(current) + " -> " + NameOf(pick) +
            (moved ? "" : " (could not focus)"));
        return true;
    }

    // Nothing further that way.
    const int menuKey = MenuKeyOf(current);
    if (inMenu) {
        if (dir == 1) {
            const int shown = page_ == 4 ? 1 : page_;
            if (menuKey < 100 && menuKey != shown) {
                // Down from a tab you are not on: open it and go in
                libretro_.LogLine("nav: down from the " + NameOf(current) + " tab opens it");
                pendingContentFocus_ = true;
                ShowPage(menuKey);
                return true;
            }
            libretro_.LogLine("nav: down from " + NameOf(current) + " enters the page");
            FocusContentSoon(8, true);
        }
        return true;   // the dock's ends hold the controller; it never falls off
    }
    if (contentScroll_ != nullptr) {
        const double offset = contentScroll_->VerticalOffset;
        if (dir == 0) {
            if (offset > 1.0) {
                // read what is above the first control before leaving the page
                contentScroll_->ChangeView(nullptr, ref new Platform::Box<double>(std::max(0.0, offset - 260.0)), nullptr, false);
                return true;
            }
            libretro_.LogLine("nav: up from " + NameOf(current) + " goes to the dock");
            FocusMenu(CurrentMenuKey());
            return true;
        }
        if (dir == 1 && offset < contentScroll_->ScrollableHeight - 1.0) {
            contentScroll_->ChangeView(nullptr, ref new Platform::Box<double>(
                std::min(contentScroll_->ScrollableHeight, offset + 260.0)), nullptr, false);
            return true;
        }
    }
    libretro_.LogLine(std::string("nav: ") + names[dir] + " from " + NameOf(current) + " has nowhere to go (" +
        std::to_string(considered) + " controls considered)");
    return true;
}
bool CatalogShell::HandleNavKey(Windows::System::VirtualKey code) {
    using Windows::System::VirtualKey;
    auto focused = Windows::UI::Xaml::Input::FocusManager::GetFocusedElement();
    const bool typing = dynamic_cast<TextBox^>(focused) != nullptr || dynamic_cast<PasswordBox^>(focused) != nullptr;
    const bool adjusting = dynamic_cast<Slider^>(focused) != nullptr;
    // a read-only pane (the log) takes Up and Down to scroll itself, then lets go
    auto pane = dynamic_cast<ScrollViewer^>(focused);
    int dir = -1;
    bool arrowKey = false;
    switch (code) {
    case VirtualKey::GamepadDPadUp: case VirtualKey::GamepadLeftThumbstickUp: dir = 0; break;
    case VirtualKey::GamepadDPadDown: case VirtualKey::GamepadLeftThumbstickDown: dir = 1; break;
    case VirtualKey::GamepadDPadLeft: case VirtualKey::GamepadLeftThumbstickLeft: dir = 2; break;
    case VirtualKey::GamepadDPadRight: case VirtualKey::GamepadLeftThumbstickRight: dir = 3; break;
    case VirtualKey::Up: dir = 0; arrowKey = true; break;
    case VirtualKey::Down: dir = 1; arrowKey = true; break;
    case VirtualKey::Left: dir = 2; arrowKey = true; break;
    case VirtualKey::Right: dir = 3; arrowKey = true; break;
    default: break;
    }
    if (dir >= 0) {
        // a slider takes Left and Right to change its value; a text box takes
        // the keyboard's arrows to move the cursor
        if (dir >= 2 && (adjusting || (typing && arrowKey))) return false;
        if (pane != nullptr && dir <= 1) {
            const double step = dir == 1 ? 180.0 : -180.0;
            const double to = std::max(0.0, std::min(pane->ScrollableHeight, pane->VerticalOffset + step));
            if (std::abs(to - pane->VerticalOffset) > 0.5) {
                pane->ChangeView(nullptr, ref new Platform::Box<double>(to), nullptr, true);
                return true;
            }
        }
        return Navigate(dir);
    }
    switch (code) {
    case VirtualKey::GamepadA: {
        // with nothing focused, A picks up the page instead of doing nothing
        auto control = dynamic_cast<Control^>(focused);
        if (control == nullptr || !InWindow(control)) {
            libretro_.LogLine("nav: A with nothing focused; entering the page");
            FocusContentSoon(8);
            return true;
        }
        return false;
    }
    case VirtualKey::GamepadB:
    case VirtualKey::Escape: {
        PlayCue(2);
        if (MenuKeyOf(focused) < 0) {
            // in the page: a game's page goes back to the Library, everything
            // else goes up to the dock
            if (page_ == 4) { ShowPage(1); return true; }
            libretro_.LogLine("nav: B goes to the dock");
            FocusMenu(CurrentMenuKey());
            return true;
        }
        // on the dock: back Home
        if (page_ != 0) {
            pendingMenuFocus_ = 0;
            ShowPage(0);
        }
        return true;
    }
    case VirtualKey::GamepadLeftTrigger:
    case VirtualKey::GamepadRightTrigger:
    case VirtualKey::PageUp:
    case VirtualKey::PageDown: {
        // a page at a time
        if (contentScroll_ == nullptr) return false;
        const bool up = code == VirtualKey::GamepadLeftTrigger || code == VirtualKey::PageUp;
        const double step = contentScroll_->ViewportHeight * 0.85 * (up ? -1.0 : 1.0);
        const double to = std::max(0.0, std::min(contentScroll_->ScrollableHeight, contentScroll_->VerticalOffset + step));
        contentScroll_->ChangeView(nullptr, ref new Platform::Box<double>(to), nullptr, false);
        return true;
    }
    default:
        return false;
    }
}
void CatalogShell::SwitchTab(int delta) {
    // the dock's order: Home, Library, Mods, Settings, Tools
    static const int order[] = { 0, 1, 5, 2, 3 };
#if defined(VITA3K_RELEASE_BUILD)
    constexpr int tabCount = 4;
#else
    constexpr int tabCount = 5;
#endif
    const int page = page_ == 4 ? 1 : page_;
    int current = 0;
    for (int i = 0; i < tabCount; ++i)
        if (order[i] == page) current = i;
    const int next = (current + delta + tabCount) % tabCount;
    if (next == current) return;
    PlayCue(4);
    // on the dock, the controller follows to the new page's tab; in a page,
    // it lands in the new page
    if (MenuKeyOf(Windows::UI::Xaml::Input::FocusManager::GetFocusedElement()) >= 0)
        pendingMenuFocus_ = order[next];
    ShowPage(order[next]);
}
void CatalogShell::ShowPage(int nextPage) {
#if defined(VITA3K_RELEASE_BUILD)
    if (nextPage == 3) nextPage = 0;
#endif
    if (nextPage < 0 || nextPage > 5) nextPage = 0;
    if (page_ == 3 && nextPage != 3) renderer_.Shutdown();
    if (nextPage != page_) PlayCue(4);
    page_ = nextPage;
    BuildNavigation();
    Render();
}
void CatalogShell::Banner(Platform::String^ eyebrow, Platform::String^ headline,
                          Platform::String^ description) {
    auto body = Vertical();
    body->Margin = Thickness(2, 0, 0, 16);
    auto tag = Text(eyebrow, 10, true);
    tag->CharacterSpacing = 220;
    tag->Foreground = BrandBrush();
    body->Children->Append(tag);
    auto title = Text(headline, 32, false);
    title->FontWeight = FontWeights::Light;
    title->Margin = Thickness(0, 6, 0, 4);
    body->Children->Append(title);
    auto note = Text(description, 13, false, true);
    note->MaxWidth = 640;
    body->Children->Append(note);
    content_->Children->Append(body);
}
void CatalogShell::Section(Platform::String^ label, Platform::String^ note) {
    auto row = Vertical();
    row->Margin = Thickness(2, 18, 0, 12);
    auto heading = Text(label, 17, true);
    row->Children->Append(heading);
    if (note != nullptr && !note->IsEmpty()) {
        auto secondary = Text(note, 12, false, true);
        secondary->Margin = Thickness(0, 3, 0, 0);
        row->Children->Append(secondary);
    }
    content_->Children->Append(row);
}
Platform::String^ CatalogShell::ArtworkUri(size_t index) {
    if (index >= games_.size()) return L"";
    const auto key = games_[index].key;
    const auto file = Pref(L"art.file." + key);
    if (!file->IsEmpty())
        return L"ms-appdata:///local/Artwork/" + file;
    return Pref(L"art.remote." + key);
}
void CatalogShell::ConfigureCover(Border^ cover, size_t index) {
    // Give every title a film-poster-like palette even when offline or when
    // SteamGridDB has no match. Never display made-up third-party game art.
    const Color posterTops[] = {
        RgbColor(99, 34, 63), RgbColor(31, 78, 112),
        RgbColor(40, 92, 91), RgbColor(109, 71, 41),
        RgbColor(70, 50, 113)
    };
    const Color posterBottoms[] = {
        RgbColor(23, 15, 30), RgbColor(14, 22, 35),
        RgbColor(13, 30, 32), RgbColor(33, 19, 16),
        RgbColor(18, 17, 36)
    };
    const auto palette = static_cast<unsigned>(index % 5);
    cover->Background = Gradient(posterTops[palette], posterBottoms[palette]);
    const auto url = ArtworkUri(index);
    if (!url->IsEmpty()) {
        try {
            auto brush = ref new ImageBrush();
            brush->ImageSource = CachedBitmap(url, 300);
            brush->Stretch = Stretch::UniformToFill;
            cover->Background = brush;
        } catch (Platform::Exception^) {
            cover->Background = Gradient(posterTops[palette], posterBottoms[palette]);
        }
    } else if (index < games_.size() && games_[index].icon != nullptr) {
        // Local icon0.png is a fallback only. It is not mislabeled as box art.
        auto bitmap = ref new BitmapImage();
        auto brush = ref new ImageBrush();
        brush->ImageSource = bitmap;
        brush->Stretch = Stretch::Uniform;
        cover->Background = brush;
        auto icon = games_[index].icon;
        create_task(icon->OpenAsync(FileAccessMode::Read)).then(
            [bitmap](task<IRandomAccessStream^> opened) {
                try { bitmap->SetSource(opened.get()); }
                catch (Platform::Exception^) {}
            }, task_continuation_context::use_current());
    }
}

BitmapImage^ CatalogShell::CachedBitmap(Platform::String^ uri, int decodeWidth) {
    const std::wstring key = std::wstring(uri->Data()) + L"|" + std::to_wstring(decodeWidth);
    auto found = bitmapCache_.find(key);
    if (found != bitmapCache_.end()) return found->second;
    if (bitmapCache_.size() > 96) bitmapCache_.clear();
    auto bitmap = ref new BitmapImage();
    // set before the source, so that only this size is ever decoded
    bitmap->DecodePixelWidth = decodeWidth;
    bitmap->DecodePixelType = DecodePixelType::Logical;
    bitmap->UriSource = ref new Uri(uri);
    bitmapCache_[key] = bitmap;
    return bitmap;
}

Button^ CatalogShell::MakeTile(size_t index) {
    // ReVita poster: the art edge to edge, the name over a dark fade at its
    // foot, a ribbon of the brand gradient along the top.
    auto tile = ref new Windows::UI::Xaml::Controls::Button();
    tile->Width = 134;
    tile->Height = 190;
    tile->Margin = Thickness(0, 4, 12, 10);
    tile->Padding = Thickness(0);
    tile->BorderThickness = Thickness(0);
    tile->Background = Solid(Colors::Transparent);
    tile->HorizontalContentAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
    tile->VerticalContentAlignment = Windows::UI::Xaml::VerticalAlignment::Stretch;
    Skin(tile, 14);
    Windows::UI::Xaml::Automation::AutomationProperties::SetName(tile, games_[index].name);

    auto cover = ref new Border();
    cover->CornerRadius = Windows::UI::Xaml::CornerRadius(14);
    cover->BorderBrush = Solid(Alpha(40, 255, 255, 255));
    cover->BorderThickness = Thickness(1);
    ConfigureCover(cover, index);

    auto art = ref new Grid();
    const bool bare = ArtworkUri(index)->IsEmpty() && games_[index].icon == nullptr;
    if (bare) {
        auto mark = ref new Image();
        mark->Source = ref new BitmapImage(ref new Uri(L"ms-appx:///Assets/ReVitaMark.png"));
        mark->Width = 56;
        mark->Height = 56;
        mark->Opacity = 0.3;
        mark->Margin = Thickness(0, 0, 0, 30);
        art->Children->Append(mark);
    }
    auto ribbon = ref new Border();
    ribbon->Height = 3;
    ribbon->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Top;
    ribbon->Margin = Thickness(14, 0, 14, 0);
    ribbon->CornerRadius = Windows::UI::Xaml::CornerRadius(0, 0, 3, 3);
    ribbon->Background = BrandBrush(0.9);
    art->Children->Append(ribbon);
    auto foot = ref new Border();
    foot->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Bottom;
    foot->CornerRadius = Windows::UI::Xaml::CornerRadius(0, 0, 13, 13);
    foot->Padding = Thickness(10, 26, 10, 9);
    {
        auto fade = ref new LinearGradientBrush();
        fade->StartPoint = Point(0.5, 0);
        fade->EndPoint = Point(0.5, 1);
        auto top = ref new GradientStop();
        top->Color = Alpha(0, 0, 0, 0);
        top->Offset = 0;
        auto bottom = ref new GradientStop();
        bottom->Color = Alpha(225, 6, 5, 12);
        bottom->Offset = 0.7;
        fade->GradientStops->Append(top);
        fade->GradientStops->Append(bottom);
        foot->Background = fade;
    }
    auto words = ref new StackPanel();
    auto title = Text(games_[index].name, 12, true);
    title->MaxLines = 2;
    title->LineHeight = 15;
    words->Children->Append(title);
    auto kind = Text(games_[index].sourceType, 9, false, true);
    kind->CharacterSpacing = 70;
    kind->MaxLines = 1;
    kind->TextWrapping = TextWrapping::NoWrap;
    kind->Margin = Thickness(0, 2, 0, 0);
    words->Children->Append(kind);
    foot->Child = words;
    art->Children->Append(foot);
    cover->Child = art;
    tile->Content = cover;

    // Scale, ring and press feedback come from the shared button template.
    tile->GotFocus += ref new RoutedEventHandler([this,index](Platform::Object^, RoutedEventArgs^) {
        PlayCue(0);
        if (page_ == 0) UpdateSpotlight(static_cast<int>(index));
    });
    tile->Click += ref new RoutedEventHandler([this,index](Platform::Object^, RoutedEventArgs^) {
        PlayCue(1);
        selectedGame_ = static_cast<int>(index);
        ShowPage(4);
    });
    return tile;
}
void CatalogShell::MakeShelf(Platform::String^ heading,
    const std::vector<size_t>& items, StackPanel^ target) {
    if (items.empty()) return;
    auto destination = target != nullptr ? target : content_;
    auto label = Text(heading, 17, true);
    label->Margin = Thickness(2, 10, 0, 8);
    destination->Children->Append(label);
    auto shelf = ref new StackPanel();
    shelf->Orientation = Orientation::Horizontal;
    unsigned order = 0;
    for (const auto index : items) {
        auto tile = MakeTile(index);
        shelf->Children->Append(tile);
        Reveal(tile, 140.0 + 38.0 * std::min(order++, 9u), 16);
    }
    auto scroller = ref new ScrollViewer();
    scroller->IsTabStop = false;
    scroller->Height = 212;
    scroller->HorizontalScrollBarVisibility = ScrollBarVisibility::Hidden;
    scroller->HorizontalScrollMode = ScrollMode::Enabled;
    scroller->VerticalScrollBarVisibility = ScrollBarVisibility::Disabled;
    scroller->VerticalScrollMode = ScrollMode::Disabled;
    scroller->Content = shelf;
    auto lastCell = std::make_shared<int>(0);
    scroller->ViewChanged += ref new EventHandler<ScrollViewerViewChangedEventArgs^>(
        [this,lastCell](Platform::Object^ sender, ScrollViewerViewChangedEventArgs^ event) {
            if (event->IsIntermediate) return;
            auto view = dynamic_cast<ScrollViewer^>(sender);
            if (view == nullptr) return;
            const int cell = static_cast<int>(view->HorizontalOffset / 146.0);
            if (cell != *lastCell) {
                *lastCell = cell;
                PlayCue(3);
            }
        });
    destination->Children->Append(scroller);
}
std::vector<size_t> CatalogShell::RecentGames() {
    // Names, newest first, one per line (see NoteRecent).
    std::vector<size_t> found;
    const std::wstring list = Pref(L"recent", L"")->Data();
    size_t start = 0;
    while (start < list.size() && found.size() < 12) {
        size_t end = list.find(L'\n', start);
        if (end == std::wstring::npos) end = list.size();
        const std::wstring name = list.substr(start, end - start);
        start = end + 1;
        if (name.empty()) continue;
        for (size_t i = 0; i < games_.size(); ++i) {
            if (games_[i].name != nullptr && name == games_[i].name->Data()) {
                if (std::find(found.begin(), found.end(), i) == found.end()) found.push_back(i);
                break;
            }
        }
    }
    if (found.empty()) {
        // before this list existed: the game played last
        const auto last = Pref(L"lastPlayed", L"");
        for (size_t i = 0; i < games_.size(); ++i)
            if (!last->IsEmpty() && games_[i].name == last) { found.push_back(i); break; }
    }
    return found;
}
void CatalogShell::NoteRecent(size_t index) {
    if (index >= games_.size() || games_[index].name == nullptr) return;
    const std::wstring name = games_[index].name->Data();
    std::wstring updated = name;
    const std::wstring list = Pref(L"recent", L"")->Data();
    size_t start = 0;
    int kept = 1;
    while (start < list.size() && kept < 12) {
        size_t end = list.find(L'\n', start);
        if (end == std::wstring::npos) end = list.size();
        const std::wstring other = list.substr(start, end - start);
        start = end + 1;
        if (other.empty() || other == name) continue;
        updated += L"\n" + other;
        ++kept;
    }
    PutPref(L"recent", StringOf(updated));
}
Platform::String^ CatalogShell::GameMeta(size_t index) {
    if (index >= games_.size()) return L"";
    std::wstring meta = L"PS VITA";
    const auto id = GameTitleId(index);
    if (!id.empty()) meta += L"   •   " + id;
    if (games_[index].sourceType != nullptr && !games_[index].sourceType->IsEmpty())
        meta += std::wstring(L"   •   ") + games_[index].sourceType->Data();
    if (id.size() == 9 && HasFiles(FirmwareRoot() + L"\\ux0\\app\\" + id))
        meta += L"   •   INSTALLED";
    return StringOf(meta);
}
Windows::UI::Xaml::Controls::Button^ CatalogShell::Chip(Platform::String^ label, bool active) {
    auto chip = ref new Windows::UI::Xaml::Controls::Button();
    chip->Content = label;
    chip->Height = 34;
    chip->Padding = Thickness(16, 0, 16, 1);
    chip->Margin = Thickness(0, 0, 8, 0);
    chip->FontSize = 13;
    chip->FontWeight = active ? FontWeights::SemiBold : FontWeights::Normal;
    chip->BorderThickness = Thickness(1);
    chip->BorderBrush = Solid(active ? Accent() : Alpha(46, 255, 255, 255));
    chip->Background = Solid(active ? Alpha(56, Accent().R, Accent().G, Accent().B) : Alpha(16, 255, 255, 255));
    chip->Foreground = Solid(active ? Colors::White : RgbColor(196, 199, 208));
    Skin(chip, 17);
    chip->GotFocus += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { PlayCue(0); });
    chip->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { PlayCue(1); });
    return chip;
}
void CatalogShell::MakeGrid(const std::vector<size_t>& items, StackPanel^ target) {
    // The whole library at once, in rows that wrap with the screen.
    auto grid = ref new VariableSizedWrapGrid();
    grid->Orientation = Orientation::Horizontal;
    grid->ItemWidth = 146;
    grid->ItemHeight = 206;
    grid->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
    unsigned order = 0;
    for (const auto index : items) {
        auto tile = MakeTile(index);
        grid->Children->Append(tile);
        Reveal(tile, 90.0 + 22.0 * std::min(order++, 14u), 12);
    }
    (target != nullptr ? target : content_)->Children->Append(grid);
}
void CatalogShell::HideSpotlight() {
    if (spotlight_ == nullptr) return;
    spotlight_->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    spotlightArt_->Opacity = 0;
    spotIndex_ = -1;
    spotArtUri_.clear();
}
void CatalogShell::UpdateSpotlight(int index) {
    if (spotlight_ == nullptr) return;
    const bool valid = index >= 0 && static_cast<size_t>(index) < games_.size();
    if (valid && spotKicker_ != nullptr && page_ == 0) {
        // the words over the picture follow the focused game
        const auto recents = RecentGames();
        const bool recent = std::find(recents.begin(), recents.end(), static_cast<size_t>(index)) != recents.end();
        spotKicker_->Text = recent ? L"JUMP BACK IN" : L"IN YOUR COLLECTION";
        if (spotTitle_ != nullptr) spotTitle_->Text = games_[index].name;
        if (spotMeta_ != nullptr) spotMeta_->Text = GameMeta(static_cast<size_t>(index));
    }
    spotIndex_ = valid ? index : -1;

    // The veils follow the theme's page colour; built again only when it changed.
    if (spotlight_->Children->Size == 1 || spotVeilTheme_ != theme_) {
    spotVeilTheme_ = theme_;
    while (spotlight_->Children->Size > 1) spotlight_->Children->RemoveAt(spotlight_->Children->Size - 1);
    const Color page = Bg();
    auto across = ref new LinearGradientBrush();
    across->StartPoint = Point(0, 0.5);
    across->EndPoint = Point(1, 0.5);
    const double sideStops[][2] = { { 0.0, 250 }, { 0.32, 215 }, { 0.62, 90 }, { 1.0, 40 } };
    for (const auto& s : sideStops) {
        auto stop = ref new GradientStop();
        stop->Offset = s[0];
        stop->Color = Alpha(static_cast<unsigned>(s[1]), page.R, page.G, page.B);
        across->GradientStops->Append(stop);
    }
    auto side = ref new Border();
    side->Background = across;
    spotlight_->Children->Append(side);
    auto down = ref new LinearGradientBrush();
    down->StartPoint = Point(0.5, 0);
    down->EndPoint = Point(0.5, 1);
    const double downStops[][2] = { { 0.0, 120 }, { 0.18, 0 }, { 0.5, 30 }, { 0.78, 225 }, { 1.0, 255 } };
    for (const auto& s : downStops) {
        auto stop = ref new GradientStop();
        stop->Offset = s[0];
        stop->Color = Alpha(static_cast<unsigned>(s[1]), page.R, page.G, page.B);
        down->GradientStops->Append(stop);
    }
    auto bottom = ref new Border();
    bottom->Background = down;
    spotlight_->Children->Append(bottom);
    }
    spotlight_->Visibility = Windows::UI::Xaml::Visibility::Visible;

    Platform::String^ uri = valid ? BackdropUri(static_cast<size_t>(index)) : ref new Platform::String(L"");
    if (valid && uri->IsEmpty()) uri = ArtworkUri(static_cast<size_t>(index));
    const std::wstring wanted = uri->Data();
    if (wanted == spotArtUri_) return;
    spotArtUri_ = wanted;
    if (wanted.empty()) {
        spotlightArt_->Opacity = 0;
        return;
    }
    try {
        spotlightArt_->Source = CachedBitmap(uri, 1600);
    } catch (Platform::Exception^) {
        spotlightArt_->Opacity = 0;
        return;
    }
    // a gentle cross-fade (composition-driven; no work per frame on the UI thread)
    auto board = ref new Storyboard();
    auto fade = ref new DoubleAnimation();
    fade->From = 0.0;
    fade->To = 0.92;
    fade->Duration = DurationHelper::FromTimeSpan(Millis(520));
    auto ease = ref new CubicEase();
    ease->EasingMode = EasingMode::EaseOut;
    fade->EasingFunction = ease;
    Storyboard::SetTarget(fade, spotlightArt_);
    Storyboard::SetTargetProperty(fade, L"Opacity");
    board->Children->Append(fade);
    board->Begin();
}
// ── ReVita Home: the cover flow ─────────────────────────────────────────────
// Every game as a poster in one row. The focused poster faces you, larger;
// the others turn toward it and step back, like a shelf seen from its middle.
// Moving left and right glides the row so the focused poster stays centred,
// and its picture fills the screen behind the page.
namespace {
constexpr double kFlowCardWidth = 120;
constexpr double kFlowCardHeight = 164;
constexpr double kFlowStep = 132;   // card plus gap
}
Button^ CatalogShell::MakeFlowCard(size_t index, int position) {
    auto card = ref new Windows::UI::Xaml::Controls::Button();
    card->Width = kFlowCardWidth;
    card->Height = kFlowCardHeight;
    card->Margin = Thickness((kFlowStep - kFlowCardWidth) / 2, 0, (kFlowStep - kFlowCardWidth) / 2, 0);
    card->Padding = Thickness(0);
    card->BorderThickness = Thickness(0);
    card->Background = Solid(Colors::Transparent);
    card->HorizontalContentAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
    card->VerticalContentAlignment = Windows::UI::Xaml::VerticalAlignment::Stretch;
    card->RenderTransformOrigin = Point(0.5, 0.5);
    Skin(card, 14);
    Windows::UI::Xaml::Automation::AutomationProperties::SetName(card, games_[index].name);

    auto poster = ref new Border();
    poster->CornerRadius = Windows::UI::Xaml::CornerRadius(14);
    poster->BorderBrush = Solid(Alpha(46, 255, 255, 255));
    poster->BorderThickness = Thickness(1);
    ConfigureCover(poster, index);
    if (ArtworkUri(index)->IsEmpty() && games_[index].icon == nullptr) {
        auto art = ref new Grid();
        auto r = ref new Image();
        r->Source = ref new BitmapImage(ref new Uri(L"ms-appx:///Assets/ReVitaMark.png"));
        r->Width = 64;
        r->Height = 64;
        r->Opacity = 0.35;
        art->Children->Append(r);
        auto caption = Text(games_[index].name, 12, true);
        caption->Margin = Thickness(10, 0, 10, 12);
        caption->MaxHeight = 44;
        caption->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Bottom;
        art->Children->Append(caption);
        poster->Child = art;
    }
    card->Content = poster;

    // the turn and the step back (animated in FocusFlow)
    auto projection = ref new PlaneProjection();
    card->Projection = projection;
    auto scale = ref new ScaleTransform();
    card->RenderTransform = scale;

    card->GotFocus += ref new RoutedEventHandler([this,position](Platform::Object^, RoutedEventArgs^) {
        PlayCue(3);
        FocusFlow(position, true);
    });
    card->Click += ref new RoutedEventHandler([this,index](Platform::Object^, RoutedEventArgs^) {
        PlayCue(1);
        selectedGame_ = static_cast<int>(index);
        ShowPage(4);
    });
    return card;
}
void CatalogShell::FocusFlow(int position, bool animate) {
    if (position < 0 || static_cast<size_t>(position) >= flowCards_.size()) return;
    flowFocus_ = position;
    auto board = ref new Storyboard();
    auto ease = ref new CubicEase();
    ease->EasingMode = EasingMode::EaseOut;
    const auto duration = DurationHelper::FromTimeSpan(Millis(animate ? 300 : 1));
    auto animateTo = [&](DependencyObject^ target, Platform::String^ path, double to) {
        auto a = ref new DoubleAnimation();
        a->To = to;
        a->Duration = duration;
        a->EasingFunction = ease;
        a->EnableDependentAnimation = true;
        Storyboard::SetTarget(a, target);
        Storyboard::SetTargetProperty(a, path);
        board->Children->Append(a);
    };
    for (size_t i = 0; i < flowCards_.size(); ++i) {
        const int d = static_cast<int>(i) - position;
        const int reach = std::min(std::abs(d), 4);
        // only the cards near the middle move; the rest are off screen
        if (std::abs(d) > 7) continue;
        auto card = flowCards_[i];
        auto projection = dynamic_cast<PlaneProjection^>(card->Projection);
        auto scale = dynamic_cast<ScaleTransform^>(card->RenderTransform);
        const double turn = d == 0 ? 0.0 : (d < 0 ? 42.0 : -42.0);
        const double size = d == 0 ? 1.12 : 0.9 - 0.04 * (reach - 1);
        const double depth = d == 0 ? 30.0 : -40.0 * reach;
        if (projection != nullptr) {
            animateTo(projection, L"RotationY", turn);
            animateTo(projection, L"GlobalOffsetZ", depth);
        }
        if (scale != nullptr) {
            animateTo(scale, L"ScaleX", size);
            animateTo(scale, L"ScaleY", size);
        }
        animateTo(card, L"Opacity", d == 0 ? 1.0 : 0.9 - 0.16 * reach);
        Canvas::SetZIndex(card, 100 - std::abs(d));
    }
    // Down from Play, Game page or Library lands on the poster in the middle,
    // and Up from any poster goes back to Play
    for (auto action : homeActions_)
        action->XYFocusDown = flowCards_[position];
    if (!homeActions_.empty())
        for (auto card : flowCards_)
            card->XYFocusUp = homeActions_.front();
    board->Begin();
    // keep the focused card in the middle: the row has half a screen of
    // padding on each side, so the focused card's offset is position * step
    if (flowScroll_ != nullptr)
        flowScroll_->ChangeView(ref new Platform::Box<double>(position * kFlowStep), nullptr, nullptr, !animate);
    if (static_cast<size_t>(position) < flowItems_.size())
        UpdateSpotlight(static_cast<int>(flowItems_[position]));
}
void CatalogShell::MakeFlow(const std::vector<size_t>& items, size_t focus) {
    flowCards_.clear();
    flowItems_ = items;
    flowFocus_ = -1;
    flowRow_ = ref new StackPanel();
    flowRow_->Orientation = Orientation::Horizontal;
    flowRow_->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
    int start = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        auto card = MakeFlowCard(items[i], static_cast<int>(i));
        flowCards_.push_back(card);
        flowRow_->Children->Append(card);
        if (items[i] == focus) start = static_cast<int>(i);
    }
    flowScroll_ = ref new ScrollViewer();
    flowScroll_->IsTabStop = false;
    flowScroll_->Height = kFlowCardHeight * 1.22;
    flowScroll_->Margin = Thickness(-56, 2, -56, 0);   // edge to edge
    flowScroll_->HorizontalScrollBarVisibility = ScrollBarVisibility::Hidden;
    flowScroll_->HorizontalScrollMode = ScrollMode::Enabled;
    flowScroll_->VerticalScrollBarVisibility = ScrollBarVisibility::Disabled;
    flowScroll_->VerticalScrollMode = ScrollMode::Disabled;
    // the gamepad moves focus; the row follows it (FocusFlow), not the other way round
    flowScroll_->IsHorizontalRailEnabled = true;
    flowScroll_->Content = flowRow_;
    auto row = flowRow_;
    auto scroller = flowScroll_;
    flowScroll_->SizeChanged += ref new SizeChangedEventHandler(
        [this,row,scroller](Platform::Object^, SizeChangedEventArgs^ e) {
            const double side = std::max(0.0, e->NewSize.Width / 2 - kFlowStep / 2);
            row->Margin = Thickness(side, 0, side, 0);
            if (flowFocus_ >= 0) scroller->ChangeView(ref new Platform::Box<double>(flowFocus_ * kFlowStep), nullptr, nullptr, true);
        });
    content_->Children->Append(flowScroll_);
    FocusFlow(start, false);
}
void CatalogShell::Home() {
    const bool hasGames = !games_.empty();
    const auto recents = RecentGames();
    const size_t featured = !recents.empty() ? recents.front() : 0;

    if (!hasGames) {
        HideSpotlight();
        flowCards_.clear();
        flowScroll_ = nullptr;
        auto welcome = Vertical();
        welcome->Margin = Thickness(2, 48, 0, 26);
        auto mark = ref new Image();
        mark->Source = ref new BitmapImage(ref new Uri(L"ms-appx:///Assets/ReVitaMark.png"));
        mark->Width = 96;
        mark->Height = 96;
        mark->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
        welcome->Children->Append(mark);
        auto kicker = Text(L"WELCOME TO REVITA", 11, true);
        kicker->CharacterSpacing = 260;
        kicker->Foreground = BrandBrush();
        kicker->Margin = Thickness(0, 22, 0, 0);
        welcome->Children->Append(kicker);
        auto headline = Text(L"Bring your Vita\nback to life.", 54, false);
        headline->FontWeight = FontWeights::Light;
        headline->FontFamily = ref new Windows::UI::Xaml::Media::FontFamily(L"Bahnschrift");
        headline->Margin = Thickness(0, 10, 0, 12);
        welcome->Children->Append(headline);
        auto detail = Text(L"Your PS Vita collection on the big screen. Add a game or a folder to begin.", 15, false, true);
        detail->MaxWidth = 560;
        welcome->Children->Append(detail);
        auto controls = ref new StackPanel();
        controls->Orientation = Orientation::Horizontal;
        controls->Margin = Thickness(0, 24, 0, 0);
        auto add = Button(L"Add a game", true);
        add->MinWidth = 150;
        add->Height = 48;
        add->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { ChooseGameFile(); });
        controls->Children->Append(add);
        auto folder = Button(L"Choose a folder");
        folder->Height = 48;
        folder->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { ChooseGameFolder(); });
        controls->Children->Append(folder);
        welcome->Children->Append(controls);
        content_->Children->Append(welcome);
        SetupCard();
        return;
    }

    // The focused game's name and actions, over its own picture.
    auto spot = Vertical();
    spot->Margin = Thickness(2, 6, 0, 2);
    spot->MaxWidth = 720;
    spot->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
    spotKicker_ = Text(recents.empty() ? L"IN YOUR COLLECTION" : L"JUMP BACK IN", 11, true);
    spotKicker_->CharacterSpacing = 260;
    spotKicker_->Foreground = BrandBrush();
    spot->Children->Append(spotKicker_);
    spotTitle_ = Text(games_[featured].name, 34, false);
    spotTitle_->FontFamily = ref new Windows::UI::Xaml::Media::FontFamily(L"Bahnschrift");
    spotTitle_->FontWeight = FontWeights::SemiLight;
    spotTitle_->MaxLines = 1;
    spotTitle_->TextWrapping = TextWrapping::NoWrap;
    spotTitle_->Margin = Thickness(0, 4, 0, 2);
    spot->Children->Append(spotTitle_);
    spotMeta_ = Text(GameMeta(featured), 11, true, true);
    spotMeta_->CharacterSpacing = 90;
    spot->Children->Append(spotMeta_);
    auto controls = ref new StackPanel();
    controls->Orientation = Orientation::Horizontal;
    controls->Margin = Thickness(0, 12, 0, 0);
    auto play = Button(L"Play", true);
    play->MinWidth = 150;
    play->Height = 42;
    play->FontSize = 16;
    play->Background = BrandBrush();
    play->Foreground = Solid(Colors::White);
    {
        auto inside = ref new StackPanel();
        inside->Orientation = Orientation::Horizontal;
        auto glyph = ref new TextBlock();
        glyph->Text = L"\xE768";
        glyph->FontFamily = ref new Windows::UI::Xaml::Media::FontFamily(L"Segoe MDL2 Assets");
        glyph->FontSize = 14;
        glyph->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        glyph->Margin = Thickness(0, 1, 10, 0);
        inside->Children->Append(glyph);
        auto label = ref new TextBlock();
        label->Text = L"Play";
        label->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        inside->Children->Append(label);
        play->Content = inside;
    }
    // Play and the game page act on whichever poster is in the middle.
    play->Click += ref new RoutedEventHandler([this,featured](Platform::Object^, RoutedEventArgs^) {
        AttemptBoot(spotIndex_ >= 0 ? static_cast<size_t>(spotIndex_) : featured);
    });
    controls->Children->Append(play);
    auto details = Button(L"Game page");
    details->Height = 42;
    details->Click += ref new RoutedEventHandler([this,featured](Platform::Object^, RoutedEventArgs^) {
        selectedGame_ = spotIndex_ >= 0 ? spotIndex_ : static_cast<int>(featured);
        ShowPage(4);
    });
    controls->Children->Append(details);
    auto all = Button(L"Library");
    all->Height = 42;
    all->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { ShowPage(1); });
    controls->Children->Append(all);
    homeActions_ = { play, details, all };
    spot->Children->Append(controls);
    content_->Children->Append(spot);

    SetupCard();

    // The flow: played games first (newest first), then the rest A-Z.
    std::vector<size_t> order(recents.begin(), recents.end());
    std::vector<size_t> rest;
    for (size_t i = 0; i < games_.size(); ++i)
        if (std::find(order.begin(), order.end(), i) == order.end()) rest.push_back(i);
    std::sort(rest.begin(), rest.end(), [this](size_t a, size_t b) {
        return _wcsicmp(games_[a].name ? games_[a].name->Data() : L"", games_[b].name ? games_[b].name->Data() : L"") < 0;
    });
    order.insert(order.end(), rest.begin(), rest.end());
    MakeFlow(order, featured);



}
void CatalogShell::Library() {
    auto header = ref new Grid();
    header->Margin = Thickness(2, 10, 0, 16);
    auto heading = Vertical();
    auto eyebrow = Text(L"COLLECTION", 11, true);
    eyebrow->CharacterSpacing = 260;
    eyebrow->Foreground = BrandBrush();
    heading->Children->Append(eyebrow);
    auto title = Text(L"Library", 40, false);
    title->FontWeight = FontWeights::Light;
    title->Margin = Thickness(0, 4, 0, 0);
    heading->Children->Append(title);
    header->Children->Append(heading);
    wchar_t count[64] = {};
    swprintf_s(count, L"%zu %s", games_.size(), games_.size() == 1 ? L"GAME" : L"GAMES");
    auto total = Text(ref new Platform::String(count), 12, true, true);
    total->CharacterSpacing = 160;
    total->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Right;
    total->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Bottom;
    total->Margin = Thickness(0, 0, 0, 8);
    header->Children->Append(total);
    content_->Children->Append(header);

    // The actions first, then search, sorting and what to show. Every row
    // fits the screen, so nothing scrolls sideways and every button can be
    // reached with the D-pad.
    auto actions = ref new StackPanel();
    actions->Orientation = Orientation::Horizontal;
    actions->Margin = Thickness(0, 0, 0, 12);
    auto add = Button(L"Add a game", true);
    add->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { ChooseGameFile(); });
    actions->Children->Append(add);
    auto folder = Button(L"Choose folder");
    folder->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { ChooseGameFolder(); });
    actions->Children->Append(folder);
    auto refresh = Button(L"Rescan");
    refresh->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) {
        SetStatus(L"Rescanning your game folder...");
        ScanGames();
    });
    actions->Children->Append(refresh);
    auto covers = Button(L"Find cover art");
    covers->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { FetchMissingArtwork(); });
    actions->Children->Append(covers);
    content_->Children->Append(actions);

    // where the games come from, so a lost folder is plain to see
    libraryFolder_ = Text(ref new Platform::String(StorageApplicationPermissions::FutureAccessList->ContainsItem(L"vita-games")
        ? L"Game folder: checking..." : L"No game folder chosen yet. Choose folder picks the one your games are in."), 12, false, true);
    libraryFolder_->Margin = Thickness(2, 0, 0, 12);
    libraryFolder_->TextWrapping = TextWrapping::Wrap;
    content_->Children->Append(libraryFolder_);
    if (StorageApplicationPermissions::FutureAccessList->ContainsItem(L"vita-games")) {
        auto label = libraryFolder_;
        create_task(StorageApplicationPermissions::FutureAccessList->GetFolderAsync(L"vita-games")).then(
            [label](task<StorageFolder^> work) {
                try {
                    label->Text = L"Game folder: " + work.get()->Path;
                } catch (Platform::Exception^) {
                    label->Text = L"The game folder can't be opened any more. Choose folder to pick it again.";
                }
            }, task_continuation_context::use_current());
    }

    auto tools = ref new StackPanel();
    tools->Orientation = Orientation::Horizontal;
    tools->Margin = Thickness(0, 0, 0, 10);
    searchBox_ = ref new TextBox();
    searchBox_->PlaceholderText = L"Search your games";
    searchBox_->Text = search_;
    searchBox_->Width = 300;
    searchBox_->Height = 38;
    searchBox_->FontSize = 14;
    searchBox_->Padding = Thickness(14, 7, 10, 7);
    searchBox_->Margin = Thickness(0, 0, 14, 0);
    searchBox_->Background = Solid(Card());
    searchBox_->Foreground = Solid(Colors::White);
    searchBox_->BorderThickness = Thickness(1);
    searchBox_->BorderBrush = Solid(Alpha(40, 255, 255, 255));
    searchBox_->TextChanged += ref new TextChangedEventHandler(
        [this](Platform::Object^, TextChangedEventArgs^) {
            search_ = searchBox_->Text;
            RefreshLibraryItems(); // Only replace results; keep typing focus.
        });
    tools->Children->Append(searchBox_);
    const wchar_t* sorts[] = { L"A–Z", L"Recently played" };
    for (int i = 0; i < 2; ++i) {
        auto chip = Chip(ref new Platform::String(sorts[i]), librarySort_ == i);
        chip->Click += ref new RoutedEventHandler([this,i](Platform::Object^, RoutedEventArgs^) {
            librarySort_ = i;
            PutPref(L"library.sort", i ? L"1" : L"0");
            Render();
        });
        tools->Children->Append(chip);
    }
    content_->Children->Append(tools);

    auto filtersRow = ref new StackPanel();
    filtersRow->Orientation = Orientation::Horizontal;
    filtersRow->Margin = Thickness(0, 0, 0, 4);
    const wchar_t* filters[] = { L"All", L"Folders", L"Archives & packages" };
    for (int i = 0; i < 3; ++i) {
        auto chip = Chip(ref new Platform::String(filters[i]), libraryFilter_ == i);
        chip->Click += ref new RoutedEventHandler([this,i](Platform::Object^, RoutedEventArgs^) {
            libraryFilter_ = i;
            PutPref(L"library.filter", StringOf(std::to_wstring(i)));
            Render();
        });
        filtersRow->Children->Append(chip);
    }
    content_->Children->Append(filtersRow);

    libraryResults_ = Vertical();
    libraryResults_->Margin = Thickness(0, 10, 0, 0);
    content_->Children->Append(libraryResults_);
    RefreshLibraryItems();
}
void CatalogShell::RefreshLibraryItems() {
    if (libraryResults_ == nullptr) return;
    libraryResults_->Children->Clear();
    if (games_.empty()) {
        auto message = Text(L"No titles yet. Add a VPK, ZIP, PKG, VCI or an extracted game folder.", 14, false, true);
        message->Margin = Thickness(2, 16, 0, 0);
        libraryResults_->Children->Append(message);
        return;
    }
    std::vector<size_t> matches;
    for (size_t i = 0; i < games_.size(); ++i) {
        if (!ContainsIgnoreCase(games_[i].name, search_)) continue;
        const bool isFolder = games_[i].format == vita3k_uwp::GameFormat::GameFolder;
        if (libraryFilter_ == 1 && !isFolder) continue;
        if (libraryFilter_ == 2 && isFolder) continue;
        matches.push_back(i);
    }
    std::sort(matches.begin(), matches.end(), [this](size_t a, size_t b) {
        return _wcsicmp(games_[a].name ? games_[a].name->Data() : L"", games_[b].name ? games_[b].name->Data() : L"") < 0;
    });
    if (librarySort_ == 1) {
        // played ones first, newest first; the rest stay A-Z after them
        const auto recents = RecentGames();
        std::stable_sort(matches.begin(), matches.end(), [&recents](size_t a, size_t b) {
            const auto ra = std::find(recents.begin(), recents.end(), a);
            const auto rb = std::find(recents.begin(), recents.end(), b);
            return ra < rb;
        });
    }
    if (matches.empty()) {
        auto none = Text(L"No games match.", 14, false, true);
        none->Margin = Thickness(2, 16, 0, 0);
        libraryResults_->Children->Append(none);
        return;
    }
    MakeGrid(matches, libraryResults_);
}
void CatalogShell::GameDetails() {
    if (selectedGame_ < 0 || static_cast<size_t>(selectedGame_) >= games_.size()) {
        ShowPage(1);
        return;
    }
    const size_t index = static_cast<size_t>(selectedGame_);
    // the game's own picture fills the screen behind its page
    UpdateSpotlight(selectedGame_);
    auto back = Button(L"‹  Library");
    back->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { ShowPage(1); });
    back->Margin = Thickness(0, 4, 0, 26);
    back->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
    content_->Children->Append(back);

    auto layout = ref new Grid();
    layout->Margin = Thickness(2, 0, 0, 10);
    auto artColumn = ref new ColumnDefinition();
    artColumn->Width = GridLength(250);
    auto infoColumn = ref new ColumnDefinition();
    infoColumn->Width = GridLength(1, GridUnitType::Star);
    layout->ColumnDefinitions->Append(artColumn);
    layout->ColumnDefinitions->Append(infoColumn);

    auto cover = ref new Border();
    cover->Width = 222;
    cover->Height = 296;
    cover->CornerRadius = Windows::UI::Xaml::CornerRadius(14);
    cover->BorderBrush = Solid(Alpha(40, 255, 255, 255));
    cover->BorderThickness = Thickness(1);
    cover->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
    cover->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Top;
    ConfigureCover(cover, index);
    if (ArtworkUri(index)->IsEmpty() && games_[index].icon == nullptr) {
        auto fallback = Vertical();
        fallback->Margin = Thickness(16, 14, 16, 20);
        fallback->Children->Append(Text(L"PS VITA", 10, true));
        auto emblem = Text(L"V", 130, true);
        emblem->FontFamily = ref new Windows::UI::Xaml::Media::FontFamily(L"Bahnschrift");
        emblem->Opacity = 0.21;
        emblem->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Center;
        fallback->Children->Append(emblem);
        fallback->Children->Append(Text(games_[index].name, 14, true));
        cover->Child = fallback;
    }
    layout->Children->Append(cover);

    auto right = Vertical();
    right->Margin = Thickness(26, 6, 6, 4);
    Grid::SetColumn(right, 1);
    auto category = Text(GameMeta(index), 11, true);
    category->CharacterSpacing = 140;
    category->Foreground = BrandBrush();
    right->Children->Append(category);
    auto title = Text(games_[index].name, 46, false);
    title->FontWeight = FontWeights::Light;
    title->LineHeight = 52;
    title->Margin = Thickness(0, 8, 0, 12);
    title->MaxWidth = 760;
    title->MaxLines = 2;
    right->Children->Append(title);
    std::wstring titleId;
    if (games_[index].directory) titleId = games_[index].directory->Name->Data();
    const bool installed = titleId.size() == 9 && HasFiles(FirmwareRoot() + L"\\ux0\\app\\" + titleId);
    auto note = Text(installed
        ? L"Installed and ready to play."
        : L"The first start installs the game into ReVita's data folder; after that it starts straight away.",
        14, false, true);
    note->MaxWidth = 620;
    right->Children->Append(note);

    auto actions = ref new StackPanel();
    actions->Orientation = Orientation::Horizontal;
    actions->Margin = Thickness(0, 26, 0, 8);
    auto play = Button(L"Play", true);
    play->MinWidth = 176;
    play->Height = 54;
    play->FontSize = 18;
    {
        auto inside = ref new StackPanel();
        inside->Orientation = Orientation::Horizontal;
        auto glyph = ref new TextBlock();
        glyph->Text = L"";
        glyph->FontFamily = ref new Windows::UI::Xaml::Media::FontFamily(L"Segoe MDL2 Assets");
        glyph->FontSize = 16;
        glyph->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        glyph->Margin = Thickness(0, 1, 10, 0);
        inside->Children->Append(glyph);
        auto label = ref new TextBlock();
        label->Text = L"Play";
        label->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        inside->Children->Append(label);
        play->Content = inside;
    }
    play->Click += ref new RoutedEventHandler([this,index](Platform::Object^, RoutedEventArgs^) {
        AttemptBoot(index);
    });
    actions->Children->Append(play);
    auto image = Button(L"Change cover");
    image->Height = 54;
    image->Click += ref new RoutedEventHandler([this,index](Platform::Object^, RoutedEventArgs^) {
        ChooseCustomArtwork(index);
    });
    actions->Children->Append(image);
    auto find = Button(L"Choose art online");
    find->Height = 54;
    find->Click += ref new RoutedEventHandler([this,index](Platform::Object^, RoutedEventArgs^) {
        ShowArtPicker(index);
    });
    actions->Children->Append(find);
    auto mods = Button(L"Cheats & patches");
    mods->Height = 54;
    mods->Click += ref new RoutedEventHandler([this,index](Platform::Object^, RoutedEventArgs^) {
        modsGame_ = static_cast<int>(index);
        ShowPage(5);
    });
    actions->Children->Append(mods);
    right->Children->Append(actions);
    if (games_[index].format == vita3k_uwp::GameFormat::Pkg) {
        auto license = Button(L"Choose the game's license file");
        license->Margin = Thickness(0, 10, 0, 0);
        license->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
        license->Click += ref new RoutedEventHandler([this,index](Platform::Object^, RoutedEventArgs^) {
            ChooseLicenseForGame(index);
        });
        right->Children->Append(license);
    }
#if !defined(VITA3K_RELEASE_BUILD)
    auto inspect = Button(L"Inspect game (developer)");
    inspect->Margin = Thickness(0, 10, 0, 0);
    inspect->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
    inspect->Click += ref new RoutedEventHandler([this,index](Platform::Object^, RoutedEventArgs^) {
        InspectLaunch(index);
    });
    right->Children->Append(inspect);
    auto validation = Text(L"Source check: " + games_[index].validation, 11, false, true);
    validation->MaxWidth = 620;
    validation->Margin = Thickness(0, 8, 0, 4);
    validation->Opacity = 0.7;
    right->Children->Append(validation);
#endif
    layout->Children->Append(right);
    content_->Children->Append(layout);
    AppendGameInfo(index);

}
// ── Mods: cheats and patches ────────────────────────────────────────────────
// Cheats come from the VitaCheat database (one .psv file per title ID) and run
// through the core's cheat engine; `_V1` marks a cheat that is on when the game
// starts, `_V0` one that is off. Patches come from Vita3K's patch list
// (patchlist.xml); the ones turned on are listed in patch_config.xml, by the
// same key the core uses. All of it lives in the core's data folder.
namespace {
const wchar_t* kCheatDatabase = L"https://raw.githubusercontent.com/r0ah/vitacheat/master/db/";
const wchar_t* kPatchList = L"https://github.com/Vita3K/vita3k-patches/releases/download/latest/patchlist.xml";

struct CheatCard {
    std::wstring name;
    bool on = false;
    size_t lines = 0;
};
struct CheatFileInfo {
    bool present = false;
    std::wstring title, author, note;
    std::vector<CheatCard> cheats;
};
struct PatchCard {
    std::wstring title, name, author, note, patchVer, appVer;
    std::string key;
    bool on = false;
};

std::wstring CheatFilePath(const std::wstring& id) { return FirmwareRoot() + L"\\cheats\\" + id + L".psv"; }
std::wstring PatchDir() { return FirmwareRoot() + L"\\patch"; }

bool ReadAll(const std::wstring& path, std::string& out) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file) return false;
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}
bool WriteAll(const std::wstring& path, const std::string& data) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    const std::wstring temp = path + L".tmp";
    {
        std::ofstream file(std::filesystem::path(temp), std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!file) return false;
    }
    std::filesystem::rename(std::filesystem::path(temp), std::filesystem::path(path), ec);
    if (ec) {
        std::filesystem::remove(std::filesystem::path(path), ec);
        std::filesystem::rename(std::filesystem::path(temp), std::filesystem::path(path), ec);
    }
    return !ec;
}
Platform::String^ XmlText(const std::string& utf8) {
    std::wstring text = WidenUtf8(utf8);
    if (!text.empty() && text[0] == 0xFEFF) text.erase(0, 1);
    if (text.compare(0, 5, L"<?xml") == 0) {
        const auto end = text.find(L"?>");
        if (end != std::wstring::npos) text.erase(0, end + 2);
    }
    return StringOf(text);
}
std::wstring Trim(const std::wstring& s) {
    const auto a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    const auto b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}
// each line with where it starts in the file
std::vector<std::pair<size_t, std::string>> Lines(const std::string& text) {
    std::vector<std::pair<size_t, std::string>> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.emplace_back(start, line);
        if (end == text.size()) break;
        start = end + 1;
    }
    return lines;
}
bool IsCheatMarker(const std::string& line) {
    return line.size() >= 3 && line[0] == '_' && (line[1] == 'V' || line[1] == 'v') && (line[2] == '0' || line[2] == '1');
}
CheatFileInfo ReadCheats(const std::wstring& id) {
    CheatFileInfo info;
    std::string text;
    if (!ReadAll(CheatFilePath(id), text)) return info;
    info.present = true;
    for (const auto& [offset, line] : Lines(text)) {
        (void)offset;
        if (!line.empty() && line[0] == '#') {
            const std::wstring w = WidenUtf8(line.substr(1));
            const auto colon = w.find(L':');
            if (colon == std::wstring::npos) continue;
            const std::wstring field = Trim(w.substr(0, colon));
            const std::wstring value = Trim(w.substr(colon + 1));
            if (field == L"Title") info.title = value;
            else if (field == L"Code Author" || field == L"Author") info.author = value;
            else if (field == L"Note") info.note = value;
        } else if (IsCheatMarker(line)) {
            CheatCard card;
            card.on = line[2] == '1';
            card.name = Trim(WidenUtf8(line.substr(3)));
            if (card.name.empty()) card.name = L"Unnamed cheat";
            info.cheats.push_back(card);
        } else if (!line.empty() && line[0] == '$' && !info.cheats.empty()) {
            info.cheats.back().lines++;
        }
    }
    return info;
}
std::set<std::string> ReadEnabledPatches() {
    std::set<std::string> keys;
    std::string text;
    if (!ReadAll(PatchDir() + L"\\patch_config.xml", text) || text.empty()) return keys;
    try {
        auto doc = ref new Windows::Data::Xml::Dom::XmlDocument();
        doc->LoadXml(XmlText(text));
        auto nodes = doc->GetElementsByTagName(L"Enabled");
        for (unsigned i = 0; i < nodes->Length; ++i) {
            auto element = dynamic_cast<Windows::Data::Xml::Dom::XmlElement^>(nodes->Item(i));
            if (element != nullptr) keys.insert(NarrowUtf8(element->GetAttribute(L"Key")->Data()));
        }
    } catch (Platform::Exception^) {}
    return keys;
}
bool WriteEnabledPatches(const std::set<std::string>& keys) {
    auto escape = [](const std::string& s) {
        std::string out;
        for (char c : s) {
            switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c; break;
            }
        }
        return out;
    };
    std::string xml = "<?xml version=\"1.0\"?>\n<PatchConfig>\n";
    for (const auto& key : keys) xml += "\t<Enabled Key=\"" + escape(key) + "\" />\n";
    xml += "</PatchConfig>\n";
    return WriteAll(PatchDir() + L"\\patch_config.xml", xml);
}
std::vector<PatchCard> ReadPatches(const std::wstring& id, bool& listPresent) {
    std::vector<PatchCard> cards;
    std::string text;
    listPresent = ReadAll(PatchDir() + L"\\patchlist.xml", text) && !text.empty();
    if (!listPresent) return cards;
    const auto enabled = ReadEnabledPatches();
    try {
        auto doc = ref new Windows::Data::Xml::Dom::XmlDocument();
        doc->LoadXml(XmlText(text));
        auto root = doc->DocumentElement;
        if (root == nullptr) return cards;
        std::vector<std::wstring> ids;
        for (unsigned i = 0; i < root->ChildNodes->Length; ++i) {
            auto element = dynamic_cast<Windows::Data::Xml::Dom::XmlElement^>(root->ChildNodes->Item(i));
            if (element == nullptr) continue;
            const std::wstring tag = element->TagName->Data();
            if (tag == L"TitleID") {
                ids.clear();
                auto idNodes = element->GetElementsByTagName(L"ID");
                for (unsigned k = 0; k < idNodes->Length; ++k)
                    ids.push_back(Trim(idNodes->Item(k)->InnerText->Data()));
                continue;
            }
            if (tag != L"Metadata" || std::find(ids.begin(), ids.end(), id) == ids.end()) continue;
            PatchCard card;
            card.title = element->GetAttribute(L"Title")->Data();
            card.name = element->GetAttribute(L"Name")->Data();
            card.author = element->GetAttribute(L"Author")->Data();
            card.note = element->GetAttribute(L"Note")->Data();
            card.patchVer = element->GetAttribute(L"PatchVer")->Data();
            card.appVer = element->GetAttribute(L"AppVer")->Data();
            std::wstring elf = element->GetAttribute(L"AppElf")->Data();
            if (elf.empty()) elf = L"eboot.bin";
            const std::wstring nid = element->GetAttribute(L"NID")->Data();
            // the core's key: first title ID | AppVer | AppElf | NID | Name
            card.key = NarrowUtf8(ids.front() + L"|" + card.appVer + L"|" + elf + L"|" + nid + L"|" + card.name);
            card.on = enabled.count(card.key) != 0;
            cards.push_back(card);
        }
    } catch (Platform::Exception^) {}
    return cards;
}
// A newer cheat file keeps the cheats the player had on.
std::string MergeCheatStates(const std::string& fresh, const std::string& old) {
    std::set<std::string> on;
    for (const auto& [offset, line] : Lines(old)) {
        (void)offset;
        if (IsCheatMarker(line) && line[2] == '1') on.insert(line.substr(3));
    }
    std::string out = fresh;
    for (const auto& [offset, line] : Lines(fresh)) {
        if (IsCheatMarker(line) && on.count(line.substr(3)) != 0) out[offset + 2] = '1';
    }
    return out;
}
} // namespace

void CatalogShell::ToggleCheat(const std::wstring& titleId, size_t cheatIndex) {
    std::string text;
    if (!ReadAll(CheatFilePath(titleId), text)) return;
    size_t seen = 0;
    for (const auto& [offset, line] : Lines(text)) {
        if (!IsCheatMarker(line)) continue;
        if (seen++ == cheatIndex) {
            text[offset + 2] = line[2] == '1' ? '0' : '1';
            break;
        }
    }
    modsMessage_ = WriteAll(CheatFilePath(titleId), text)
        ? L"Saved. Cheats apply from the next time the game starts."
        : L"Couldn't save the cheat file.";
    Render();
}

void CatalogShell::TogglePatch(const std::string& key) {
    auto keys = ReadEnabledPatches();
    if (keys.count(key)) keys.erase(key);
    else keys.insert(key);
    modsMessage_ = WriteEnabledPatches(keys)
        ? L"Saved. Patches apply from the next time the game starts."
        : L"Couldn't save the patch settings.";
    Render();
}

void CatalogShell::FetchMods(size_t index, bool force) {
    if (modsBusy_ || index >= games_.size()) return;
    const std::wstring id = GameTitleId(index);
    if (id.empty()) {
        modsMessage_ = L"This game has no title ID, so it can't be matched with a database.";
        Render();
        return;
    }
    modsBusy_ = true;
    modsMessage_ = L"Getting the latest cheats and patches...";
    Render();
    auto client = ref new Windows::Web::Http::HttpClient();
    client->DefaultRequestHeaders->UserAgent->TryParseAdd(L"ReVita/1.0 (Xbox)");
    auto result = std::make_shared<std::wstring>();
    std::error_code existsError;
    const bool getCheats = force || !std::filesystem::exists(std::filesystem::path(CheatFilePath(id)), existsError);
    const bool getPatches = force || !std::filesystem::exists(std::filesystem::path(PatchDir() + L"\\patchlist.xml"), existsError);

    task<void> chain = task_from_result();
    if (getCheats) {
        chain = chain.then([client, id, result]() {
            return create_task(client->GetAsync(ref new Uri(StringOf(std::wstring(kCheatDatabase) + id + L".psv"))))
            .then([id, result](Windows::Web::Http::HttpResponseMessage^ response) {
                if (response->StatusCode == Windows::Web::Http::HttpStatusCode::NotFound) {
                    *result += L"The cheat database has nothing for this game yet. ";
                    return task_from_result();
                }
                response->EnsureSuccessStatusCode();
                return create_task(response->Content->ReadAsStringAsync()).then([id, result](Platform::String^ body) {
                    std::string fresh = NarrowUtf8(body->Data());
                    std::string old;
                    if (ReadAll(CheatFilePath(id), old)) fresh = MergeCheatStates(fresh, old);
                    *result += WriteAll(CheatFilePath(id), fresh) ? L"Cheats updated. " : L"Couldn't save the cheats. ";
                });
            });
        }).then([result](task<void> done) {
            try { done.get(); }
            catch (Platform::Exception^ e) { *result += L"Couldn't reach the cheat database (" + std::wstring(e->Message->Data()) + L"). "; }
        });
    }
    if (getPatches) {
        chain = chain.then([client, result]() {
            return create_task(client->GetStringAsync(ref new Uri(StringOf(kPatchList))))
            .then([result](Platform::String^ body) {
                *result += WriteAll(PatchDir() + L"\\patchlist.xml", NarrowUtf8(body->Data()))
                    ? L"Patch list updated." : L"Couldn't save the patch list.";
            });
        }).then([result](task<void> done) {
            try { done.get(); }
            catch (Platform::Exception^ e) { *result += L"Couldn't reach the patch list (" + std::wstring(e->Message->Data()) + L")."; }
        });
    }
    chain.then([this, result](task<void> done) {
        try { done.get(); } catch (...) {}
        modsBusy_ = false;
        modsMessage_ = result->empty() ? L"Already up to date." : Trim(*result);
        if (page_ == 5) Render();
    }, task_continuation_context::use_current());
}

void CatalogShell::Mods() {
    {
        auto heading = Vertical();
        heading->Margin = Thickness(2, 10, 0, 12);
        auto eyebrow = Text(L"MODS & CHEATS", 11, true);
        eyebrow->CharacterSpacing = 260;
        eyebrow->Foreground = BrandBrush();
        heading->Children->Append(eyebrow);
        auto title = Text(L"Make every game yours.", 40, false);
        title->FontWeight = FontWeights::Light;
        title->Margin = Thickness(0, 4, 0, 4);
        heading->Children->Append(title);
        auto note = Text(L"Cheats from the VitaCheat database and patches from Vita3K's patch list, for the games you have. Turn them on here; they apply when the game starts.", 13, false, true);
        note->MaxWidth = 720;
        heading->Children->Append(note);
        content_->Children->Append(heading);
    }
    // games that can be matched (they need a title ID)
    std::vector<size_t> matchable;
    for (size_t i = 0; i < games_.size(); ++i)
        if (!GameTitleId(i).empty()) matchable.push_back(i);
    if (matchable.empty()) {
        HideSpotlight();
        auto none = Text(L"Add a game first. Cheats and patches are found by the game's title ID.", 14, false, true);
        none->Margin = Thickness(2, 16, 0, 0);
        content_->Children->Append(none);
        return;
    }
    if (modsGame_ < 0 || std::find(matchable.begin(), matchable.end(), static_cast<size_t>(modsGame_)) == matchable.end()) {
        const auto recents = RecentGames();
        modsGame_ = static_cast<int>(matchable.front());
        for (auto r : recents)
            if (std::find(matchable.begin(), matchable.end(), r) != matchable.end()) { modsGame_ = static_cast<int>(r); break; }
    }
    const size_t index = static_cast<size_t>(modsGame_);
    const std::wstring id = GameTitleId(index);
    UpdateSpotlight(modsGame_);

    // the games, as small posters; A picks one
    auto strip = ref new StackPanel();
    strip->Orientation = Orientation::Horizontal;
    Windows::UI::Xaml::Controls::Button^ current = nullptr;
    for (const auto i : matchable) {
        auto pick = ref new Windows::UI::Xaml::Controls::Button();
        pick->Width = 76;
        pick->Height = 104;
        pick->Margin = Thickness(0, 4, 10, 6);
        pick->Padding = Thickness(0);
        pick->BorderThickness = Thickness(i == index ? 2 : 0);
        pick->BorderBrush = BrandBrush();
        pick->Background = Solid(Colors::Transparent);
        pick->HorizontalContentAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        pick->VerticalContentAlignment = Windows::UI::Xaml::VerticalAlignment::Stretch;
        Skin(pick, 10);
        Windows::UI::Xaml::Automation::AutomationProperties::SetName(pick, games_[i].name);
        auto poster = ref new Border();
        poster->CornerRadius = Windows::UI::Xaml::CornerRadius(10);
        ConfigureCover(poster, i);
        poster->Opacity = i == index ? 1.0 : 0.7;
        pick->Content = poster;
        pick->Click += ref new RoutedEventHandler([this, i](Platform::Object^, RoutedEventArgs^) {
            PlayCue(1);
            modsGame_ = static_cast<int>(i);
            modsMessage_.clear();
            Render();
        });
        if (i == index) current = pick;
        strip->Children->Append(pick);
    }
    auto stripScroll = ref new ScrollViewer();
    stripScroll->IsTabStop = false;
    stripScroll->HorizontalScrollBarVisibility = ScrollBarVisibility::Hidden;
    stripScroll->HorizontalScrollMode = ScrollMode::Enabled;
    stripScroll->VerticalScrollBarVisibility = ScrollBarVisibility::Disabled;
    stripScroll->VerticalScrollMode = ScrollMode::Disabled;
    stripScroll->Content = strip;
    content_->Children->Append(stripScroll);

    // the chosen game, and the button that fetches the latest
    auto gameRow = Vertical();
    gameRow->Margin = Thickness(2, 10, 0, 6);
    auto name = Text(games_[index].name, 26, false);
    name->FontWeight = FontWeights::Light;
    gameRow->Children->Append(name);
    auto meta = Text(StringOf(id), 11, true, true);
    meta->CharacterSpacing = 120;
    gameRow->Children->Append(meta);
    auto actions = ref new StackPanel();
    actions->Orientation = Orientation::Horizontal;
    actions->Margin = Thickness(0, 12, 0, 0);
    auto latest = Button(modsBusy_ ? L"Downloading..." : L"Get the latest from the internet", true);
    latest->IsEnabled = !modsBusy_;
    latest->Click += ref new RoutedEventHandler([this, index](Platform::Object^, RoutedEventArgs^) { FetchMods(index, true); });
    actions->Children->Append(latest);
    gameRow->Children->Append(actions);
    if (!modsMessage_.empty()) {
        auto status = Text(StringOf(modsMessage_), 12, false, true);
        status->Margin = Thickness(0, 8, 0, 0);
        gameRow->Children->Append(status);
    }
    content_->Children->Append(gameRow);

    // A card for each cheat or patch: the game's cover, the name, who made it,
    // what it does, and a switch.
    auto card = [this, index](Platform::String^ title, Platform::String^ detail, Platform::String^ note,
                              bool on, std::function<void()> toggle) {
        auto button = ref new Windows::UI::Xaml::Controls::Button();
        button->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        button->HorizontalContentAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        button->Margin = Thickness(0, 4, 0, 4);
        button->Padding = Thickness(10);
        button->MaxWidth = 860;
        button->BorderThickness = Thickness(1);
        button->BorderBrush = on ? static_cast<Brush^>(BrandBrush(0.7)) : Solid(Alpha(30, 255, 255, 255));
        button->Background = Solid(Card());
        Skin(button, 16);
        Windows::UI::Xaml::Automation::AutomationProperties::SetName(button, title);
        auto grid = ref new Grid();
        auto c0 = ref new ColumnDefinition(); c0->Width = GridLength(56);
        auto c1 = ref new ColumnDefinition(); c1->Width = GridLength(1, GridUnitType::Star);
        auto c2 = ref new ColumnDefinition(); c2->Width = GridLength(1, GridUnitType::Auto);
        grid->ColumnDefinitions->Append(c0);
        grid->ColumnDefinitions->Append(c1);
        grid->ColumnDefinitions->Append(c2);
        auto thumb = ref new Border();
        thumb->Width = 52;
        thumb->Height = 70;
        thumb->CornerRadius = Windows::UI::Xaml::CornerRadius(8);
        thumb->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Top;
        ConfigureCover(thumb, index);
        grid->Children->Append(thumb);
        auto words = Vertical();
        words->Margin = Thickness(12, 2, 12, 2);
        auto heading = Text(title, 15, true);
        heading->MaxLines = 2;
        words->Children->Append(heading);
        auto line = Text(detail, 11, true, true);
        line->CharacterSpacing = 60;
        line->Margin = Thickness(0, 3, 0, 0);
        words->Children->Append(line);
        if (note != nullptr && !note->IsEmpty()) {
            auto about = Text(note, 12, false, true);
            about->Margin = Thickness(0, 5, 0, 0);
            about->MaxLines = 3;
            words->Children->Append(about);
        }
        Grid::SetColumn(words, 1);
        grid->Children->Append(words);
        auto pill = ref new Border();
        pill->CornerRadius = Windows::UI::Xaml::CornerRadius(14);
        pill->Padding = Thickness(14, 5, 14, 6);
        pill->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        pill->Background = on ? static_cast<Brush^>(BrandBrush()) : Solid(Alpha(24, 255, 255, 255));
        pill->Child = Text(on ? L"ON" : L"OFF", 11, true, !on);
        Grid::SetColumn(pill, 2);
        grid->Children->Append(pill);
        button->Content = grid;
        button->GotFocus += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { PlayCue(0); });
        button->Click += ref new RoutedEventHandler([this, toggle](Platform::Object^, RoutedEventArgs^) {
            PlayCue(1);
            toggle();
        });
        return button;
    };

    const CheatFileInfo cheats = ReadCheats(id);
    Section(L"Cheats", cheats.present
        ? StringOf(L"From the VitaCheat database" + (cheats.author.empty() ? std::wstring() : L", codes by " + cheats.author) +
                   L". A cheat that is ON runs from the moment the game starts.")
        : StringOf(std::wstring(L"Nothing downloaded for this game yet.")));
    if (cheats.present && cheats.cheats.empty()) {
        content_->Children->Append(Text(L"This game's cheat file has no cheats in it.", 13, false, true));
    }
    for (size_t c = 0; c < cheats.cheats.size(); ++c) {
        const auto& cheat = cheats.cheats[c];
        wchar_t detail[96] = {};
        swprintf_s(detail, L"CHEAT   \x2022   %zu CODE LINE%s", cheat.lines, cheat.lines == 1 ? L"" : L"S");
        const std::wstring titleId = id;
        content_->Children->Append(card(StringOf(cheat.name), ref new Platform::String(detail),
            c == 0 && !cheats.note.empty() ? StringOf(cheats.note) : static_cast<Platform::String^>(nullptr), cheat.on,
            [this, titleId, c]() { ToggleCheat(titleId, c); }));
    }

    bool listPresent = false;
    const auto patches = ReadPatches(id, listPresent);
    Section(L"Patches", listPresent
        ? StringOf(std::wstring(L"From Vita3K's patch list: fixes and improvements like higher frame rates or resolution. Turn one on and start the game."))
        : StringOf(std::wstring(L"The patch list hasn't been downloaded yet.")));
    if (listPresent && patches.empty()) {
        content_->Children->Append(Text(L"No patches in the list for this game yet.", 13, false, true));
    }
    for (const auto& patch : patches) {
        std::wstring detail = L"PATCH";
        if (!patch.patchVer.empty()) detail += L"   \x2022   V" + patch.patchVer;
        if (!patch.author.empty()) detail += L"   \x2022   BY " + patch.author;
        if (!patch.appVer.empty()) detail += L"   \x2022   GAME VERSION " + patch.appVer;
        const std::string key = patch.key;
        content_->Children->Append(card(StringOf(patch.name.empty() ? patch.title : patch.name), StringOf(detail),
            StringOf(patch.note), patch.on, [this, key]() { TogglePatch(key); }));
    }

    // the first visit to a game gets its cheats and the patch list by itself
    static std::set<std::wstring> fetched;
    if (!modsBusy_ && (!cheats.present || !listPresent) && fetched.insert(id).second) {
        Dispatcher->RunAsync(Windows::UI::Core::CoreDispatcherPriority::Low,
            ref new Windows::UI::Core::DispatchedHandler([this, index]() { FetchMods(index, false); }));
    }

}

std::wstring CatalogShell::GameTitleId(size_t index) const {
    const auto& game = games_[index];
    std::wstring id;
    if (game.directory) id = vita3k_uwp::FindTitleId(game.directory->Name->Data());
    if (id.empty() && game.file) id = vita3k_uwp::FindTitleId(game.file->Name->Data());
    if (id.empty() && game.key) id = vita3k_uwp::FindTitleId(game.key->Data());
    return id;
}
std::wstring CatalogShell::GameInfoKey(size_t index) const {
    const auto id = GameTitleId(index);
    if (!id.empty()) return id;
    std::wstring name = games_[index].name ? games_[index].name->Data() : L"";
    std::wstring key;
    for (wchar_t c : name)
        if (iswalnum(c)) key.push_back(static_cast<wchar_t>(towlower(c)));
    return key.empty() ? L"unknown" : key.substr(0, 60);
}
void CatalogShell::FetchGameInfoFor(size_t index, bool force) {
    if (index >= games_.size()) return;
    const auto key = GameInfoKey(index);
    if (infoFetching_.count(key)) return;
    vita3k_uwp::GameInfo cached;
    bool stale = false;
    if (!force && vita3k_uwp::LoadGameInfo(key, cached, stale) && !stale) return;
    infoFetching_.insert(key);
    const std::wstring title = games_[index].name ? games_[index].name->Data() : L"";
    const auto titleId = GameTitleId(index);
    const int shownGame = static_cast<int>(index);
    vita3k_uwp::FetchGameInfo(title, titleId).then([this, key, shownGame](vita3k_uwp::GameInfo info) {
        infoFetching_.erase(key);
        // Nothing found (offline, say) keeps what was there before.
        vita3k_uwp::GameInfo previous;
        bool oldStale = false;
        if (!info.Empty() || !vita3k_uwp::LoadGameInfo(key, previous, oldStale)) vita3k_uwp::SaveGameInfo(key, info);
        if (page_ == 4 && selectedGame_ == shownGame) Render();
    }, concurrency::task_continuation_context::use_current());
}
void CatalogShell::AppendGameInfo(size_t index) {
    const auto key = GameInfoKey(index);
    vita3k_uwp::GameInfo info;
    bool stale = false;
    const bool have = vita3k_uwp::LoadGameInfo(key, info, stale);
    if (!have || stale) FetchGameInfoFor(index, false);
    auto S = [](const std::wstring& s) { return ref new Platform::String(s.c_str()); };

    auto body = Vertical();
    auto eyebrow = Text(L"ABOUT THIS GAME", 10, true);
    eyebrow->CharacterSpacing = 160;
    eyebrow->Foreground = BrandBrush();
    body->Children->Append(eyebrow);

    if (!have) {
        auto waiting = Text(infoFetching_.count(key) ? L"Looking this game up online\u2026" : L"Nothing found online for this game yet.", 14, false, true);
        waiting->Margin = Thickness(0, 10, 0, 0);
        body->Children->Append(waiting);
    } else if (info.Empty()) {
        auto none = Text(L"Nothing was found online for this game. Its name may differ from the one Wikipedia uses.", 14, false, true);
        none->Margin = Thickness(0, 10, 0, 0);
        none->MaxWidth = 900;
        body->Children->Append(none);
    } else {
        if (!info.description.empty()) {
            auto about = Text(S(info.description), 15);
            about->Margin = Thickness(0, 10, 0, 4);
            about->MaxWidth = 960;
            about->MaxLines = 7;
            about->LineHeight = 23;
            body->Children->Append(about);
        }
        // Who made it and when.
        std::vector<std::pair<std::wstring, std::wstring>> facts;
        if (!info.developer.empty()) facts.emplace_back(L"DEVELOPER", info.developer);
        if (!info.publisher.empty() && info.publisher != info.developer) facts.emplace_back(L"PUBLISHER", info.publisher);
        if (!info.released.empty()) facts.emplace_back(L"RELEASED", info.released);
        if (!info.genre.empty()) facts.emplace_back(L"GENRE", info.genre);
        if (!facts.empty()) {
            auto row = ref new StackPanel();
            row->Orientation = Orientation::Horizontal;
            row->Margin = Thickness(0, 14, 0, 0);
            for (const auto& fact : facts) {
                auto cell = Vertical();
                cell->Margin = Thickness(0, 0, 34, 10);
                cell->MaxWidth = 280;
                auto label = Text(S(fact.first), 10, true, true);
                label->CharacterSpacing = 120;
                cell->Children->Append(label);
                auto value = Text(S(fact.second), 14, true);
                value->Margin = Thickness(0, 3, 0, 0);
                cell->Children->Append(value);
                row->Children->Append(cell);
            }
            body->Children->Append(row);
        }
        // What critics gave it, and how it runs in Vita3K.
        auto chips = ref new StackPanel();
        chips->Orientation = Orientation::Horizontal;
        chips->Margin = Thickness(0, 10, 0, 0);
        auto chip = [&](const std::wstring& top, const std::wstring& bottom, Color tint) {
            auto box = ref new Border();
            box->CornerRadius = Windows::UI::Xaml::CornerRadius(12);
            box->Padding = Thickness(14, 9, 16, 10);
            box->Margin = Thickness(0, 0, 10, 10);
            box->Background = Solid(tint);
            box->BorderBrush = Solid(Alpha(30, 255, 255, 255));
            box->BorderThickness = Thickness(1);
            auto stack = Vertical();
            auto a = Text(S(top), 10, true, true);
            a->CharacterSpacing = 80;
            stack->Children->Append(a);
            auto b = Text(S(bottom), 18, true);
            b->Margin = Thickness(0, 2, 0, 0);
            stack->Children->Append(b);
            box->Child = stack;
            chips->Children->Append(box);
        };
        for (const auto& score : info.scores) chip(score.first, score.second, Alpha(40, 255, 255, 255));
        if (!info.compat.empty()) {
            const bool good = info.compat == L"Playable" || info.compat == L"Ingame +";
            const bool bad = info.compat == L"Nothing" || info.compat == L"Bootable" || info.compat == L"Intro";
            chip(L"VITA3K COMPATIBILITY", info.compat,
                 good ? Alpha(70, 60, 190, 120) : bad ? Alpha(70, 210, 80, 80) : Alpha(70, 220, 170, 60));
        }
        if (chips->Children->Size) body->Children->Append(chips);
        if (!info.compat.empty()) {
            auto meaning = Text(S(vita3k_uwp::DescribeCompat(info.compat) +
                L" (reported for Vita3K on PC; the Xbox version can differ)"), 12, false, true);
            meaning->MaxWidth = 900;
            body->Children->Append(meaning);
        }
    }

    auto footer = ref new StackPanel();
    footer->Orientation = Orientation::Horizontal;
    footer->Margin = Thickness(0, 14, 0, 0);
    auto source = Text(L"From Wikipedia, Wikidata and the Vita3K compatibility list.", 11, false, true);
    source->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
    source->Opacity = 0.75;
    footer->Children->Append(source);
    auto refresh = Button(infoFetching_.count(key) ? L"Looking up\u2026" : L"Refresh info");
    refresh->Margin = Thickness(16, 0, 0, 0);
    refresh->IsEnabled = !infoFetching_.count(key);
    refresh->Click += ref new RoutedEventHandler([this, index](Platform::Object^, RoutedEventArgs^) {
        FetchGameInfoFor(index, true);
        Render();
    });
    footer->Children->Append(refresh);
    body->Children->Append(footer);
    auto panel = Panel(body, 24);
    panel->Margin = Thickness(0, 14, 0, 10);
    content_->Children->Append(panel);
}
void CatalogShell::CollectFocusable(DependencyObject^ node, std::vector<Control^>& out) {
    if (node == nullptr) return;
    auto element = dynamic_cast<UIElement^>(node);
    if (element != nullptr && (element->Visibility != Windows::UI::Xaml::Visibility::Visible || !element->IsHitTestVisible)) return;
    auto control = dynamic_cast<Control^>(node);
    if (control != nullptr) {
        if (!control->IsEnabled) return;
        if (control->IsTabStop) {
            out.push_back(control);
            return; // a control's template is not a second set of navigation stops
        }
    }
    const int count = Windows::UI::Xaml::Media::VisualTreeHelper::GetChildrenCount(node);
    for (int i = 0; i < count; ++i)
        CollectFocusable(Windows::UI::Xaml::Media::VisualTreeHelper::GetChild(node, i), out);
}
void CatalogShell::Render() {
    const unsigned revision = ++navigationRevision_;
    // Settings keeps its category list on the left; its page sits beside it
    if (content_ != nullptr)
        content_->Margin = Thickness(56, 10, 56, 28);
    // Drawing the same page again (a toggle, a theme, a sort) keeps the
    // controller on the control it was on instead of throwing it to the top.
    int keepFocus = -1;
    if (page_ == shownPage_ && content_ != nullptr) {
        auto focused = dynamic_cast<DependencyObject^>(Windows::UI::Xaml::Input::FocusManager::GetFocusedElement());
        if (focused != nullptr) {
            std::vector<Control^> stops;
            CollectFocusable(content_, stops);
            for (size_t i = 0; i < stops.size(); ++i)
                if (stops[i] == focused) { keepFocus = static_cast<int>(i); break; }
        }
    }
    const double keepOffset = contentScroll_ != nullptr ? contentScroll_->VerticalOffset : 0.0;
    content_->Children->Clear();
    clock_ = nullptr;
    greeting_ = nullptr;
    musicStatus_ = nullptr;
    libraryResults_ = nullptr;
    libraryFolder_ = nullptr;
    spotKicker_ = nullptr;
    spotTitle_ = nullptr;
    spotMeta_ = nullptr;
    flowCards_.clear();
    flowScroll_ = nullptr;
    flowRow_ = nullptr;
    flowFocus_ = -1;
    homeActions_.clear();
    // the spotlight belongs to Home and a game's page
    if (page_ != 0 && page_ != 4 && page_ != 5) HideSpotlight();
    // The PS3-style home takes the whole screen over the usual layout.
    if (page_ == 0 && xmbMode_) {
        if (!inGame_) ShowXmb();
        shownPage_ = page_;
        return;
    }
    if (xmb_ != nullptr && xmb_->Visibility == Windows::UI::Xaml::Visibility::Visible) HideXmb();
    switch(page_) {
    case 1: Library(); break;
    case 2: Settings(); break;
    case 3: Diagnostics(); break;
    case 4: GameDetails(); break;
    case 5: Mods(); break;
    default: Home(); break;
    }
    // Each page settles in: its blocks rise and fade in one after another.
    // Re-renders on the same page (theme change, toggles) only fade, so the
    // layout never jumps while someone is working in it.
    const bool newPage = page_ != shownPage_;
    shownPage_ = page_;
    unsigned order = 0;
    for (auto child : content_->Children) {
        Reveal(child, 45.0 * std::min(order++, 8u), newPage ? 18 : 0);
    }
    // The controller always has somewhere to be: if the page that was just
    // drawn took the focused control with it, the new page's first control
    // takes over (tried again for a moment while the page is laid out).
    Dispatcher->RunAsync(Windows::UI::Core::CoreDispatcherPriority::Low,
        ref new Windows::UI::Core::DispatchedHandler([this,revision,keepFocus,keepOffset]() {
            if (revision != navigationRevision_ || inGame_ || artPicker_ != nullptr || content_ == nullptr) return;
            auto active = dynamic_cast<DependencyObject^>(Windows::UI::Xaml::Input::FocusManager::GetFocusedElement());
            if (InWindow(active)) return; // the user already moved since this render
            if (keepFocus >= 0) {
                if (contentScroll_ != nullptr)
                    contentScroll_->ChangeView(nullptr, ref new Platform::Box<double>(keepOffset), nullptr, true);
                std::vector<Control^> stops;
                CollectFocusable(content_, stops);
                if (!stops.empty() &&
                    stops[std::min(static_cast<size_t>(keepFocus), stops.size() - 1)]->Focus(Windows::UI::Xaml::FocusState::Keyboard))
                    return;
            }
            if (xmb_ != nullptr && xmb_->Visibility == Windows::UI::Xaml::Visibility::Visible) return;
            FocusContentSoon(8);
        }));
}

void CatalogShell::Settings() {
    {
        auto heading = Vertical();
        heading->Margin = Thickness(2, 10, 0, 16);
        auto eyebrow = Text(L"SETTINGS", 11, true);
        eyebrow->CharacterSpacing = 260;
        eyebrow->Foreground = BrandBrush();
        heading->Children->Append(eyebrow);
        auto title = Text(L"Set up once. Play anywhere.", 40, false);
        title->FontWeight = FontWeights::Light;
        title->Margin = Thickness(0, 4, 0, 0);
        heading->Children->Append(title);
        content_->Children->Append(heading);
    }
    if (settingsTab_ == 0) {
        FirmwareSettings();
        StorageSettings();
    }
    if (settingsTab_ == 1) EmulationSettings();
    if (settingsTab_ == 2) {
    Section(L"Appearance", L"Eight moods in ReVita's neon colours, each with its own living scene and its own song.");
    const wchar_t* styles[] = {
        L"ReVita", L"Synthwave", L"Miami Night", L"Pastel Dream",
        L"Ultraviolet", L"Cotton Sky", L"Midnight Arcade", L"Starfall"
    };
    const wchar_t* captions[] = {
        L"Violet night", L"Neon sun, endless grid", L"Cyan tide, hot pink",
        L"Lavender haze", L"Violet aurora", L"Blue and pink clouds",
        L"Navy glass, neon light", L"Planets and stars"
    };
    const Color samples[] = {
        RgbColor(92, 64, 200), RgbColor(150, 36, 160), RgbColor(30, 150, 190), RgbColor(190, 120, 180),
        RgbColor(110, 50, 200), RgbColor(90, 140, 210), RgbColor(200, 50, 140), RgbColor(110, 100, 200)
    };
    auto palettes = ref new VariableSizedWrapGrid();
    palettes->Orientation = Orientation::Horizontal;
    palettes->ItemWidth = 158;
    palettes->ItemHeight = 96;
    for (int i = 0; i < 8; ++i) {
        const bool active = theme_ == i;
        auto choice = ref new Windows::UI::Xaml::Controls::Button();
        choice->Width = 148;
        choice->Height = 84;
        choice->Margin = Thickness(0, 4, 10, 8);
        choice->Padding = Thickness(0);
        choice->BorderThickness = Thickness(active ? 2 : 1);
        choice->BorderBrush = Solid(active ? Accent() : Alpha(34, 255, 255, 255));
        choice->HorizontalContentAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        choice->VerticalContentAlignment = Windows::UI::Xaml::VerticalAlignment::Stretch;
        Skin(choice, 14);
        auto swatch = ref new Border();
        swatch->Background = Gradient(RgbColor(23, 23, 29), samples[i]);
        swatch->Padding = Thickness(12);
        swatch->CornerRadius = Windows::UI::Xaml::CornerRadius(13);
        auto caption = Vertical();
        caption->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Bottom;
        auto name = Text(ref new Platform::String(styles[i]), 15, true);
        caption->Children->Append(name);
        auto sub = Text(ref new Platform::String(captions[i]), 11, false, true);
        sub->Foreground = Solid(RgbColor(214, 212, 225));
        sub->Margin = Thickness(0, 4, 0, 0);
        caption->Children->Append(sub);
        swatch->Child = caption;
        choice->Content = swatch;
        choice->GotFocus += ref new RoutedEventHandler(
            [this](Platform::Object^, RoutedEventArgs^) { PlayCue(0); });
        choice->Click += ref new RoutedEventHandler(
            [this,i](Platform::Object^, RoutedEventArgs^) { SaveTheme(i); });
        palettes->Children->Append(choice);
    }
    content_->Children->Append(palettes);

    Section(L"Home screen", L"The cinematic home, or the PlayStation 3's cross media bar.");
    auto layouts = ActionRow(200);
    auto cinematic = Button(L"Cinematic", !xmbMode_);
    cinematic->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { SaveLayout(false); });
    layouts->Children->Append(cinematic);
    auto xmb = Button(L"PS3 XMB", xmbMode_);
    xmb->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { SaveLayout(true); });
    layouts->Children->Append(xmb);
    content_->Children->Append(layouts);
    auto layoutNote = Text(L"XMB: left and right change category, up and down choose, A opens, Y shows a game's page. "
        L"Pick its colour below.", 12, false, true);
    layoutNote->Margin = Thickness(3, 6, 0, 8);
    content_->Children->Append(layoutNote);
    Section(L"XMB colour", L"The PS3's colour of the month, or one of your own all year.");
    auto xmbColours = ref new VariableSizedWrapGrid();
    xmbColours->Orientation = Orientation::Horizontal;
    xmbColours->ItemWidth = 156;
    xmbColours->ItemHeight = 70;
    for (int i = 0; i < kXmbThemeCount; ++i) {
        const XmbTheme& theme = kXmbThemes[i];
        auto choice = ref new Windows::UI::Xaml::Controls::Button();
        choice->Margin = Thickness(0, 0, 10, 10);
        choice->Padding = Thickness(0);
        choice->HorizontalContentAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        choice->VerticalContentAlignment = Windows::UI::Xaml::VerticalAlignment::Stretch;
        Skin(choice, 12, i == xmbTheme_);
        auto swatch = ref new Border();
        swatch->CornerRadius = Windows::UI::Xaml::CornerRadius(11);
        swatch->Padding = Thickness(12, 8, 12, 8);
        if (theme.monthly) {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            const Color month = XmbMonthColor(now.wMonth);
            swatch->Background = Gradient(Shade(month, 0.4), month);
        } else {
            swatch->Background = Gradient(RgbColor(theme.top[0], theme.top[1], theme.top[2]),
                                          RgbColor(theme.bottom[0], theme.bottom[1], theme.bottom[2]));
        }
        auto label = Text(ref new Platform::String(theme.name), 13, true);
        label->Foreground = Solid(Colors::White);
        label->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Bottom;
        swatch->Child = label;
        choice->Content = swatch;
        if (i == xmbTheme_) choice->BorderBrush = Solid(Colors::White);
        choice->GotFocus += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { PlayCue(0); });
        choice->Click += ref new RoutedEventHandler([this, i](Platform::Object^, RoutedEventArgs^) { SaveXmbTheme(i); });
        xmbColours->Children->Append(choice);
    }
    content_->Children->Append(xmbColours);
    }

    if (settingsTab_ == 3) {
    Section(L"Home menu soundtrack", L"Original ReVita songs, written for this app. Match the theme plays each theme's own song.");
    // shown in this order; the number is the saved track
    const wchar_t* names[] = {
        L"Match the theme", L"Starlight Drive", L"Afterlight", L"Neon Bloom", L"Midnight Arcade",
        L"Coastline '87", L"Pastel Horizon", L"Cotton Sky", L"Ultraviolet", L"Your own track", L"Off"
    };
    const int trackOf[] = { 10, 1, 2, 3, 4, 6, 7, 8, 9, 5, 0 };
    auto music = ActionRow(160);
    for (int slot = 0; slot < 11; ++slot) {
        const int i = trackOf[slot];
        auto b = Button(ref new Platform::String(names[slot]), track_ == i);
        b->Click += ref new RoutedEventHandler([this,i](Platform::Object^, RoutedEventArgs^) {
            if (i == 5) PickCustomMusic();
            else SetMusic(i);
        });
        music->Children->Append(b);
    }
    content_->Children->Append(music);
    musicStatus_ = Text(L"Audio: " + playbackStatus_, 12, false, true);
    musicStatus_->Margin = Thickness(3, 11, 0, 11);
    content_->Children->Append(musicStatus_);
    content_->Children->Append(Text(L"MUSIC VOLUME", 12, true, true));
    auto volume = ref new Slider();
    volume->Minimum = 0;
    volume->Maximum = 100;
    volume->Value = musicVolume_ * 100;
    volume->Width = 420;
    volume->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
    volume->ValueChanged += ref new RangeBaseValueChangedEventHandler(
        [this](Platform::Object^, RangeBaseValueChangedEventArgs^ e) {
            SetVolume(e->NewValue / 100.0);
        });
    content_->Children->Append(volume);

    Section(L"Interface sounds", L"Soft focus, confirmation, back and game-shelf swipe cues.");
    auto soundRow = ActionRow(200);
    auto enabled = Button(uiSounds_ ? L"UI sounds: ON" : L"UI sounds: OFF", uiSounds_);
    enabled->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) {
        uiSounds_ = !uiSounds_;
        SaveCueSettings();
        Render();
    });
    soundRow->Children->Append(enabled);
    auto preview = Button(L"Preview selection sound");
    preview->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) {
        PlayCue(1);
    });
    soundRow->Children->Append(preview);
    content_->Children->Append(soundRow);
    auto cueVolume = ref new Slider();
    cueVolume->Minimum = 0;
    cueVolume->Maximum = 100;
    cueVolume->Value = uiSoundVolume_ * 100.0;
    cueVolume->Width = 420;
    cueVolume->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
    cueVolume->ValueChanged += ref new RangeBaseValueChangedEventHandler(
        [this](Platform::Object^, RangeBaseValueChangedEventArgs^ e) {
            uiSoundVolume_ = std::max(0.0, std::min(1.0, e->NewValue / 100.0));
            for (auto player : cuePlayers_) if (player != nullptr) player->Volume = uiSoundVolume_;
            SaveCueSettings();
        });
    content_->Children->Append(Text(L"INTERFACE VOLUME", 12, true, true));
    content_->Children->Append(cueVolume);
    }

    if (settingsTab_ == 4) {
    Section(L"Game artwork", L"Use original cover images or bring in your own.");
    auto art = Vertical();
    art->Children->Append(Text(
        L"SteamGridDB cover matching requires a personal API key. The search uses each "
        L"game's title and only accepts an exact title match, to avoid mismatched artwork. "
        L"If no match is available, your game keeps its themed cover.", 13, false, true));
    auto password = ref new PasswordBox();
    password->PlaceholderText = L"SteamGridDB API key (saved in Windows PasswordVault)";
    password->Height = 40;
    password->Width = 460;
    password->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
    art->Children->Append(password);
    auto buttons = ref new StackPanel();
    buttons->Orientation = Orientation::Horizontal;
    auto save = Button(L"Save key", true);
    save->Click += ref new RoutedEventHandler([this,password](Platform::Object^, RoutedEventArgs^) {
        SaveApiKey(password->Password);
        password->Password = L"";
    });
    buttons->Children->Append(save);
    auto fetch = Button(L"Get missing covers");
    fetch->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) {
        FetchMissingArtwork();
    });
    buttons->Children->Append(fetch);
    art->Children->Append(buttons);
    art->Children->Append(Text(
        ApiKey()->IsEmpty() ? L"No API key saved yet." : L"SteamGridDB API key saved.",
        12, false, true));
    art->Children->Append(Text(
        L"Select a game card to import a custom PNG or JPEG. Custom artwork takes priority.",
        12, false, true));
    content_->Children->Append(Panel(art));
    }

    if (settingsTab_ == 5) {
    {
        // About: the name, the mark, and the engine underneath
        auto about = ref new Grid();
        auto c0 = ref new ColumnDefinition(); c0->Width = GridLength(1, GridUnitType::Auto);
        auto c1 = ref new ColumnDefinition(); c1->Width = GridLength(1, GridUnitType::Star);
        about->ColumnDefinitions->Append(c0);
        about->ColumnDefinitions->Append(c1);
        auto mark = ref new Image();
        mark->Source = ref new BitmapImage(ref new Uri(L"ms-appx:///Assets/ReVitaMark.png"));
        mark->Width = 76;
        mark->Height = 76;
        mark->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Top;
        about->Children->Append(mark);
        auto words = Vertical();
        words->Margin = Thickness(18, 0, 0, 0);
        Grid::SetColumn(words, 1);
        auto word = ref new Image();
        word->Source = ref new BitmapImage(ref new Uri(L"ms-appx:///Assets/ReVitaWordmark.png"));
        word->Height = 30;
        word->Stretch = Stretch::Uniform;
        word->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
        words->Children->Append(word);
        auto line = Text(L"PS Vita emulation for Xbox. Beta.", 14, false);
        line->Margin = Thickness(0, 8, 0, 4);
        words->Children->Append(line);
        words->Children->Append(Text(
            L"Powered by Vita3K, the open-source PS Vita emulator, with Mesa's Dozen driver for "
            L"graphics. ReVita and Vita3K are free software under the GNU GPL v2. ReVita includes "
            L"no Sony firmware, keys or games.", 12, false, true));
        about->Children->Append(words);
        content_->Children->Append(Panel(about, 20));
    }
    Section(L"Diagnostics", L"Where to look when something goes wrong.");
    auto advanced = Vertical();
    advanced->Children->Append(Text(
        L"When a game or install goes wrong, the log says why. Save it to your drive and open it on a PC, "
        L"or look at the end of it here.", 13, false, true));
    auto logActions = ref new StackPanel();
    logActions->Orientation = Orientation::Horizontal;
    logActions->Margin = Thickness(0, 12, 0, 0);
    auto saveLog = Button(L"Save log to your drive", true);
    saveLog->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { SaveLogToDrive(); });
    logActions->Children->Append(saveLog);
    auto showLog = Button(L"Show the recent log");
    showLog->Margin = Thickness(10, 0, 0, 0);
    logActions->Children->Append(showLog);
    advanced->Children->Append(logActions);
    auto logView = Text(L"", 10, false, true);
    logView->FontFamily = ref new Windows::UI::Xaml::Media::FontFamily(L"Consolas");
    logView->IsTextSelectionEnabled = true;
    logView->TextWrapping = TextWrapping::Wrap;
    logView->TextTrimming = TextTrimming::None;
    auto logScroll = ref new ScrollViewer();
    logScroll->IsTabStop = true; // this read-only pane needs controller scrolling
    logScroll->MaxHeight = 320;
    logScroll->Margin = Thickness(0, 12, 0, 0);
    logScroll->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    logScroll->Content = logView;
    advanced->Children->Append(logScroll);
    showLog->Click += ref new RoutedEventHandler([this,logView,logScroll](Platform::Object^, RoutedEventArgs^) {
        logView->Text = ref new Platform::String(RecentLogText().c_str());
        logScroll->Visibility = Windows::UI::Xaml::Visibility::Visible;
        logScroll->ChangeView(nullptr, ref new Platform::Box<double>(1e9), nullptr, true);
    });
    advanced->Children->Append(Text(
        L"It is also in the Device Portal: File Explorer, LocalAppData, this app, LocalState, vita3k-runtime.log.",
        11, false, true));
#if !defined(VITA3K_RELEASE_BUILD)
    // D3D12's debug layer checks every call the graphics driver makes, which
    // costs frame time; it is for chasing a graphics bug, so it is off unless
    // asked for here. It can only start with the app (before any device).
    {
        auto checks = Button(Pref(L"debug.d3d12")->Equals(L"1")
            ? L"Graphics debug checks: On (slower)" : L"Graphics debug checks: Off");
        checks->Margin = Thickness(0, 12, 0, 0);
        checks->Click += ref new RoutedEventHandler([this,checks](Platform::Object^, RoutedEventArgs^) {
            const bool on = !Pref(L"debug.d3d12")->Equals(L"1");
            PutPref(L"debug.d3d12", on ? L"1" : L"0");
            checks->Content = ref new Platform::String(on ? L"Graphics debug checks: On after a restart (slower)" : L"Graphics debug checks: Off after a restart");
        });
        advanced->Children->Append(checks);
    }
    // Development builds only; release builds ship without the Tools page.
    auto openTools = Button(L"Developer diagnostics");
    openTools->Margin = Thickness(0, 12, 0, 0);
    openTools->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) {
        ShowPage(3);
    });
    advanced->Children->Append(openTools);
#endif
    content_->Children->Append(Panel(advanced));
    }
    RestorePaths();
}
void CatalogShell::Diagnostics() {
    Banner(L"UNDER THE HOOD", L"Developer laboratory.",
        L"Vita3K CPU, graphics and emulator diagnostics.");
    Section(L"Vita3K core");
    auto libretroBox = Vertical();
    libretroBox->Children->Append(Text(
        L"Check for a Vita3K core DLL inside the app package. "
        L"Verify the core API before attempting to load a game. "
        L"The desktop DLL cannot automatically run as UWP. Game execution needs a "
        L"successfully ported core and working Vulkan graphics (possibly Mesa Dozen).",
        13, false, true));
    auto probeCore = Button(L"Check for actual Vita3K core", true);
    probeCore->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) {
        ProbeLibretro(false);
    });
    libretroBox->Children->Append(probeCore);
    auto initCore = Button(L"Attempt genuine core initialization (experimental)");
    initCore->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) {
        ProbeLibretro(true);
    });
    libretroBox->Children->Append(initCore);
    libretroResult_ = Text(L"Libretro core not probed. No UWP-compatible core DLL bundled yet.", 14, true);
    libretroBox->Children->Append(libretroResult_);
    content_->Children->Append(Panel(libretroBox));

    Section(L"Vulkan over Direct3D 12 / Mesa Dozen");
    auto dzn = Vertical();
    dzn->Children->Append(Text(
        L"Mesa Dozen translates Vulkan to native D3D12 on Windows. "
        L"On Xbox, the Vulkan loader and Dozen ICD must "
        L"run inside the app package. "
        L"A native D3D12 test frame is not evidence that Vulkan works.", 13, false, true));
    auto dznResult = Text(L"No Vulkan / Dozen probe run on this Xbox.", 14, true);
    auto dznButton = Button(L"Probe actual packaged Vulkan device");
    dznButton->Click += ref new RoutedEventHandler(
        [this,dznResult](Platform::Object^, RoutedEventArgs^) {
            dznResult->Text = StringOf(ProbePackagedVulkan());
        });
    dzn->Children->Append(dznButton);
    dzn->Children->Append(dznResult);
    content_->Children->Append(Panel(dzn));

    Section(L"Vita CPU test", L"Run a short ARMv7 program through Dynarmic.");
    auto cpuPanel = Vertical();
    cpuPanel->Children->Append(Text(
        L"This test runs three ARM instructions through Vita3K's Dynarmic CPU: "
        L"MOV, ADD and SVC. A pass means Xbox executed ARM guest code and returned "
        L"R0 = 12. It does not launch a Vita game.", 13, false, true));
    auto cpuReport = Text(L"ARM CPU test not started.", 13, false, true);
    auto runCpu = Button(L"Run Vita ARM instructions", true);
    runCpu->Click += ref new RoutedEventHandler(
        [cpuReport](Platform::Object^, RoutedEventArgs^) {
            cpuReport->Text = StringOf(RunVitaGuestArmCpuProbe());
        });
    cpuPanel->Children->Append(runCpu);
    cpuPanel->Children->Append(cpuReport);
    content_->Children->Append(Panel(cpuPanel));

    Section(L"Upstream Vita3K memory allocator");
    auto core = Vertical();
    core->Children->Append(Text(
        L"This runs upstream bitmap allocation, reservation and release routines. "
        L"It is not a Dynarmic emulator boot.", 13, false, true));
    auto test = Button(L"Run real upstream memory test", true);
    test->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) {
        ProbeCore();
    });
    core->Children->Append(test);
    coreResult_ = Text(L"Not tested on this Xbox.", 14, true);
    core->Children->Append(coreResult_);
    content_->Children->Append(Panel(core));

    Section(L"Virtual address space and JIT permissions");
    auto memory = Vertical();
    memory->Children->Append(Text(
        L"Check read/write pages, RW-to-RX protection and a reserved 4 GiB guest address "
        L"range. This does not execute generated ARM instructions.", 13, false, true));
    auto probe = Button(L"Probe memory compatibility");
    probe->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) {
        ProbeMemory();
    });
    memory->Children->Append(probe);
    memoryResult_ = Text(L"Not tested on this Xbox.", 14, true);
    memory->Children->Append(memoryResult_);
    auto jitDescription = Text(
        L"JIT liveness runs a tiny native x64 function generated into a writable page, "
        L"then switches it to executable-only/readable via VirtualProtectFromApp. "
        L"Only run this on the Xbox after testing the normal memory probe. "
        L"An Xbox code-integrity policy could terminate the app instead of returning an error. "
        L"Passing is not proof that the Vita CPU or Dynarmic works.", 13, false, true);
    memory->Children->Append(jitDescription);
    auto jitButton = Button(L"Execute on-demand JIT machine-code test");
    auto jitReport = Text(L"Generated machine code has not been executed.", 13, true);
    jitButton->Click += ref new RoutedEventHandler(
        [this,jitReport](Platform::Object^, RoutedEventArgs^) {
            jitReport->Text = StringOf(RunUwpJitLivenessProbe());
        });
    memory->Children->Append(jitButton);
    memory->Children->Append(jitReport);
    content_->Children->Append(Panel(memory));

    Section(L"Direct3D 12 renderer");
    auto graphics = Vertical();
    graphics->Children->Append(Text(
        L"This display uses a D3D12 device, command queue, swap chain and GPU upload. "
        L"It cannot render PS Vita games until the GXM command and shader pipeline exists.",
        13, false, true));
    renderSurface_ = ref new SwapChainPanel();
    renderSurface_->Width = 960;
    renderSurface_->Height = 544;
    renderSurface_->Margin = Thickness(0, 10, 0, 10);
    graphics->Children->Append(renderSurface_);
    rendererResult_ = Text(L"Renderer not initialized.", 13, true);
    graphics->Children->Append(rendererResult_);
    auto actions = ref new StackPanel();
    actions->Orientation = Orientation::Horizontal;
    auto initialize = Button(L"Initialize D3D12", true);
    initialize->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { BootRenderer(); });
    actions->Children->Append(initialize);
    auto upload = Button(L"Present generated framebuffer");
    upload->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { PresentTestFrame(); });
    actions->Children->Append(upload);
    graphics->Children->Append(actions);
    content_->Children->Append(Panel(graphics));
    renderSurface_->Loaded += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) {
        BootRenderer();
    });
}

void CatalogShell::ApplySurfaceColors() {
    // The rail and the status line are tinted glass over the moving scene.
    root_->Background = Solid(Bg());
    // the top bar: the page colour at the top, clear at its lower edge
    {
        const Color page = Bg();
        auto glass = ref new LinearGradientBrush();
        glass->StartPoint = Point(0.5, 0);
        glass->EndPoint = Point(0.5, 1);
        auto top = ref new GradientStop();
        top->Color = Alpha(235, page.R, page.G, page.B);
        top->Offset = 0;
        glass->GradientStops->Append(top);
        auto bottom = ref new GradientStop();
        bottom->Color = Alpha(0, page.R, page.G, page.B);
        bottom->Offset = 1;
        glass->GradientStops->Append(bottom);
        (void)glass;
        navigation_->Background = nullptr;   // nothing dims the page
    }
    if (statusBar_ != nullptr) statusBar_->Background = Solid(Alpha(120, Bg().R, Bg().G, Bg().B));
    if (status_ != nullptr) status_->Foreground = Solid(Muted());
}
void CatalogShell::UpdateClock() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    if (clock_ != nullptr) {
        const int hour12 = now.wHour % 12 == 0 ? 12 : now.wHour % 12;
        wchar_t text[32] = {};
        swprintf_s(text, L"%d:%02d %s", hour12, now.wMinute, now.wHour < 12 ? L"AM" : L"PM");
        clock_->Text = ref new Platform::String(text);
    }
    if (topClock_ != nullptr) {
        const int hour12 = now.wHour % 12 == 0 ? 12 : now.wHour % 12;
        wchar_t text[32] = {};
        swprintf_s(text, L"%d:%02d %s", hour12, now.wMinute, now.wHour < 12 ? L"AM" : L"PM");
        topClock_->Text = ref new Platform::String(text);
    }
    if (topDate_ != nullptr) {
        static const wchar_t* days[] = { L"SUNDAY", L"MONDAY", L"TUESDAY", L"WEDNESDAY", L"THURSDAY", L"FRIDAY", L"SATURDAY" };
        static const wchar_t* months[] = { L"JAN", L"FEB", L"MAR", L"APR", L"MAY", L"JUN", L"JUL", L"AUG", L"SEP", L"OCT", L"NOV", L"DEC" };
        wchar_t text[48] = {};
        swprintf_s(text, L"%s  %s %d", days[now.wDayOfWeek % 7], months[(now.wMonth + 11) % 12], now.wDay);
        topDate_->Text = ref new Platform::String(text);
    }
    if (xmbClock_ != nullptr) {
        // as the PS3 shows it: month/day, then the time
        const int hour12 = now.wHour % 12 == 0 ? 12 : now.wHour % 12;
        wchar_t text[48] = {};
        swprintf_s(text, L"%d/%d   %d:%02d %s", now.wMonth, now.wDay, hour12, now.wMinute, now.wHour < 12 ? L"AM" : L"PM");
        xmbClock_->Text = ref new Platform::String(text);
    }
    if (greeting_ != nullptr) {
        const wchar_t* line = now.wHour < 5 ? L"Late-night session." : now.wHour < 12 ? L"Good morning." :
                              now.wHour < 17 ? L"Good afternoon." : now.wHour < 22 ? L"Good evening." : L"Late-night session.";
        greeting_->Text = ref new Platform::String(line);
    }
}
void CatalogShell::SaveTheme(int theme) {
    if (theme < 0 || theme > 7) return;
    theme_ = theme;
    PutPref(L"revita.theme", StringOf(std::to_wstring(theme)));
    PlayCue(5);
    ApplySurfaceColors();
    backdrop_.SetTheme(theme_, Bg(), Accent(), Glow());
    BuildNavigation();
    if (track_ == 10) {
        musicPlayer_->Stop();
        PlayBundledMusic(10);
    }
    Render();
    SetStatus(L"Theme saved. Your colors will be restored on the next launch.");
}
int CatalogShell::ThemeTrack() const {
    // ReVita, Synthwave, Miami Night, Pastel Dream, Ultraviolet, Cotton Sky, Midnight Arcade, Starfall
    static const int song[8] = { 1, 3, 6, 7, 9, 8, 4, 2 };
    return song[(theme_ >= 0 && theme_ < 8) ? theme_ : 0];
}
void CatalogShell::SetVolume(double value) {
    musicVolume_ = std::max(0.0, std::min(1.0, value));
    musicPlayer_->Volume = musicVolume_;
    PutPref(L"music.volume", StringOf(std::to_wstring(musicVolume_)));
}
void CatalogShell::SetMusic(int track) {
    if (track < 0 || track > 10) return;
    track_ = track;
    PutPref(L"music.track", StringOf(std::to_wstring(track)));
    musicPlayer_->Stop();
    musicPlayer_->Source = nullptr;
    customMusicStream_ = nullptr;
    if (track > 0 && track != 5) PlayBundledMusic(track);
    else if (track == 5) PlayCustomMusic();
    else playbackStatus_ = L"Music off";
    Render();
    if (musicStatus_ != nullptr) musicStatus_->Text = playbackStatus_;
}
void CatalogShell::PlayBundledMusic(int track) {
    if (track == 10) track = ThemeTrack();
    if (track < 1 || track > 9 || track == 5) return;
    // Original ReVita songs shipped in the package (scripts/music/revita.py).
    // 5 is the player's own track; 10 means "the theme's song".
    static const wchar_t* files[10] = { nullptr, L"starlight-drive", L"afterlight", L"neon-bloom",
                                        L"midnight-arcade", nullptr, L"coastline-87", L"pastel-horizon",
                                        L"cotton-sky", L"ultraviolet" };
    musicPlayer_->IsLooping = true;
    musicPlayer_->Source = ref new Uri(ref new Platform::String(
        (std::wstring(L"ms-appx:///Assets/Music/") + files[track] + L".m4a").c_str()));
    playbackStatus_ = L"Playing";
    if (musicStatus_ != nullptr) musicStatus_->Text = playbackStatus_;
    musicPlayer_->Play(); // MediaOpened also starts it once ready.
}
void CatalogShell::PlayCustomMusic() {
    auto access = StorageApplicationPermissions::FutureAccessList;
    if (!access->ContainsItem(L"menu-track")) {
        playbackStatus_ = L"Choose a custom MP3, WAV or M4A file first.";
        if (musicStatus_ != nullptr) musicStatus_->Text = playbackStatus_;
        return;
    }
    playbackStatus_ = L"Opening custom soundtrack...";
    create_task(access->GetFileAsync(L"menu-track"))
    .then([this](StorageFile^ file) {
        return create_task(file->OpenAsync(FileAccessMode::Read)).then(
            [this,file](IRandomAccessStream^ stream) {
                if (track_ != 5) return;
                // Retain source for the entire playback lifetime.
                customMusicStream_ = stream;
                musicPlayer_->IsLooping = true;
                musicPlayer_->SetSource(customMusicStream_, file->ContentType);
                musicPlayer_->Play();
                playbackStatus_ = L"Opening " + file->Name + L"...";
                if (musicStatus_ != nullptr) musicStatus_->Text = playbackStatus_;
            });
    }).then([this](task<void> work) {
        try { work.get(); }
        catch (Platform::Exception^ error) {
            playbackStatus_ = L"Couldn't open custom music: " + error->Message;
            if (musicStatus_ != nullptr) musicStatus_->Text = playbackStatus_;
        }
    }, task_continuation_context::use_current());
}
void CatalogShell::PickCustomMusic() {
    auto picker = ref new FileOpenPicker();
    picker->SuggestedStartLocation = PickerLocationId::MusicLibrary;
    picker->FileTypeFilter->Append(L".mp3");
    picker->FileTypeFilter->Append(L".wav");
    picker->FileTypeFilter->Append(L".m4a");
    create_task(picker->PickSingleFileAsync()).then(
        [this](task<StorageFile^> work) {
            try {
                auto file = work.get();
                if (file == nullptr) return;
                StorageApplicationPermissions::FutureAccessList->AddOrReplace(L"menu-track", file);
                SetMusic(5);
            } catch (Platform::Exception^ e) { SetStatus(L"Music picker error: " + e->Message); }
        }, task_continuation_context::use_current());
}
void CatalogShell::RestorePaths() {
    auto future = StorageApplicationPermissions::FutureAccessList;
    if (folderLabel_ != nullptr) {
        folderLabel_->Text = future->ContainsItem(L"vita-games")
            ? L"Game folder access saved." : L"No game folder chosen yet.";
    }
    RefreshFirmwareStatus();
    if (addonsLabel_ != nullptr) {
        auto addons = Pref(L"storage.addons", L"");
        addonsLabel_->Text = addons->IsEmpty() ? ref new Platform::String(L"No updates folder chosen yet.") : L"Updates folder: " + addons;
    }
    if (dataLabel_ != nullptr) {
        auto chosen = Pref(L"storage.dataRoot", L"");
        dataLabel_->Text = chosen->IsEmpty() ? ref new Platform::String(L"Internal storage") : chosen;
    }
    if (future->ContainsItem(L"vita-games")) {
        create_task(future->GetFolderAsync(L"vita-games")).then(
            [this](task<StorageFolder^> work) {
                try {
                    auto folder = work.get();
                    if (folderLabel_ != nullptr)
                        folderLabel_->Text = L"Game folder: " + folder->Path;
                } catch (Platform::Exception^) {
                    if (folderLabel_ != nullptr)
                        folderLabel_->Text = L"Game folder permission expired. Select again.";
                }
            }, task_continuation_context::use_current());
    }
}
void CatalogShell::ChooseGameFolder() {
    auto picker = ref new FolderPicker();
    picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
    picker->FileTypeFilter->Append(L"*");
    SetStatus(L"Select a directory containing legitimate Vita game data.");
    create_task(picker->PickSingleFolderAsync()).then(
        [this](task<StorageFolder^> work) {
            try {
                auto folder = work.get();
                if (folder == nullptr) return;
                StorageApplicationPermissions::FutureAccessList->AddOrReplace(L"vita-games", folder);
                PutPref(L"storage.gamesPath", folder->Path);
                SetStatus(L"Library folder access saved. Scanning selected storage...");
                RestorePaths();
                ScanGames();
            } catch (Platform::Exception^ e) {
                SetStatus(L"Game folder picker failed: " + e->Message);
            }
        }, task_continuation_context::use_current());
}
namespace {
// The three firmware parts, as the core installs them.
struct FirmwarePart {
    const wchar_t* title;
    const wchar_t* partition;
    bool required;
    const wchar_t* versionKey;
    const wchar_t* about;
};
const FirmwarePart kFirmwareParts[3] = {
    { L"System firmware", L"vs0", true, L"firmware.version.vs0",
      L"Sony's PS Vita system update. It downloads as PSP2UPDAT.PUP; older copies are named PSVUPDAT.PUP. They are the same thing." },
    { L"Preinstalled package", L"pd0", false, L"firmware.version.pd0",
      L"A separate, smaller Sony download with the Vita's built-in apps. Some games want it." },
    { L"Fonts", L"sa0", false, L"firmware.version.sa0",
      L"A separate Sony download with the system fonts. Without it some text is missing." },
};

std::wstring InternalDataRoot() {
    return std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\Vita3KSystem";
}
// Where Vita3K keeps firmware, installed games and saves: the app's own
// storage, or a folder the user chose (usually on a USB drive).
std::wstring DataRoot() {
    auto chosen = Pref(L"storage.dataRoot", L"");
    return chosen->IsEmpty() ? InternalDataRoot() : std::wstring(chosen->Data());
}
std::wstring FirmwareRoot() {
    return DataRoot() + L"\\vita3k";
}
bool HasFiles(const std::wstring& directory) {
    std::error_code ec;
    return std::filesystem::is_directory(directory, ec) && !std::filesystem::is_empty(directory, ec);
}
// True when the core will be able to read this path where it is.
bool CanOpenFromApp(const std::wstring& path) {
    HANDLE handle = CreateFile2FromAppW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    CloseHandle(handle);
    return true;
}
std::string NarrowUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, &result[0], count, nullptr, nullptr);
    result.resize(static_cast<size_t>(count - 1));
    return result;
}
std::wstring WidenUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (count <= 1) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &result[0], count);
    result.resize(static_cast<size_t>(count - 1));
    return result;
}
// ---- Settings that outlive an uninstall ----
// Uninstalling the app deletes everything in its own (internal) storage,
// settings included. A copy of the settings sits as Vita3K-settings.txt on the
// drive: next to the data folder, at the drive's root and in the games folder.
// A fresh install with nothing set up looks for it on the connected drives and
// takes the settings, data folder, games folder and updates folder back.
std::vector<std::wstring> SettingsBackupDirs() {
    std::vector<std::wstring> dirs;
    auto add = [&dirs](const std::wstring& dir) {
        if (dir.empty()) return;
        for (const auto& have : dirs)
            if (_wcsicmp(have.c_str(), dir.c_str()) == 0) return;
        dirs.push_back(dir);
    };
    for (const wchar_t* key : { L"storage.dataRoot", L"storage.gamesPath" }) {
        const std::wstring dir = Pref(ref new Platform::String(key), L"")->Data();
        if (dir.empty()) continue;
        add(dir);
        add(std::filesystem::path(dir).root_path().wstring());
    }
    return dirs;
}
void BackupSettings() {
    try {
        std::string text;
        for (auto pair : ApplicationData::Current->LocalSettings->Values) {
            auto value = dynamic_cast<Platform::String^>(pair->Value);
            if (value == nullptr) continue;
            const std::wstring key = pair->Key->Data();
            const std::wstring data = value->Data();
            if (key.rfind(L"firmware.stage", 0) == 0 || key == L"restore.done") continue;
            if (data.find_first_of(L"\t\r\n") != std::wstring::npos) continue;
            text += NarrowUtf8(key) + "\t" + NarrowUtf8(data) + "\n";
        }
        if (text.empty()) return;
        for (const auto& dir : SettingsBackupDirs()) {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) continue;
            FILE* file = nullptr;
            if (_wfopen_s(&file, (dir + L"\\Vita3K-settings.txt").c_str(), L"wb") != 0 || file == nullptr) continue;
            fwrite(text.data(), 1, text.size(), file);
            fclose(file);
        }
    } catch (...) {
    }
}
// Reads a settings copy; settings already set on this install win.
bool ImportSettings(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    auto values = ApplicationData::Current->LocalSettings->Values;
    std::string line;
    unsigned taken = 0;
    while (std::getline(in, line)) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos || tab == 0) continue;
        auto key = ref new Platform::String(WidenUtf8(line.substr(0, tab)).c_str());
        if (values->HasKey(key)) continue;
        values->Insert(key, ref new Platform::String(WidenUtf8(line.substr(tab + 1)).c_str()));
        ++taken;
    }
    return taken != 0;
}
// A folder permission is lost with the app: ask for it again by path.
void RegrantFolder(const wchar_t* pathKey, const wchar_t* token) {
    auto path = Pref(ref new Platform::String(pathKey), L"");
    if (path->IsEmpty()) return;
    create_task(StorageFolder::GetFolderFromPathAsync(path)).then([token](task<StorageFolder^> found) {
        try { StorageApplicationPermissions::FutureAccessList->AddOrReplace(ref new Platform::String(token), found.get()); }
        catch (Platform::Exception^) {}
    });
}
// Fresh install: look for a settings copy on the connected drives.
void RestoreSettingsFromDrives(std::function<void()> done) {
    if (!Pref(L"storage.dataRoot", L"")->IsEmpty() || Pref(L"storage.internal", L"0")->Equals(L"1") ||
        StorageApplicationPermissions::FutureAccessList->ContainsItem(L"vita-games") || !Pref(L"storage.addons", L"")->IsEmpty())
        return;
    create_task(KnownFolders::RemovableDevices->GetFoldersAsync()).then(
        [done](task<Windows::Foundation::Collections::IVectorView<StorageFolder^>^> found) {
            try {
                for (auto drive : found.get()) {
                    std::wstring root = drive->Path->Data();
                    if (!root.empty() && root.back() != L'\\') root.push_back(L'\\');
                    for (const wchar_t* relative : { L"Vita3K-settings.txt", L"Vita3K\\Vita3K-settings.txt" }) {
                        std::error_code ec;
                        const std::wstring file = root + relative;
                        if (!std::filesystem::exists(file, ec) || !ImportSettings(file)) continue;
                        RegrantFolder(L"storage.gamesPath", L"vita-games");
                        RegrantFolder(L"storage.addons", L"vita-addons");
                        done();
                        return;
                    }
                }
            } catch (Platform::Exception^) {
            }
        }, task_continuation_context::use_current());
}
// 0x03740000 -> "3.74"
std::wstring FirmwareVersionText(std::uint32_t version) {
    if (!version) return L"";
    wchar_t text[16] = {};
    swprintf_s(text, L"%X.%02X", (version >> 24) & 0xFF, (version >> 16) & 0xFF);
    return text;
}
const wchar_t* PupKindName(std::uint32_t partitions) {
    if (partitions & 2) return L"System firmware";
    if (partitions & 4) return L"Preinstalled package";
    if (partitions & 8) return L"Font package";
    return nullptr;
}
// The most telling line of a failure's log text: the last error, else the last line.
std::wstring ProblemLine(const std::wstring& text) {
    std::wstring best, last;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);
        start = end + 1;
        if (line.empty()) continue;
        last = line;
        std::wstring lower = line;
        for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
        if (lower.find(L"fail") != std::wstring::npos || lower.find(L"error") != std::wstring::npos ||
            lower.find(L"could not") != std::wstring::npos || lower.find(L"crash") != std::wstring::npos ||
            lower.find(L"denied") != std::wstring::npos || lower.find(L"|e|") != std::wstring::npos)
            best = line;
    }
    std::wstring pick = best.empty() ? last : best;
    if (pick.size() > 220) pick = pick.substr(0, 220) + L"...";
    return pick.empty() ? std::wstring(L"Save the log in Settings, Diagnostics, for the details.") : pick;
}
struct FirmwareResult {
    std::wstring name;
    std::uint32_t partitions = 0;
    std::uint32_t headerVersion = 0;
    bool recognised = false;
    bool installed = false;
    std::wstring version;
    std::wstring detail;
};
}

bool CatalogShell::SystemFirmwareInstalled() {
    return HasFiles(FirmwareRoot() + L"\\vs0");
}

void CatalogShell::SetupCard() {
    const bool storageDone = !Pref(L"storage.dataRoot", L"")->IsEmpty() || Pref(L"storage.internal", L"0")->Equals(L"1");
    const bool firmwareDone = SystemFirmwareInstalled();
    const bool gamesDone = StorageApplicationPermissions::FutureAccessList->ContainsItem(L"vita-games") || !games_.empty();
    if (storageDone && firmwareDone && gamesDone) return;

    auto setup = Vertical();
    auto kick = Text(L"GET STARTED", 10, true);
    kick->CharacterSpacing = 300;
    kick->Foreground = BrandBrush();
    setup->Children->Append(kick);
    auto heading = Text(L"Three steps and you're playing", 22, false);
    heading->FontWeight = FontWeights::Light;
    heading->Margin = Thickness(0, 6, 0, 14);
    setup->Children->Append(heading);

    auto step = [this,setup](int number, bool done, bool current, Platform::String^ title, Platform::String^ detail) {
        auto row = ref new Grid();
        row->Margin = Thickness(0, 0, 0, 14);
        auto c0 = ref new ColumnDefinition();
        c0->Width = GridLength(44);
        auto c1 = ref new ColumnDefinition();
        c1->Width = GridLength(1, GridUnitType::Star);
        row->ColumnDefinitions->Append(c0);
        row->ColumnDefinitions->Append(c1);
        auto badge = ref new Border();
        badge->Width = 30;
        badge->Height = 30;
        badge->CornerRadius = Windows::UI::Xaml::CornerRadius(15);
        badge->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Top;
        badge->Background = Solid(done ? RgbColor(52, 168, 104) : current ? Accent() : Alpha(40, 255, 255, 255));
        auto mark = Text(done ? L"\uE73E" : ref new Platform::String(std::to_wstring(number).c_str()), 14, true);
        if (done) mark->FontFamily = ref new Windows::UI::Xaml::Media::FontFamily(L"Segoe MDL2 Assets");
        mark->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Center;
        mark->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        if (current && !done) mark->Foreground = Solid(Colors::Black);
        badge->Child = mark;
        row->Children->Append(badge);
        auto copy = Vertical();
        auto name = Text(title, 15, true);
        if (done) name->Opacity = 0.6;
        copy->Children->Append(name);
        auto about = Text(detail, 12, false, true);
        about->Margin = Thickness(0, 2, 0, 0);
        copy->Children->Append(about);
        Grid::SetColumn(copy, 1);
        row->Children->Append(copy);
        setup->Children->Append(row);
        return copy;
    };

    // 1. Storage: USB drives first.
    auto storage = step(1, storageDone, !storageDone, L"Choose where ReVita keeps its data",
        storageDone ? ref new Platform::String(Pref(L"storage.dataRoot", L"")->IsEmpty() ? L"Internal storage" : Pref(L"storage.dataRoot", L"")->Data())
                    : ref new Platform::String(L"Firmware, installed games and saves. A USB drive is best: installed games can be large."));
    if (!storageDone) {
        auto drives = ref new StackPanel();
        drives->Orientation = Orientation::Horizontal;
        drives->Margin = Thickness(0, 10, 0, 0);
        storage->Children->Append(drives);
        auto finding = Text(L"Looking for USB drives...", 12, false, true);
        drives->Children->Append(finding);
        create_task(KnownFolders::RemovableDevices->GetFoldersAsync()).then(
            [this,drives,finding](task<Windows::Foundation::Collections::IVectorView<StorageFolder^>^> found) {
                unsigned index = 0;
                if (drives->Children->IndexOf(finding, &index)) drives->Children->RemoveAt(index);
                unsigned count = 0;
                try {
                    for (auto drive : found.get()) {
                        auto path = drive->Path;
                        auto use = Button(L"Use " + drive->DisplayName, count == 0);
                        use->Margin = Thickness(0, 0, 10, 0);
                        use->Click += ref new RoutedEventHandler([this,path](Platform::Object^, RoutedEventArgs^) {
                            std::wstring root = path->Data();
                            if (!root.empty() && root.back() != L'\\') root.push_back(L'\\');
                            ChangeDataRoot(root + L"Vita3K");
                        });
                        drives->Children->Append(use);
                        ++count;
                    }
                } catch (Platform::Exception^) {}
                if (count == 0) {
                    auto pick = Button(L"Choose a folder on a drive", true);
                    pick->Margin = Thickness(0, 0, 10, 0);
                    pick->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { ChooseDataFolder(); });
                    drives->Children->Append(pick);
                }
                auto internal = Button(L"Use internal storage");
                internal->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) {
                    PutPref(L"storage.internal", L"1");
                    ChangeDataRoot(L"");
                    Render();
                });
                drives->Children->Append(internal);
            }, task_continuation_context::use_current());
    }

    // 2. Firmware.
    auto firmware = step(2, firmwareDone, storageDone && !firmwareDone, L"Install the PS Vita firmware",
        firmwareDone ? L"Installed" : L"Pick your .PUP files, all at once. They install right away.");
    if (!firmwareDone) {
        auto install = Button(L"Install firmware", storageDone);
        install->Margin = Thickness(0, 10, 0, 0);
        install->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
        install->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { ShowPage(2); ChooseFirmware(); });
        firmware->Children->Append(install);
    }

    // 3. Games.
    auto games = step(3, gamesDone, storageDone && firmwareDone && !gamesDone, L"Choose your games folder",
        gamesDone ? L"Done" : L"Your Vita games stay where they are, on the drive. Nothing is copied.");
    if (!gamesDone) {
        auto choose = Button(L"Choose games folder", storageDone && firmwareDone);
        choose->Margin = Thickness(0, 10, 0, 0);
        choose->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
        choose->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { ChooseGameFolder(); });
        games->Children->Append(choose);
    }

    auto tip = Text(L"Tip: in Dev Home, set this app's type to Game, so the Xbox gives it the memory games need.", 11, false, true);
    tip->Opacity = 0.75;
    setup->Children->Append(tip);
    auto card = Panel(setup, 24);
    card->Margin = Thickness(0, 0, 0, 20);
    card->Background = Gradient(Card(), Alpha(255, 30, 24, 38));
    content_->Children->Append(card);
}

std::wstring CatalogShell::RecentLogText() {
    libretro_.FlushLog();
    const std::wstring path = std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\vita3k-runtime.log";
    FILE* file = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!file) return L"No log yet. It is written when ReVita starts a game.";
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    const long take = std::min<long>(size, 24 * 1024);
    std::fseek(file, size - take, SEEK_SET);
    std::string tail(static_cast<size_t>(take), '\0');
    std::fread(&tail[0], 1, tail.size(), file);
    std::fclose(file);
    const auto firstLine = tail.find('\n');
    if (size > take && firstLine != std::string::npos) tail.erase(0, firstLine + 1);
    return WidenUtf8(tail);
}

void CatalogShell::SaveLogToDrive() {
    libretro_.FlushLog();
    // To the drive Vita3K's data is on, else the games folder, else ask.
    auto save = [this](StorageFolder^ target) {
        create_task(ApplicationData::Current->LocalFolder->GetFileAsync(L"vita3k-runtime.log"))
        .then([target](StorageFile^ log) {
            return create_task(log->CopyAsync(target, L"vita3k-runtime.log", NameCollisionOption::ReplaceExisting));
        }).then([this,target](task<StorageFile^> copied) {
            try {
                copied.get();
                SetStatus(L"Log saved to " + target->Path + L"\\vita3k-runtime.log");
                // The frame the game saved for diagnosis, when there is one.
                create_task(ApplicationData::Current->LocalFolder->TryGetItemAsync(L"frame-300.bmp"))
                .then([target](IStorageItem^ item) {
                    auto frame = dynamic_cast<StorageFile^>(item);
                    if (frame) frame->CopyAsync(target, L"frame-300.bmp", NameCollisionOption::ReplaceExisting);
                });
            } catch (Platform::Exception^ e) {
                SetStatus(L"Couldn't save the log: " + e->Message);
            }
        }, task_continuation_context::use_current());
    };
    auto pick = [this,save]() {
        auto picker = ref new FolderPicker();
        picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
        picker->FileTypeFilter->Append(L"*");
        create_task(picker->PickSingleFolderAsync()).then([save](task<StorageFolder^> picked) {
            try { if (auto folder = picked.get()) save(folder); } catch (Platform::Exception^) {}
        }, task_continuation_context::use_current());
    };
    auto chosen = Pref(L"storage.dataRoot", L"");
    if (!chosen->IsEmpty()) {
        create_task(StorageFolder::GetFolderFromPathAsync(chosen)).then([save,pick](task<StorageFolder^> found) {
            try { save(found.get()); } catch (Platform::Exception^) { pick(); }
        }, task_continuation_context::use_current());
        return;
    }
    auto access = StorageApplicationPermissions::FutureAccessList;
    if (access->ContainsItem(L"vita-games")) {
        create_task(access->GetFolderAsync(L"vita-games")).then([save,pick](task<StorageFolder^> found) {
            try { save(found.get()); } catch (Platform::Exception^) { pick(); }
        }, task_continuation_context::use_current());
        return;
    }
    pick();
}

void CatalogShell::CleanUpOldCopies() {
    // Earlier builds copied every game into app storage and parked firmware
    // files in the system folder. Games now play where they are and firmware
    // installs straight away, so those copies only take up space.
    const std::wstring local = ApplicationData::Current->LocalFolder->Path->Data();
    const std::wstring internalFirmware = InternalDataRoot() + L"\\vita3k";
    create_task([local,internalFirmware]() {
        std::error_code ec;
        std::filesystem::remove_all(local + L"\\Vita3KContent", ec);
        std::filesystem::remove_all(local + L"\\Vita3KTemp", ec);
        for (const wchar_t* name : { L"PSP2UPDAT.PUP", L"PSP2UPDAT_PREINST.PUP", L"PSP2UPDAT_FONT.PUP" })
            std::filesystem::remove(internalFirmware + L"\\" + name, ec);
    });
    auto values = ApplicationData::Current->LocalSettings->Values;
    for (const wchar_t* key : { L"firmware.stage.main", L"firmware.stage.preinst" })
        if (values->HasKey(ref new Platform::String(key))) values->Remove(ref new Platform::String(key));
}

void CatalogShell::ChooseFirmware() {
    // Like a BIOS folder in other emulators: pick every .PUP at once, under
    // any names. Each one is identified by what is inside it.
    auto picker = ref new FileOpenPicker();
    picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
    picker->FileTypeFilter->Append(L".pup");
    picker->FileTypeFilter->Append(L".PUP");
    create_task(picker->PickMultipleFilesAsync()).then(
        [this](task<Windows::Foundation::Collections::IVectorView<StorageFile^>^> picked) {
            try {
                auto files = picked.get();
                std::vector<StorageFile^> list;
                if (files) for (auto file : files) list.push_back(file);
                InstallFirmwareFiles(list);
            } catch (Platform::Exception^ e) {
                SetStatus(L"Firmware picker failed: " + e->Message);
            }
        }, task_continuation_context::use_current());
}

void CatalogShell::ChooseFirmwareFolder() {
    auto picker = ref new FolderPicker();
    picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
    picker->FileTypeFilter->Append(L"*");
    create_task(picker->PickSingleFolderAsync()).then([this](task<StorageFolder^> picked) {
        StorageFolder^ folder = nullptr;
        try { folder = picked.get(); } catch (Platform::Exception^ e) { SetStatus(L"Folder picker failed: " + e->Message); }
        if (folder == nullptr) return;
        create_task(folder->GetFilesAsync()).then(
            [this](task<Windows::Foundation::Collections::IVectorView<StorageFile^>^> listed) {
                std::vector<StorageFile^> list;
                try {
                    for (auto file : listed.get()) {
                        std::wstring name = file->Name->Data();
                        for (auto& c : name) c = static_cast<wchar_t>(towlower(c));
                        if (name.size() > 4 && name.compare(name.size() - 4, 4, L".pup") == 0) list.push_back(file);
                    }
                } catch (Platform::Exception^ e) {
                    SetStatus(L"Couldn't read that folder: " + e->Message);
                    return;
                }
                if (list.empty()) {
                    ShowFirmwareProgress(-1, L"No .PUP files in that folder.");
                    return;
                }
                InstallFirmwareFiles(list);
            }, task_continuation_context::use_current());
    }, task_continuation_context::use_current());
}

void CatalogShell::DownloadFirmware() {
    if (firmwareBusy_ || firmwareDownloading_) {
        SetStatus(L"Firmware is already being downloaded or installed.");
        return;
    }
    if (libretro_.GameLoaded() || booting_) {
        SetStatus(L"Stop the game before installing firmware.");
        return;
    }
    firmwareDownloading_ = true;
    ShowFirmwareProgress(-2, L"Contacting PlayStation's update server...");
    // The PS Vita system software, as linked from playstation.com's PS Vita
    // system software page. Nothing of Sony's is shipped with ReVita.
    auto uri = ref new Uri(L"http://dus01.psv.update.playstation.net/update/psv/image/2022_0209/"
                           L"rel_f2c7b12fe85496ec88a0391b514d6e3b/PSVUPDAT.PUP");
    auto client = ref new Windows::Web::Http::HttpClient();
    auto request = client->GetAsync(uri, Windows::Web::Http::HttpCompletionOption::ResponseContentRead);
    auto dispatcher = Dispatcher;
    auto lastPercent = std::make_shared<int>(-1);
    request->Progress = ref new AsyncOperationProgressHandler<Windows::Web::Http::HttpResponseMessage^, Windows::Web::Http::HttpProgress>(
        [this,dispatcher,lastPercent](IAsyncOperationWithProgress<Windows::Web::Http::HttpResponseMessage^, Windows::Web::Http::HttpProgress>^,
                                      Windows::Web::Http::HttpProgress progress) {
            if (progress.TotalBytesToReceive == nullptr || progress.TotalBytesToReceive->Value == 0) return;
            const unsigned long long total = progress.TotalBytesToReceive->Value;
            const int percent = static_cast<int>(100.0 * progress.BytesReceived / total);
            if (percent == *lastPercent) return;
            *lastPercent = percent;
            const unsigned long long receivedMb = progress.BytesReceived >> 20, totalMb = total >> 20;
            dispatcher->RunAsync(Windows::UI::Core::CoreDispatcherPriority::Low,
                ref new Windows::UI::Core::DispatchedHandler([this,percent,receivedMb,totalMb]() {
                    wchar_t text[128] = {};
                    swprintf_s(text, L"Downloading the PS Vita system software: %llu of %llu MB",
                        receivedMb, totalMb);
                    ShowFirmwareProgress(percent, ref new Platform::String(text));
                }));
        });
    create_task(request).then([client](Windows::Web::Http::HttpResponseMessage^ response) {
        response->EnsureSuccessStatusCode();
        return create_task(ApplicationData::Current->LocalFolder->CreateFolderAsync(L"Vita3KTemp", CreationCollisionOption::OpenIfExists))
        .then([](StorageFolder^ folder) {
            return folder->CreateFileAsync(L"PSVUPDAT.PUP", CreationCollisionOption::ReplaceExisting);
        }).then([response](StorageFile^ file) {
            return create_task(file->OpenAsync(FileAccessMode::ReadWrite)).then([response,file](IRandomAccessStream^ stream) {
                return create_task(response->Content->WriteToStreamAsync(stream)).then([stream](unsigned long long) {
                    return stream->FlushAsync();
                }).then([stream,file](bool) {
                    delete stream;   // closes it, so the installer can open the file
                    return file;
                });
            });
        });
    }).then([this](task<StorageFile^> saved) {
        firmwareDownloading_ = false;
        StorageFile^ file = nullptr;
        try {
            file = saved.get();
        } catch (Platform::Exception^ e) {
            ShowFirmwareProgress(-1, L"Couldn't download the firmware: " + e->Message +
                L" Check the console's internet connection, or install .PUP files you have.");
            return;
        } catch (...) {
            ShowFirmwareProgress(-1, L"Couldn't download the firmware. Check the console's internet connection.");
            return;
        }
        ShowFirmwareProgress(100, L"Downloaded. Installing...");
        InstallFirmwareFiles(std::vector<StorageFile^>{ file });
    }, task_continuation_context::use_current());
}

void CatalogShell::ShowFirmwareProgress(double value, Platform::String^ message) {
    if (firmwareProgress_ != nullptr) {
        // -1 hides the bar, -2 shows it without a percentage.
        firmwareProgress_->Visibility = value == -1 ? Windows::UI::Xaml::Visibility::Collapsed
                                                    : Windows::UI::Xaml::Visibility::Visible;
        firmwareProgress_->IsIndeterminate = value == -2;
        if (value >= 0) firmwareProgress_->Value = value;
    }
    if (message != nullptr) {
        if (firmwareMessage_ != nullptr) firmwareMessage_->Text = message;
        SetStatus(message);
    }
}

void CatalogShell::InstallFirmwareFiles(std::vector<StorageFile^> files) {
    if (files.empty()) return;
    if (firmwareBusy_) {
        SetStatus(L"Firmware is already being installed.");
        return;
    }
    if (libretro_.GameLoaded() || booting_) {
        SetStatus(L"Stop the game before installing firmware.");
        return;
    }
    firmwareBusy_ = true;
    ShowFirmwareProgress(0, L"Getting the firmware files ready...");
    // Files the core can read where they are (internal storage, USB drives)
    // install from there. Anything else is copied to a temporary folder first.
    auto paths = std::make_shared<std::vector<std::pair<std::wstring, std::wstring>>>();
    auto temps = std::make_shared<std::vector<std::wstring>>();
    const std::string system = NarrowUtf8(DataRoot());
    auto dispatcher = Dispatcher;
    create_task(ApplicationData::Current->LocalFolder->CreateFolderAsync(L"Vita3KTemp", CreationCollisionOption::OpenIfExists))
    .then([files,paths,temps](StorageFolder^ temp) {
        task<void> chain = task_from_result();
        for (auto file : files) {
            chain = chain.then([file,temp,paths,temps]() -> task<void> {
                if (CanOpenFromApp(file->Path->Data())) {
                    paths->push_back({ file->Path->Data(), file->Name->Data() });
                    return task_from_result();
                }
                return create_task(file->CopyAsync(temp, file->Name, NameCollisionOption::GenerateUniqueName))
                    .then([file,paths,temps](StorageFile^ copy) {
                        paths->push_back({ copy->Path->Data(), file->Name->Data() });
                        temps->push_back(copy->Path->Data());
                    });
            });
        }
        return chain;
    }).then([this,paths,system,dispatcher]() {
        std::vector<FirmwareResult> results;
        const size_t total = paths->size();
        // With Vita3K's data on a USB drive, each package installs into the
        // app's own storage first and the finished parts are then copied to
        // the drive: the installer's temporary files and deletions stay off
        // the drive, where they go through the slower brokered file access.
        const std::wstring finalRoot = WidenUtf8(system);
        const bool staged = _wcsicmp(finalRoot.c_str(), InternalDataRoot().c_str()) != 0;
        const std::wstring stagingRoot = std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\Vita3KStaging";
        const std::string installRoot = staged ? NarrowUtf8(stagingRoot) : system;
        // The system firmware first, then the preinstalled package, then the
        // fonts: the system firmware is what games need, and it is the one
        // that installed alone (or first after a restart) but failed after the
        // other two in the same batch.
        struct Pending { std::wstring path, name; bool recognised = false; std::uint32_t version = 0, partitions = 0; };
        std::vector<Pending> order;
        for (const auto& entry : *paths) {
            Pending item{ entry.first, entry.second };
            item.recognised = libretro_.PupInfo(NarrowUtf8(entry.first), item.version, item.partitions);
            order.push_back(item);
        }
        const auto rank = [](const Pending& item) {
            if (!item.recognised) return 9;
            if (item.partitions & 3u) return 0;
            if (item.partitions & 4u) return 1;
            return 2;
        };
        std::stable_sort(order.begin(), order.end(), [&](const Pending& a, const Pending& b) { return rank(a) < rank(b); });
        for (size_t i = 0; i < total; ++i) {
            FirmwareResult result;
            result.name = order[i].name;
            const std::string pup = NarrowUtf8(order[i].path);
            result.recognised = order[i].recognised;
            result.headerVersion = order[i].version;
            result.partitions = order[i].partitions;
            const wchar_t* kind = PupKindName(result.partitions);
            if (!result.recognised || !kind) {
                result.detail = L"not a PS Vita firmware package";
                results.push_back(result);
                continue;
            }
            const std::wstring heading = std::wstring(L"Installing ") + kind + L" " +
                FirmwareVersionText(result.headerVersion) + L" (" + std::to_wstring(i + 1) + L" of " +
                std::to_wstring(total) + L")...";
            std::function<void(std::uint32_t)> notify = [this,dispatcher,i,total,heading](std::uint32_t percent) {
                const double overall = (static_cast<double>(i) * 100.0 + percent) / static_cast<double>(total);
                dispatcher->RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal,
                    ref new Windows::UI::Core::DispatchedHandler([this,overall,heading]() {
                        ShowFirmwareProgress(overall, ref new Platform::String(heading.c_str()));
                    }));
            };
            notify(0);
            std::string version;
            libretro_.LogLine("Firmware: installing " + NarrowUtf8(result.name) + " (" + std::to_string(i + 1) + " of " + std::to_string(total) + ")");
            result.installed = libretro_.InstallFirmware(installRoot, pup, notify, version, result.detail);
            if (!result.installed && !libretro_.GameLoaded()) {
                // It installs on its own: give the file system a moment to
                // finish the previous package's cleanup and try once more.
                libretro_.LogLine("Firmware: " + NarrowUtf8(result.name) + " failed in the batch; trying it once more");
                Sleep(1500);
                notify(0);
                version.clear();
                result.installed = libretro_.InstallFirmware(installRoot, pup, notify, version, result.detail);
            }
            libretro_.LogLine("Firmware: " + NarrowUtf8(result.name) + (result.installed ? " installed" : " failed: " + NarrowUtf8(result.detail)));
            result.version = version.empty() ? FirmwareVersionText(result.headerVersion) : WidenUtf8(version);
            if (result.installed && staged) {
                dispatcher->RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal,
                    ref new Windows::UI::Core::DispatchedHandler([this]() {
                        ShowFirmwareProgress(-2, L"Copying the firmware to your drive...");
                    }));
                for (const wchar_t* part : { L"os0", L"vs0", L"pd0", L"sa0" }) {
                    const std::wstring from = stagingRoot + L"\\vita3k\\" + part;
                    if (!HasFiles(from)) continue;
                    const std::wstring to = finalRoot + L"\\vita3k\\" + part;
                    std::error_code ec;
                    std::filesystem::remove_all(to, ec);
                    ec.clear();
                    std::filesystem::create_directories(to, ec);
                    ec.clear();
                    std::filesystem::copy(from, to, std::filesystem::copy_options::recursive |
                        std::filesystem::copy_options::overwrite_existing, ec);
                    if (ec) {
                        result.installed = false;
                        result.detail = L"Copying " + std::wstring(part) + L" to the drive failed: " + WidenUtf8(ec.message());
                        libretro_.LogLine("Firmware: copying " + NarrowUtf8(part) + " to the drive failed: " + ec.message());
                        break;
                    }
                }
                std::error_code ec;
                std::filesystem::remove_all(stagingRoot, ec);
            }
            results.push_back(result);
        }
        return results;
    }, task_continuation_context::use_arbitrary())
    .then([this,temps](task<std::vector<FirmwareResult>> done) {
        firmwareBusy_ = false;
        for (const auto& path : *temps) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
        std::vector<FirmwareResult> results;
        try {
            results = done.get();
        } catch (Platform::Exception^ e) {
            ShowFirmwareProgress(-1, L"Couldn't read the firmware files: " + e->Message);
            RefreshFirmwareStatus();
            return;
        } catch (...) {
            ShowFirmwareProgress(-1, L"Couldn't install the firmware. vita3k-runtime.log has the details.");
            RefreshFirmwareStatus();
            return;
        }
        std::wstring summary;
        int systemCount = 0;
        for (const auto& r : results) {
            if (!summary.empty()) summary += L"\n";
            const wchar_t* kind = PupKindName(r.partitions);
            if (!r.recognised || !kind) {
                summary += r.name + L": not a PS Vita firmware package, skipped.";
                continue;
            }
            if (r.partitions & 2) ++systemCount;
            if (!r.installed) {
                summary += r.name + L": " + kind + L", install failed. " + ProblemLine(r.detail);
                continue;
            }
            summary += r.name + L": " + kind + (r.version.empty() ? std::wstring() : L" " + r.version) + L", installed.";
            const std::uint32_t bits[3] = { 2u, 4u, 8u };
            for (int part = 0; part < 3; ++part)
                if (r.partitions & bits[part])
                    PutPref(ref new Platform::String(kFirmwareParts[part].versionKey),
                            ref new Platform::String(r.version.c_str()));
        }
        if (systemCount > 1)
            summary += L"\nMore than one file was the system firmware: PSVUPDAT.PUP and PSP2UPDAT.PUP are the same update under two names.";
        const std::wstring root = FirmwareRoot();
        std::wstring missing;
        for (const auto& part : kFirmwareParts) {
            if (HasFiles(root + L"\\" + part.partition)) continue;
            if (!missing.empty()) missing += L", ";
            missing += part.title;
        }
        summary += missing.empty() ? std::wstring(L"\nAll firmware parts are installed.")
                                   : L"\nStill missing: " + missing + L".";
        ShowFirmwareProgress(-1, ref new Platform::String(summary.c_str()));
        RefreshFirmwareStatus();
    }, task_continuation_context::use_current());
}
namespace {
Platform::String^ CoreOptionPrefKey(const vita3k_uwp::CoreOptionDef& option) {
    std::wstring key = L"core.";
    for (const char* c = option.key; *c; ++c) key.push_back(static_cast<wchar_t>(*c));
    return ref new Platform::String(key.c_str());
}
// The saved value of an option, or its default when none (or an unknown one) is saved.
std::string SavedCoreOption(const vita3k_uwp::CoreOptionDef& option) {
    auto saved = Pref(CoreOptionPrefKey(option), L"");
    std::string value;
    for (const wchar_t* c = saved->Data(); *c; ++c) value.push_back(static_cast<char>(*c));
    for (std::size_t v = 0; v < option.valueCount; ++v)
        if (value == option.values[v].value) return value;
    return option.defaultValue;
}
}

namespace {
// Settings a game needs to get through, applied when the player has not set
// that option themselves. Each from the game's Vita3K compatibility report.
struct TitleOverride {
    const wchar_t* titleId;
    const char* key;
    const char* value;
    const char* renderer; // only with this renderer (nullptr: any)
    bool always = false;  // even over the player's own setting
};
const TitleOverride kTitleOverrides[] = {
    // Danganronpa: Trigger Happy Havoc (Vita3K/compatibility#94): the game
    // stops responding when you examine things or talk to people, unless it
    // runs on OpenGL with surface sync on. Tested on the console: that pair
    // fixes it, any other doesn't. Settings are app-wide here, so this game
    // always gets the pair.
    { L"PCSE00261", "vita3k_backend_renderer", "OpenGL", nullptr, true },
    { L"PCSE00261", "vita3k_surface_sync", "enabled", nullptr, true },
    // Gravity Rush: its bloom reads the game's own picture back; without
    // surface sync it comes out over-bright (Vita3K/compatibility#177, the
    // fix the iOS build ships). On even with OpenGL, unless the player chose.
    { L"PCSA00011", "vita3k_surface_sync", "enabled", nullptr, false },
    { L"PCSF00024", "vita3k_surface_sync", "enabled", nullptr, false },
    // Minecraft: with surface sync the picture stays black on Vulkan (build
    // 107 log: every presented frame empty while the game ran at 35 fps).
    { L"PCSE00491", "vita3k_surface_sync", "disabled", nullptr, false },
    { L"PCSB00560", "vita3k_surface_sync", "disabled", nullptr, false },
    { L"PCSG00302", "vita3k_surface_sync", "disabled", nullptr, false },
};
// The title ID (four letters, five digits) in a game's folder or file name.
std::wstring TitleIdIn(const std::wstring& path) {
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
    return std::wstring();
}
}

void CatalogShell::ApplyCoreOptions(const std::wstring& contentPath) {
    const std::wstring titleId = TitleIdIn(contentPath);
    std::string renderer = "Vulkan";
    for (std::size_t i = 0; i < vita3k_uwp::kCoreOptionCount; ++i)
        if (std::strcmp(vita3k_uwp::kCoreOptions[i].key, "vita3k_backend_renderer") == 0)
            renderer = SavedCoreOption(vita3k_uwp::kCoreOptions[i]);
    for (const auto& o : kTitleOverrides)
        if (o.always && titleId == o.titleId && std::strcmp(o.key, "vita3k_backend_renderer") == 0)
            renderer = o.value;
    for (std::size_t i = 0; i < vita3k_uwp::kCoreOptionCount; ++i) {
        const auto& option = vita3k_uwp::kCoreOptions[i];
        std::string value = SavedCoreOption(option);
        const bool chosen = Pref(CoreOptionPrefKey(option), L"")->Length() != 0;
        // Surface sync is off by default (the iOS build learned the same:
        // on for everyone it broke games); the games that need it get it
        // from the table above.
        if (!chosen && std::strcmp(option.key, "vita3k_surface_sync") == 0)
            value = "disabled";
        if (!titleId.empty()) {
            for (const auto& o : kTitleOverrides) {
                if ((!chosen || o.always) && titleId == o.titleId && std::strcmp(option.key, o.key) == 0 && (!o.renderer || renderer == o.renderer)) {
                    value = o.value;
                    libretro_.LogLine(std::string("Game setting for ") + NarrowUtf8(titleId) + ": " + o.key + " = " + o.value);
                }
            }
        }
        libretro_.SetOption(option.key, value);
    }
}

void CatalogShell::FirmwareSettings() {
    Section(L"Firmware", L"ReVita runs on Sony's PS Vita firmware. ReVita ships none of it.");
    auto box = Vertical();
    // One status tile per part.
    auto tiles = ref new Grid();
    for (int i = 0; i < 3; ++i) {
        auto column = ref new ColumnDefinition();
        column->Width = GridLength(1, GridUnitType::Star);
        tiles->ColumnDefinitions->Append(column);
    }
    for (int i = 0; i < 3; ++i) {
        const FirmwarePart& part = kFirmwareParts[i];
        auto tile = ref new Border();
        tile->CornerRadius = Windows::UI::Xaml::CornerRadius(14);
        tile->Background = Gradient(RgbColor(28, 29, 37), RgbColor(19, 20, 26));
        tile->BorderBrush = Solid(Alpha(26, 255, 255, 255));
        tile->BorderThickness = Thickness(1);
        tile->Padding = Thickness(16, 14, 16, 14);
        tile->Margin = Thickness(i == 0 ? 0 : 6, 0, i == 2 ? 0 : 6, 0);
        auto inner = Vertical();
        auto head = ref new StackPanel();
        head->Orientation = Orientation::Horizontal;
        firmwareDot_[i] = ref new Border();
        firmwareDot_[i]->Width = 10;
        firmwareDot_[i]->Height = 10;
        firmwareDot_[i]->CornerRadius = Windows::UI::Xaml::CornerRadius(5);
        firmwareDot_[i]->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        firmwareDot_[i]->Margin = Thickness(0, 0, 8, 0);
        head->Children->Append(firmwareDot_[i]);
        auto need = Text(part.required ? L"REQUIRED" : L"RECOMMENDED", 10, true, !part.required);
        need->CharacterSpacing = 160;
        if (part.required) need->Foreground = BrandBrush();
        head->Children->Append(need);
        inner->Children->Append(head);
        auto title = Text(ref new Platform::String(part.title), 17, true);
        title->Margin = Thickness(0, 8, 0, 2);
        inner->Children->Append(title);
        firmwareStatus_[i] = Text(L"", 13, false, true);
        inner->Children->Append(firmwareStatus_[i]);
        auto about = Text(ref new Platform::String(part.about), 11, false, true);
        about->Margin = Thickness(0, 8, 0, 0);
        about->Opacity = 0.8;
        inner->Children->Append(about);
        tile->Child = inner;
        Grid::SetColumn(tile, i);
        tiles->Children->Append(tile);
    }
    box->Children->Append(tiles);

    auto actions = ref new StackPanel();
    actions->Orientation = Orientation::Horizontal;
    actions->Margin = Thickness(0, 16, 0, 0);
    auto download = Button(L"Download from PlayStation", !SystemFirmwareInstalled());
    download->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { DownloadFirmware(); });
    actions->Children->Append(download);
    auto install = Button(L"Install firmware files", false);
    install->Margin = Thickness(10, 0, 0, 0);
    install->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { ChooseFirmware(); });
    actions->Children->Append(install);
    auto fromFolder = Button(L"Install from a folder");
    fromFolder->Margin = Thickness(10, 0, 0, 0);
    fromFolder->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { ChooseFirmwareFolder(); });
    actions->Children->Append(fromFolder);
    auto reset = Button(L"Remove installed firmware");
    reset->Margin = Thickness(10, 0, 0, 0);
    reset->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { ResetInstalledFirmware(); });
    actions->Children->Append(reset);
    box->Children->Append(actions);

    firmwareProgress_ = ref new ProgressBar();
    firmwareProgress_->Minimum = 0;
    firmwareProgress_->Maximum = 100;
    firmwareProgress_->Height = 6;
    firmwareProgress_->Margin = Thickness(0, 14, 0, 0);
    firmwareProgress_->Foreground = BrandBrush();
    firmwareProgress_->Visibility = firmwareBusy_ ? Windows::UI::Xaml::Visibility::Visible
                                                  : Windows::UI::Xaml::Visibility::Collapsed;
    box->Children->Append(firmwareProgress_);
    firmwareMessage_ = Text(
        L"Download the system software from PlayStation's own update server, or pick .PUP files you "
        L"already have, all at once, under any names. ReVita tells the system firmware, preinstalled "
        L"package and fonts apart and installs them right away.",
        12, false, true);
    firmwareMessage_->Margin = Thickness(0, 10, 0, 0);
    box->Children->Append(firmwareMessage_);
    content_->Children->Append(Panel(box));
    RefreshFirmwareStatus();
}

void CatalogShell::StorageSettings() {
    Section(L"Storage", L"Games play from where they are. ReVita's own data can live on your drive too.");
    auto box = Vertical();
    auto addRow = [this,box](Platform::String^ title, Platform::String^ detail, TextBlock^ label,
                             Windows::UI::Xaml::Controls::Button^ first, Windows::UI::Xaml::Controls::Button^ second) {
        auto grid = ref new Grid();
        grid->Margin = Thickness(0, 6, 0, 10);
        auto left = ref new ColumnDefinition();
        left->Width = GridLength(1, GridUnitType::Star);
        auto right = ref new ColumnDefinition();
        right->Width = GridLength(1, GridUnitType::Auto);
        grid->ColumnDefinitions->Append(left);
        grid->ColumnDefinitions->Append(right);
        auto copy = Vertical();
        copy->Children->Append(Text(title, 15, true));
        label->Foreground = BrandBrush();
        label->Margin = Thickness(0, 3, 0, 3);
        copy->Children->Append(label);
        auto about = Text(detail, 11, false, true);
        about->Margin = Thickness(0, 0, 24, 0);
        copy->Children->Append(about);
        grid->Children->Append(copy);
        auto actions = ref new StackPanel();
        actions->Orientation = Orientation::Horizontal;
        actions->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        actions->Children->Append(first);
        if (second != nullptr) {
            second->Margin = Thickness(10, 0, 0, 0);
            actions->Children->Append(second);
        }
        Grid::SetColumn(actions, 1);
        grid->Children->Append(actions);
        box->Children->Append(grid);
    };
    folderLabel_ = Text(L"", 13);
    auto chooseGames = Button(L"Choose games folder", true);
    chooseGames->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { ChooseGameFolder(); });
    addRow(L"Games", L"A folder of games (game folders, .vpk or .pkg files), or one game's own folder. "
        L"A USB drive works: games are read where they are, nothing is copied.", folderLabel_, chooseGames, nullptr);
    dataLabel_ = Text(L"", 13);
    auto onDrive = Button(L"Use a folder on a drive");
    onDrive->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { ChooseDataFolder(); });
    auto internal = Button(L"Use internal storage");
    internal->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { ChangeDataRoot(L""); });
    addRow(L"ReVita data", L"Firmware, installed games, saves and caches. ReVita installs a game here the first "
        L"time it runs it, so if internal space is short, put this on your USB drive.", dataLabel_, onDrive, internal);
    addonsLabel_ = Text(L"", 13);
    auto chooseAddons = Button(L"Choose updates folder", true);
    chooseAddons->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { ChooseAddonsFolder(); });
    auto installAddons = Button(L"Install new ones");
    installAddons->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { InstallAddons(); });
    addRow(L"Updates and DLC", L"A folder of game updates and DLC (folders, .vpk, .zip or .pkg). New ones are installed "
        L"when you choose it, with Install new ones, and before a game they belong to starts.", addonsLabel_, chooseAddons, installAddons);
    content_->Children->Append(Panel(box));
    RestorePaths();
}

void CatalogShell::ChooseDataFolder() {
    auto picker = ref new FolderPicker();
    picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
    picker->FileTypeFilter->Append(L"*");
    create_task(picker->PickSingleFolderAsync()).then([this](task<StorageFolder^> picked) {
        StorageFolder^ folder = nullptr;
        try { folder = picked.get(); } catch (Platform::Exception^ e) { SetStatus(L"Folder picker failed: " + e->Message); }
        if (folder == nullptr) return;
        StorageApplicationPermissions::FutureAccessList->AddOrReplace(L"vita-data", folder);
        ChangeDataRoot(std::wstring(folder->Path->Data()) + L"\\Vita3K");
    }, task_continuation_context::use_current());
}

namespace {
// The updates and DLC in a folder: folders with a param.sfo (one level of
// grouping folders too), and .vpk/.zip/.pkg files.
std::vector<std::filesystem::path> AddonItems(const std::wstring& root) {
    namespace fs = std::filesystem;
    std::vector<fs::path> items;
    std::error_code ec;
    const auto isTitle = [](const fs::path& dir) {
        std::error_code e;
        return fs::exists(dir / L"sce_sys" / L"param.sfo", e);
    };
    const auto isPackage = [](const fs::path& file) {
        std::wstring ext = file.extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        return ext == L".pkg" || ext == L".vpk" || ext == L".zip";
    };
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e;
        const fs::path path = it->path();
        if (fs::is_directory(path, e)) {
            if (isTitle(path)) { items.push_back(path); continue; }
            for (fs::directory_iterator sub(path, e), subEnd; !e && sub != subEnd; sub.increment(e)) {
                std::error_code e2;
                if (fs::is_directory(sub->path(), e2) ? isTitle(sub->path()) : isPackage(sub->path()))
                    items.push_back(sub->path());
            }
        } else if (isPackage(path)) {
            items.push_back(path);
        }
    }
    return items;
}
// Changes when the item does: its size (a file) or its param.sfo's.
std::wstring AddonSignature(const std::filesystem::path& item) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path probe = fs::is_directory(item, ec) ? item / L"sce_sys" / L"param.sfo" : item;
    const auto size = fs::file_size(probe, ec);
    const auto stamp = fs::last_write_time(probe, ec).time_since_epoch().count();
    return item.wstring() + L"|" + std::to_wstring(size) + L"|" + std::to_wstring(stamp);
}
// An update's PKG decrypts with its game's license. A game dump keeps that
// as sce_sys/package/work.bin, but Vita3K only has its own copy once the game
// has been installed, so an update installed first (a fresh data folder, or
// the update dropped in before the game was ever played) was refused for "no
// license". The game's work.bin is copied in where Vita3K looks for it.
bool ProvideUpdateLicense(VitaLibretroHost& host, const std::filesystem::path& pkg, const std::vector<std::wstring>& gameDirs) {
    namespace fs = std::filesystem;
    std::wstring ext = pkg.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    if (ext != L".pkg") return false;
    std::string contentId;
    {
        std::ifstream in(pkg, std::ios::binary);
        char header[0x60] = {};
        if (!in.read(header, sizeof(header))) return false;
        contentId.assign(header + 0x30, strnlen(header + 0x30, 0x30));
    }
    if (contentId.size() < 16) return false;
    const std::string titleId = contentId.substr(7, 9);
    const fs::path licenseDir = fs::path(FirmwareRoot()) / L"ux0" / L"license" / WidenUtf8(titleId);
    std::error_code ec;
    const fs::path target = licenseDir / WidenUtf8(contentId + ".rif");
    if (fs::exists(target, ec)) return true;
    // The game's folder: one of the library's, or one named after the title
    // ID next to the updates folder or a level up (ROMS/PSVita/games/PCSE00491)
    std::vector<fs::path> candidates;
    const std::wstring wideTitle = WidenUtf8(titleId);
    for (const auto& dir : gameDirs) {
        if (dir.empty()) continue;
        const fs::path p(dir);
        candidates.push_back(p);
        candidates.push_back(p / wideTitle);
    }
    for (const fs::path& base : { pkg.parent_path(), pkg.parent_path().parent_path(), pkg.parent_path().parent_path().parent_path() }) {
        if (base.empty()) continue;
        candidates.push_back(base / wideTitle);
        for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e;
            if (it->is_directory(e)) candidates.push_back(it->path() / wideTitle);
        }
        ec.clear();
    }
    for (const auto& dir : candidates) {
        const fs::path workBin = dir / L"sce_sys" / L"package" / L"work.bin";
        std::error_code e;
        if (!fs::exists(workBin, e)) continue;
        std::string bytes;
        {
            std::ifstream in(workBin, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        if (bytes.size() < 0x40) continue;
        // a license names its content at 0x10: it must be this title's
        const std::string licensed(bytes.data() + 0x10, strnlen(bytes.data() + 0x10, 0x30));
        if (licensed.size() < 16 || licensed.substr(7, 9) != titleId) continue;
        fs::create_directories(licenseDir, e);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) continue;
        out.close();
        // under the game's own name too, as Vita3K files it when installing the game
        if (licensed != contentId) {
            std::ofstream copy(licenseDir / WidenUtf8(licensed + ".rif"), std::ios::binary | std::ios::trunc);
            copy.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        host.LogLine("Updates and DLC: " + titleId + "'s license taken from " + NarrowUtf8(workBin.wstring()) + " for " + NarrowUtf8(pkg.filename().wstring()));
        return true;
    }
    host.LogLine("Updates and DLC: no license for " + titleId + " found yet (its game's folder has sce_sys/package/work.bin; play the game once, or keep it in the games folder)");
    return false;
}

// Installs the items not installed before (for one title, when titleId is set:
// those whose name says it). Returns a line for each one tried.
std::wstring InstallPendingAddons(VitaLibretroHost& host, const std::string& system, const std::wstring& root,
                                  const std::wstring& titleId, int& installed, int& failed,
                                  const std::vector<std::wstring>& gameDirs = {}) {
    std::wstring summary;
    for (const auto& item : AddonItems(root)) {
        const std::wstring name = item.filename().wstring();
        if (!titleId.empty() && item.wstring().find(titleId) == std::wstring::npos) continue;
        const auto key = L"addon.done." + StableKey(ref new Platform::String(AddonSignature(item).c_str()));
        if (!Pref(key, L"")->IsEmpty()) {
            // The marker is a setting, and settings come back from the drive
            // backup after a reinstall while Vita3K's data folder (where the
            // update or DLC actually lives) starts empty. Trust the marker
            // only when the content is still there.
            const std::wstring itemTitle = TitleIdIn(name);
            const bool present = itemTitle.empty()
                || HasFiles(FirmwareRoot() + L"\\ux0\\patch\\" + itemTitle)
                || HasFiles(FirmwareRoot() + L"\\ux0\\addcont\\" + itemTitle);
            if (present) continue;
            host.LogLine("Updates and DLC: " + NarrowUtf8(name) + " was marked installed but its files are gone (app data was reset); installing again");
        }
        ProvideUpdateLicense(host, item, gameDirs);
        std::string id, category;
        std::wstring detail;
        host.LogLine("Updates and DLC: installing " + NarrowUtf8(item.wstring()));
        if (host.InstallAddon(system, NarrowUtf8(item.wstring()), id, category, detail)) {
            PutPref(key, L"1");
            ++installed;
            const wchar_t* kind = category.find("gp") != std::string::npos ? L"update" :
                                  category.find("ac") != std::string::npos ? L"DLC" : L"content";
            summary += name + L": " + kind + L" for " + WidenUtf8(id) + L" installed.\n";
            host.LogLine("Updates and DLC: installed " + NarrowUtf8(name) + " (" + id + ", " + category + ")");
        } else {
            ++failed;
            summary += name + L": not installed. " + ProblemLine(detail) + L"\n";
            host.LogLine("Updates and DLC: " + NarrowUtf8(name) + " failed: " + NarrowUtf8(detail));
        }
    }
    return summary;
}
}

void CatalogShell::ChooseAddonsFolder() {
    auto picker = ref new FolderPicker();
    picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
    picker->FileTypeFilter->Append(L"*");
    create_task(picker->PickSingleFolderAsync()).then([this](task<StorageFolder^> picked) {
        StorageFolder^ folder = nullptr;
        try { folder = picked.get(); } catch (Platform::Exception^ e) { SetStatus(L"Folder picker failed: " + e->Message); }
        if (folder == nullptr) return;
        StorageApplicationPermissions::FutureAccessList->AddOrReplace(L"vita-addons", folder);
        PutPref(L"storage.addons", folder->Path);
        RestorePaths();
        InstallAddons();
    }, task_continuation_context::use_current());
}

void CatalogShell::InstallAddons() {
    const std::wstring root = Pref(L"storage.addons", L"")->Data();
    if (root.empty()) {
        SetStatus(L"Choose an updates folder first.");
        return;
    }
    if (addonsBusy_ || firmwareBusy_ || booting_ || libretro_.GameLoaded()) {
        SetStatus(L"Wait until the game or the current install has finished.");
        return;
    }
    addonsBusy_ = true;
    SetStatus(L"Looking for new updates and DLC...");
    const std::string system = NarrowUtf8(DataRoot());
    std::vector<std::wstring> gameDirs;
    for (const auto& game : games_) {
        if (game.directory != nullptr) gameDirs.push_back(game.directory->Path->Data());
        else if (game.file != nullptr) gameDirs.push_back(std::filesystem::path(game.file->Path->Data()).parent_path().wstring());
    }
    create_task([this, root, system, gameDirs]() {
        int installed = 0, failed = 0;
        std::wstring summary = InstallPendingAddons(libretro_, system, root, std::wstring(), installed, failed, gameDirs);
        if (installed == 0 && failed == 0) summary = L"No new updates or DLC in the folder.";
        return summary;
    }).then([this](task<std::wstring> done) {
        addonsBusy_ = false;
        std::wstring summary;
        try { summary = done.get(); } catch (...) { summary = L"Installing updates and DLC stopped. vita3k-runtime.log has the details."; }
        while (!summary.empty() && summary.back() == L'\n') summary.pop_back();
        SetStatus(ref new Platform::String(summary.c_str()));
    }, task_continuation_context::use_current());
}

void CatalogShell::ChangeDataRoot(const std::wstring& chosen) {
    if (libretro_.GameLoaded() || booting_ || firmwareBusy_) {
        SetStatus(L"Wait until the game or the firmware install has finished, then change the data folder.");
        return;
    }
    const std::wstring from = DataRoot();
    const std::wstring to = chosen.empty() ? InternalDataRoot() : chosen;
    if (_wcsicmp(from.c_str(), to.c_str()) == 0) {
        SetStatus(L"ReVita data is already there.");
        return;
    }
    if (!chosen.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(to, ec);
        if (!std::filesystem::is_directory(to, ec)) {
            SetStatus(ref new Platform::String((L"Can't create " + to + L". Check that the drive is connected and writable.").c_str()));
            return;
        }
    }
    auto apply = [this,chosen,to]() {
        PutPref(L"storage.dataRoot", ref new Platform::String(chosen.c_str()));
        SetStatus(ref new Platform::String((L"ReVita data now lives in " + to + L".").c_str()));
        Render();
    };
    if (!HasFiles(from + L"\\vita3k")) {
        apply();
        return;
    }
    auto dialog = ref new ContentDialog();
    dialog->Title = L"Move your ReVita data?";
    dialog->Content = ref new Platform::String((L"Firmware, installed games and saves are in " + from +
        L". Move them to " + to + L"? Large games take a while.").c_str());
    dialog->PrimaryButtonText = L"Move";
    dialog->SecondaryButtonText = L"Start fresh there";
    dialog->CloseButtonText = L"Cancel";
    create_task(dialog->ShowAsync()).then([this,from,to,apply](ContentDialogResult answer) {
        if (answer == ContentDialogResult::Secondary) { apply(); return; }
        if (answer != ContentDialogResult::Primary) return;
        booting_ = true; // keeps games from starting while files move
        SetStatus(L"Moving ReVita data. Leave the app open until it finishes...");
        create_task([from,to]() -> std::wstring {
            std::error_code ec;
            std::filesystem::create_directories(to + L"\\vita3k", ec);
            std::filesystem::copy(from + L"\\vita3k", to + L"\\vita3k",
                std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) return L"Moving failed: " + WidenUtf8(ec.message()) + L". Nothing was removed from the old place.";
            std::filesystem::remove_all(from + L"\\vita3k", ec);
            return std::wstring();
        }).then([this,apply](task<std::wstring> moved) {
            booting_ = false;
            std::wstring error;
            try { error = moved.get(); } catch (...) { error = L"Moving failed."; }
            if (!error.empty()) { SetStatus(ref new Platform::String(error.c_str())); return; }
            apply();
        }, task_continuation_context::use_current());
    }, task_continuation_context::use_current());
}

void CatalogShell::EmulationSettings() {
    Section(L"Emulation", L"The Vita3K engine's own options. They take effect when a game starts.");
    {
        // What draws the game, and how it fills the screen.
        auto display = Vertical();
        display->Children->Append(Text(L"Display", 16, true));

        auto screenRow = ref new Grid();
        screenRow->Margin = Thickness(0, 14, 0, 0);
        auto l2 = ref new ColumnDefinition();
        l2->Width = GridLength(1, GridUnitType::Star);
        auto r2 = ref new ColumnDefinition();
        r2->Width = GridLength(1, GridUnitType::Auto);
        screenRow->ColumnDefinitions->Append(l2);
        screenRow->ColumnDefinitions->Append(r2);
        auto screenCopy = Vertical();
        screenCopy->Children->Append(Text(L"Screen", 14, true));
        auto screenInfo = Text(L"Fit keeps the Vita's shape with thin bars; Stretch fills the whole TV.", 12, false, true);
        screenInfo->Margin = Thickness(0, 3, 24, 0);
        screenCopy->Children->Append(screenInfo);
        screenRow->Children->Append(screenCopy);
        auto screen = ref new ComboBox();
        screen->Width = 240;
        screen->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        screen->Items->Append(L"Fit (keep shape)");
        screen->Items->Append(L"Stretch to fill");
        screen->SelectedIndex = Pref(L"display.stretch", L"0")->Equals(L"1") ? 1 : 0;
        screen->SelectionChanged += ref new SelectionChangedEventHandler([this](Platform::Object^ sender, SelectionChangedEventArgs^) {
            PutPref(L"display.stretch", safe_cast<ComboBox^>(sender)->SelectedIndex == 1 ? L"1" : L"0");
            PlayCue(1);
        });
        Grid::SetColumn(screen, 1);
        screenRow->Children->Append(screen);
        display->Children->Append(screenRow);
        content_->Children->Append(Panel(display));
    }
    std::wstring current;
    StackPanel^ group = nullptr;
    for (std::size_t i = 0; i < vita3k_uwp::kCoreOptionCount; ++i) {
        const auto& option = vita3k_uwp::kCoreOptions[i];
        if (group == nullptr || current != option.category) {
            if (group != nullptr) content_->Children->Append(Panel(group));
            group = Vertical();
            current = option.category;
            group->Children->Append(Text(ref new Platform::String(option.category), 16, true));
        }
        auto row = ref new Grid();
        row->Margin = Thickness(0, 14, 0, 0);
        auto left = ref new ColumnDefinition();
        left->Width = GridLength(1, GridUnitType::Star);
        auto right = ref new ColumnDefinition();
        right->Width = GridLength(1, GridUnitType::Auto);
        row->ColumnDefinitions->Append(left);
        row->ColumnDefinitions->Append(right);
        auto copy = Vertical();
        copy->Children->Append(Text(ref new Platform::String(option.label), 14, true));
        if (option.info && *option.info) {
            auto info = Text(ref new Platform::String(option.info), 12, false, true);
            info->Margin = Thickness(0, 3, 24, 0);
            info->MaxLines = 3;
            copy->Children->Append(info);
        }
        row->Children->Append(copy);
        auto picker = ref new ComboBox();
        picker->Width = 240;
        picker->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        const std::string saved = SavedCoreOption(option);
        for (std::size_t v = 0; v < option.valueCount; ++v) {
            picker->Items->Append(ref new Platform::String(option.values[v].label));
            if (saved == option.values[v].value) picker->SelectedIndex = static_cast<int>(v);
        }
        picker->SelectionChanged += ref new SelectionChangedEventHandler(
            [this,i](Platform::Object^ sender, SelectionChangedEventArgs^) {
                const int chosen = safe_cast<ComboBox^>(sender)->SelectedIndex;
                const auto& changed = vita3k_uwp::kCoreOptions[i];
                if (chosen < 0 || static_cast<std::size_t>(chosen) >= changed.valueCount) return;
                const char* value = changed.values[chosen].value;
                std::wstring wide;
                for (const char* c = value; *c; ++c) wide.push_back(static_cast<wchar_t>(*c));
                PutPref(CoreOptionPrefKey(changed), ref new Platform::String(wide.c_str()));
                libretro_.SetOption(changed.key, value);
                PlayCue(1);
            });
        Grid::SetColumn(picker, 1);
        row->Children->Append(picker);
        group->Children->Append(row);
    }
    if (group != nullptr) content_->Children->Append(Panel(group));
    auto reset = Button(L"Restore default emulation options");
    reset->Margin = Thickness(0, 12, 0, 0);
    reset->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) {
        for (std::size_t i = 0; i < vita3k_uwp::kCoreOptionCount; ++i) {
            const auto& option = vita3k_uwp::kCoreOptions[i];
            ApplicationData::Current->LocalSettings->Values->Remove(CoreOptionPrefKey(option));
            libretro_.SetOption(option.key, option.defaultValue);
        }
        SetStatus(L"Emulation options restored to their defaults.");
        Render();
    });
    content_->Children->Append(reset);
}
void CatalogShell::RefreshFirmwareStatus() {
    const std::wstring root = FirmwareRoot();
    for (int i = 0; i < 3; ++i) {
        if (firmwareStatus_[i] == nullptr) continue;
        const FirmwarePart& part = kFirmwareParts[i];
        const bool installed = HasFiles(root + L"\\" + part.partition);
        auto version = Pref(ref new Platform::String(part.versionKey), L"");
        firmwareStatus_[i]->Text = installed
            ? (version->IsEmpty() ? ref new Platform::String(L"Installed") : L"Installed  \u00B7  " + version)
            : ref new Platform::String(L"Not installed");
        const Color color = installed ? RgbColor(111, 227, 154)
            : part.required ? Accent() : RgbColor(240, 199, 94);
        if (firmwareDot_[i] != nullptr) firmwareDot_[i]->Background = Solid(color);
    }
}
void CatalogShell::ResetInstalledFirmware() {
    if (firmwareBusy_ || libretro_.GameLoaded() || booting_) {
        SetStatus(L"Wait until the game or the firmware install has finished.");
        return;
    }
    ShowFirmwareProgress(-1, L"Removing installed firmware...");
    const std::wstring root = FirmwareRoot();
    create_task([root]() {
        for (const wchar_t* part : { L"os0", L"vs0", L"pd0", L"sa0", L"PUP_DEC" }) {
            std::error_code ec;
            std::filesystem::remove_all(root + L"\\" + part, ec);
        }
    }).then([this](task<void> done) {
        try { done.get(); } catch (...) {}
        auto values = ApplicationData::Current->LocalSettings->Values;
        for (const auto& part : kFirmwareParts) {
            auto key = ref new Platform::String(part.versionKey);
            if (values->HasKey(key)) values->Remove(key);
        }
        ShowFirmwareProgress(-1, L"Installed firmware removed. Install it again with Install firmware files.");
        RefreshFirmwareStatus();
    }, task_continuation_context::use_current());
}
void CatalogShell::ScanGames() {
    auto access = StorageApplicationPermissions::FutureAccessList;
    if (!access->ContainsItem(L"vita-games")) {
        const auto revision = ++libraryRevision_;
        games_.clear();
        RestorePinnedGameFiles(revision);
        Render();
        SetStatus(L"Choose a folder or add individual Vita VPK, ZIP, PKG, VCI or eboot.bin files.");
        return;
    }
    SetStatus(L"Scanning up to 300 entries and checking supported Vita source formats...");
    create_task(access->GetFolderAsync(L"vita-games")).then(
        [](StorageFolder^ folder) {
            // The chosen folder may be one game rather than a folder of games
            // (its eboot.bin is right there). Then it is the only entry.
            return create_task(folder->TryGetItemAsync(L"eboot.bin")).then(
                [folder](IStorageItem^ eboot) -> task<Windows::Foundation::Collections::IVectorView<IStorageItem^>^> {
                    if (eboot != nullptr) {
                        auto single = ref new Platform::Collections::Vector<IStorageItem^>();
                        single->Append(folder);
                        return task_from_result(single->GetView());
                    }
                    return create_task(folder->GetItemsAsync(0, 300));
                });
        }).then([this](task<Windows::Foundation::Collections::IVectorView<IStorageItem^>^> work) {
            try {
                auto items = work.get();
                ++libraryRevision_;
                const unsigned revision = libraryRevision_;
                games_.clear();
                for (auto item : items) {
                    const bool directory = item->IsOfType(StorageItemTypes::Folder);
                    auto file = directory ? nullptr : safe_cast<StorageFile^>(item);
                    const auto format = directory ? vita3k_uwp::GameFormat::GameFolder :
                        vita3k_uwp::ClassifyGameFile(std::wstring(item->Name->Data()));
                    if (format == vita3k_uwp::GameFormat::Unknown) continue;
                    LibraryGame game;
                    game.name = directory ? item->Name : PrettyFileName(item->Name);
                    game.key = StableKey(item->Path);
                    game.sourceType = ref new Platform::String(vita3k_uwp::GameFormatLabel(format));
                    game.format = format;
                    game.validation = directory ? L"Checking for eboot.bin in game folder..." :
                        L"Checking file signature...";
                    game.directory = directory ? safe_cast<StorageFolder^>(item) : nullptr;
                    game.file = file;
                    game.icon = nullptr;
                    games_.push_back(game);
                }
                SetStatus(StringOf(std::to_wstring(games_.size())) +
                    L" Vita-format candidates found. Checking game headers...");
                Render();
                for (size_t i = 0; i < games_.size(); ++i) {
                    if (games_[i].directory != nullptr) {
                        CheckGameFolder(i, revision);
                        ReadSfo(i, games_[i].directory);
                    } else {
                        CheckGameFile(i, revision);
                    }
                }
                RestorePinnedGameFiles(revision);
            } catch (Platform::Exception^ e) {
                SetStatus(L"Unable to scan Vita library: " + e->Message);
            }
        }, task_continuation_context::use_current());
}
void CatalogShell::CheckGameFolder(size_t index, unsigned revision) {
    if (index >= games_.size() || revision != libraryRevision_) return;
    auto folder = games_[index].directory;
    if (folder == nullptr) return;
    create_task(folder->GetFileAsync(L"eboot.bin")).then(
        [this,index,revision](task<StorageFile^> work) {
            if (revision != libraryRevision_ || index >= games_.size()) return;
            try {
                games_[index].file = work.get();
                games_[index].validation = L"EBOOT.BIN present. Checking executable format...";
                CheckGameFile(index, revision);
            } catch (Platform::Exception^) {
                games_[index].validation =
                    L"Not an extracted Vita game root: eboot.bin is missing.";
                Render();
            }
        }, task_continuation_context::use_current());
}
void CatalogShell::CheckGameFile(size_t index, unsigned revision) {
    if (revision != libraryRevision_ || index >= games_.size()) return;
    auto file = games_[index].file;
    if (file == nullptr) return;
    const auto format = games_[index].format == vita3k_uwp::GameFormat::GameFolder ?
        vita3k_uwp::GameFormat::ExecutableBin : games_[index].format;
    // Read only a small header; never load an entire multi-gigabyte commercial PKG.
    create_task(file->OpenAsync(FileAccessMode::Read)).then(
        [this,index,revision,format](IRandomAccessStream^ stream) {
            const auto bytes = static_cast<unsigned>(
                std::min<std::uint64_t>(stream->Size, 64));
            auto reader = ref new DataReader(stream->GetInputStreamAt(0));
            return create_task(reader->LoadAsync(bytes)).then(
                [this,index,revision,format,reader](unsigned loaded) {
                    if (revision != libraryRevision_ || index >= games_.size()) return;
                    auto buffer = ref new Platform::Array<unsigned char>(loaded);
                    if (loaded > 0) reader->ReadBytes(buffer);
                    const auto check = vita3k_uwp::CheckGameHeader(
                        format, buffer->Data, static_cast<size_t>(loaded));
                    std::wstring diagnostic;
                    for (const char* p = check.diagnostic; *p; ++p)
                        diagnostic.push_back(static_cast<wchar_t>(*p));
                    games_[index].validation = StringOf(diagnostic);
                    Render();
                });
        }).then([this,index,revision](task<void> work) {
            try { work.get(); }
            catch (Platform::Exception^ error) {
                if (revision != libraryRevision_ || index >= games_.size()) return;
                games_[index].validation = L"Unable to inspect game header: " + error->Message;
                Render();
            }
        }, task_continuation_context::use_current());
}

void CatalogShell::ChooseLicenseForGame(size_t index) {
    if (index >= games_.size() || games_[index].format != vita3k_uwp::GameFormat::Pkg) return;
    const auto title = games_[index].name;
    const auto token = L"vita-license-" + games_[index].key;
    auto picker = ref new FileOpenPicker();
    picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
    picker->FileTypeFilter->Append(L".rif");
    picker->FileTypeFilter->Append(L".zrif");
    picker->FileTypeFilter->Append(L".bin");
    create_task(picker->PickSingleFileAsync()).then(
        [this,token,title](task<StorageFile^> work) {
            try {
                auto file = work.get();
                if (file == nullptr) return;
                StorageApplicationPermissions::FutureAccessList->AddOrReplace(token, file);
                SetStatus(L"License file access saved for " + title +
                    L". PKG installation and decryption are not implemented.");
                Render();
            } catch (Platform::Exception^ error) {
                SetStatus(L"Could not select PKG license: " + error->Message);
            }
        }, task_continuation_context::use_current());
}
void CatalogShell::InspectLaunch(size_t index) {
    if (index >= games_.size()) return;
    PlayCue(1);
    // This is a real source-file preflight, not an emulated boot or game process.
    if (games_[index].file == nullptr) {
        SetStatus(L"Boot preflight: game executable is missing or unreadable.");
        return;
    }
    if (games_[index].format == vita3k_uwp::GameFormat::Pkg &&
        !StorageApplicationPermissions::FutureAccessList->ContainsItem(
            L"vita-license-" + games_[index].key)) {
        SetStatus(L"PKG needs the title's matching legitimate license. "
            L"The Vita3K CPU, installer, kernel and GXM renderer are still missing.");
        return;
    }
    SetStatus(L"Input preflight: " + games_[index].validation +
        L". Game boot is blocked: Dynarmic CPU, Vita kernel, executable loader "
        L"and GXM renderer are not integrated in this UWP build.");
}

namespace {
// Progress of one staging copy, shared by its tasks.
struct StageProgress {
    std::atomic<uint32_t> files{0};
    std::atomic<uint32_t> skipped{0};
    std::atomic<uint64_t> bytes{0};
};

// Copies one file unless an identical-sized copy is already there, so a game
// that was staged before starts without copying gigabytes again, and an
// interrupted copy resumes where it stopped.
task<void> StageFile(StorageFile^ source, StorageFolder^ target, std::shared_ptr<StageProgress> progress) {
    return create_task(target->TryGetItemAsync(source->Name))
    .then([source,target,progress](IStorageItem^ existing) -> task<void> {
        auto copy = [source,target,progress]() {
            return create_task(source->CopyAsync(target, source->Name, NameCollisionOption::ReplaceExisting))
                .then([progress](StorageFile^) { progress->files++; });
        };
        auto existingFile = dynamic_cast<StorageFile^>(existing);
        if (existingFile == nullptr) return copy();
        return create_task(source->GetBasicPropertiesAsync())
        .then([existingFile](FileProperties::BasicProperties^ from) {
            return create_task(existingFile->GetBasicPropertiesAsync())
                .then([from](FileProperties::BasicProperties^ to) { return from->Size == to->Size; });
        }).then([copy,progress](bool same) -> task<void> {
            if (same) { progress->skipped++; return task_from_result(); }
            return copy();
        });
    });
}

task<void> StageFolder(StorageFolder^ source, StorageFolder^ target, std::shared_ptr<StageProgress> progress,
                       std::function<void()> report) {
    return create_task(source->GetFilesAsync())
    .then([source,target,progress,report](Collections::IVectorView<StorageFile^>^ files) {
        task<void> chain = task_from_result();
        for (auto file : files) {
            chain = chain.then([file,target,progress,report]() {
                return StageFile(file, target, progress).then([report]() { report(); });
            });
        }
        return chain;
    }).then([source]() { return create_task(source->GetFoldersAsync()); })
    .then([target,progress,report](Collections::IVectorView<StorageFolder^>^ folders) {
        task<void> chain = task_from_result();
        for (auto folder : folders) {
            chain = chain.then([folder,target,progress,report]() {
                return create_task(target->CreateFolderAsync(folder->Name, CreationCollisionOption::OpenIfExists))
                    .then([folder,progress,report](StorageFolder^ child) {
                        return StageFolder(folder, child, progress, report);
                    });
            });
        }
        return chain;
    });
}

std::string Utf8Of(Platform::String^ source) {
    const int count = WideCharToMultiByte(CP_UTF8, 0, source->Data(), -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) return std::string();
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, source->Data(), -1, &result[0], count, nullptr, nullptr);
    result.resize(static_cast<size_t>(count - 1));
    return result;
}
}

void CatalogShell::AttemptBoot(size_t index) {
    if (index >= games_.size()) return;
    if (booting_) {
        SetStatus(L"A game is already starting. Wait for it to finish.");
        return;
    }
    PutPref(L"lastPlayed", games_[index].name);
    NoteRecent(index);
    auto folder = games_[index].directory;
    // A folder game also carries its eboot.bin; the whole folder is what runs.
    auto file = folder != nullptr ? nullptr : games_[index].file;
    if (file == nullptr && folder == nullptr) {
        SetStatus(L"This entry has no game file or folder to start.");
        return;
    }
    if (file != nullptr && _wcsicmp(file->Name->Data(), L"eboot.bin") == 0) {
        SetStatus(L"An eboot.bin can't start on its own. Add the game's whole folder with Choose a folder.");
        return;
    }
    std::wstring probe;
    if (!libretro_.Probe(probe)) {
        SetStatus(StringOf(probe));
        return;
    }
    if (!HasFiles(FirmwareRoot() + L"\\vs0")) {
        SetStatus(L"Install the system firmware first: Settings, Firmware, Install firmware files.");
        ShowPage(2);
        return;
    }
    // Games are read where they are (internal storage or a USB drive): the
    // core reaches them through the FromApp file redirect. Only a place it
    // still cannot read falls back to copying the game into app storage.
    const std::wstring direct = folder != nullptr ? folder->Path->Data() : file->Path->Data();
    const std::wstring check = folder != nullptr ? direct + L"\\sce_sys\\param.sfo" : direct;
    if (CanOpenFromApp(check)) {
        booting_ = true;
        SetStatus(ref new Platform::String((L"Starting " + std::wstring(games_[index].name->Data()) + L"...").c_str()));
        StartCore(ref new Platform::String(direct.c_str()));
        return;
    }
    booting_ = true;
    // Vita3K reads games with ordinary file APIs, which UWP only allows inside
    // the app's own storage, so the game is copied there first. The copy keeps
    // the original names (the core recognises eboot.bin and folder layouts by
    // name) and is reused on the next start.
    auto progress = std::make_shared<StageProgress>();
    auto dispatcher = Dispatcher;
    auto lastReport = std::make_shared<std::atomic<uint32_t>>(0);
    std::function<void()> report = [this,progress,dispatcher,lastReport]() {
        const uint32_t done = progress->files + progress->skipped;
        if (done - lastReport->load() < 10) return;
        lastReport->store(done);
        dispatcher->RunAsync(Windows::UI::Core::CoreDispatcherPriority::Low,
            ref new Windows::UI::Core::DispatchedHandler([this,progress]() {
                SetStatus(L"Copying the game into ReVita storage: " +
                    StringOf(std::to_wstring(progress->files.load())) + L" copied, " +
                    StringOf(std::to_wstring(progress->skipped.load())) + L" already there...");
            }));
    };
    SetStatus(L"ReVita can't read this game where it is, so it is being copied into app storage first...");
    create_task(ApplicationData::Current->LocalFolder->CreateFolderAsync(
        L"Vita3KContent", CreationCollisionOption::OpenIfExists))
    .then([file,folder,progress,report](StorageFolder^ content) -> task<Platform::String^> {
        if (file != nullptr) {
            return create_task(content->CreateFolderAsync(L"files", CreationCollisionOption::OpenIfExists))
            .then([file,progress](StorageFolder^ files) {
                // eboot.bin is only meaningful inside its game folder.
                return StageFile(file, files, progress).then([files,file]() {
                    return files->Path + L"\\" + file->Name;
                });
            });
        }
        return create_task(content->CreateFolderAsync(folder->Name, CreationCollisionOption::OpenIfExists))
        .then([folder,progress,report](StorageFolder^ target) {
            return StageFolder(folder, target, progress, report).then([target]() { return target->Path; });
        });
    }).then([this,progress](task<Platform::String^> staged) {
        try {
            auto path = staged.get();
            SetStatus(L"Game ready (" + StringOf(std::to_wstring(progress->files.load())) +
                L" files copied, " + StringOf(std::to_wstring(progress->skipped.load())) +
                L" reused). Starting...");
            StartCore(path);
        } catch (Platform::Exception^ error) {
            booting_ = false;
            SetStatus(L"Couldn't copy the game into ReVita storage: " + error->Message +
                L" Check free space on the console.");
        } catch (...) {
            booting_ = false;
            SetStatus(L"Couldn't copy the game into ReVita storage.");
        }
    }, task_continuation_context::use_current());
}

void CatalogShell::EnterGameView(Platform::String^ title) {
    inGame_ = true;
    if (xmb_ != nullptr) {
        xmb_->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        if (xmbTimer_) xmbTimer_->Stop();
        xmbRibbons_.SetActive(false); // nothing animates behind a game
    }
    gameTitle_ = title;
    quickMenu_ = nullptr;
    quickMenuOpen_ = false;
    gameStarted_ = false;
    loadFailed_ = false;
    loadingHidden_ = false;
    gameRunningSince_ = 0;
    pictureSeen_ = false;
    framesShown_ = 0;
    lastLevel_ = 0;
    lastLit_ = 0;
    {
        std::lock_guard<std::mutex> guard(presentErrorMutex_);
        presentError_.clear();
    }
    // The game owns the whole screen: the menus go away (and can't take
    // focus or controller input), the menu music stops.
    navigation_->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    if (contentScroll_) contentScroll_->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    if (statusBar_) statusBar_->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    musicPlayer_->Pause();
    // Nothing of the menu scene runs under a game.
    backdrop_.SetActive(false);
    if (clockTimer_) clockTimer_->Stop();

    gameLayer_ = ref new Grid();
    gameLayer_->Background = Solid(Colors::Black);
    Grid::SetColumnSpan(gameLayer_, 2);
    Grid::SetRowSpan(gameLayer_, 2);
    renderSurface_ = ref new SwapChainPanel();
    renderSurface_->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
    renderSurface_->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Stretch;
    renderSurface_->SizeChanged += ref new SizeChangedEventHandler([this](Platform::Object^, SizeChangedEventArgs^ e) {
        renderer_.SetViewport(static_cast<float>(e->NewSize.Width), static_cast<float>(e->NewSize.Height),
            Pref(L"display.stretch", L"0")->Equals(L"1"));
    });
    gameLayer_->Children->Append(renderSurface_);

    // Loading screen, over the game until it shows its first picture: the
    // game's own picture across the screen, its cover and name at the lower
    // left, the progress under them.
    int gameIndex = -1;
    for (size_t i = 0; i < games_.size(); ++i)
        if (title != nullptr && games_[i].name == title) { gameIndex = static_cast<int>(i); break; }
    loadingLayer_ = ref new Grid();
    loadingLayer_->Background = Gradient(Bg(), Glow());
    if (gameIndex >= 0) {
        Platform::String^ wide = BackdropUri(static_cast<size_t>(gameIndex));
        if (wide->IsEmpty()) wide = ArtworkUri(static_cast<size_t>(gameIndex));
        if (!wide->IsEmpty()) {
            try {
                auto art = ref new Image();
                art->Source = CachedBitmap(wide, 1600);
                art->Stretch = Stretch::UniformToFill;
                art->Opacity = 0.55;
                loadingLayer_->Children->Append(art);
            } catch (Platform::Exception^) {}
        }
    }
    {
        auto shade = ref new LinearGradientBrush();
        shade->StartPoint = Point(0.5, 0);
        shade->EndPoint = Point(0.5, 1);
        const Color page = Bg();
        const double stops[][2] = { { 0.0, 120 }, { 0.45, 150 }, { 1.0, 250 } };
        for (const auto& s : stops) {
            auto stop = ref new GradientStop();
            stop->Offset = s[0];
            stop->Color = Alpha(static_cast<unsigned>(s[1]), page.R, page.G, page.B);
            shade->GradientStops->Append(stop);
        }
        auto veil = ref new Border();
        veil->Background = shade;
        loadingLayer_->Children->Append(veil);
    }
    auto block = ref new StackPanel();
    block->Orientation = Orientation::Horizontal;
    block->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
    block->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Bottom;
    block->Margin = Thickness(80, 0, 80, 84);
    if (gameIndex >= 0) {
        auto cover = ref new Border();
        cover->Width = 132;
        cover->Height = 176;
        cover->CornerRadius = Windows::UI::Xaml::CornerRadius(12);
        cover->BorderBrush = Solid(Alpha(46, 255, 255, 255));
        cover->BorderThickness = Thickness(1);
        cover->Margin = Thickness(0, 0, 30, 0);
        cover->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Bottom;
        ConfigureCover(cover, static_cast<size_t>(gameIndex));
        block->Children->Append(cover);
    }
    auto column = Vertical();
    column->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Bottom;
    column->MaxWidth = 720;
    auto kicker = Text(L"NOW LOADING", 11, true);
    kicker->CharacterSpacing = 300;
    kicker->Foreground = BrandBrush();
    column->Children->Append(kicker);
    auto name = Text(title, 42, false);
    name->FontWeight = FontWeights::Light;
    name->MaxLines = 2;
    name->Margin = Thickness(0, 8, 0, 20);
    column->Children->Append(name);
    loadingBar_ = ref new ProgressBar();
    loadingBar_->Width = 520;
    loadingBar_->Height = 4;
    loadingBar_->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
    loadingBar_->Minimum = 0;
    loadingBar_->Maximum = 100;
    loadingBar_->IsIndeterminate = true;
    loadingBar_->Foreground = BrandBrush();
    loadingBar_->Background = Solid(Alpha(40, 255, 255, 255));
    column->Children->Append(loadingBar_);
    loadingStage_ = Text(L"Getting ready", 14, false, true);
    loadingStage_->Margin = Thickness(0, 12, 0, 0);
    column->Children->Append(loadingStage_);
    block->Children->Append(column);
    loadingLayer_->Children->Append(block);
    loadingHint_ = Text(L"View + Menu together: quick menu", 11, false, true);
    loadingHint_->Opacity = 0.75;
    loadingHint_->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Right;
    loadingHint_->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Bottom;
    loadingHint_->Margin = Thickness(0, 0, 80, 40);
    loadingHint_->MaxWidth = 640;
    loadingHint_->TextWrapping = TextWrapping::Wrap;
    loadingHint_->TextAlignment = TextAlignment::Right;
    loadingLayer_->Children->Append(loadingHint_);
    gameLayer_->Children->Append(loadingLayer_);
    // The performance overlay: a small, quiet pill in the top-left corner.
    perfOverlay_ = ref new Border();
    perfOverlay_->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
    perfOverlay_->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Top;
    perfOverlay_->Margin = Thickness(22, 16, 0, 0);
    perfOverlay_->Padding = Thickness(10, 3, 10, 4);
    perfOverlay_->CornerRadius = Windows::UI::Xaml::CornerRadius(8);
    perfOverlay_->Background = Solid(Alpha(105, 0, 0, 0));
    perfOverlay_->IsHitTestVisible = false;
    perfText_ = Text(L"", 12, true);
    perfText_->TextWrapping = TextWrapping::NoWrap;
    perfText_->Foreground = Solid(Alpha(225, 255, 255, 255));
    perfOverlay_->Child = perfText_;
    gameLayer_->Children->Append(perfOverlay_);
    ApplyPerfMode();
    root_->Children->Append(gameLayer_);

    if (!gameTimer_) {
        gameTimer_ = ref new DispatcherTimer();
        TimeSpan interval;
        interval.Duration = 2000000; // 200 ms
        gameTimer_->Interval = interval;
        gameTimer_->Tick += ref new EventHandler<Platform::Object^>([this](Platform::Object^, Platform::Object^) { GameTick(); });
    }
    gameTimer_->Start();
}

void CatalogShell::GameTick() {
    if (!inGame_ || loadFailed_) return;
    if (gameStarted_ && !libretro_.Running()) {
        // View + Menu, or the core stopped on its own.
        ExitGameView();
        return;
    }
    if (!gameStarted_) return;
    UpdatePerfOverlay();
    if (loadingHidden_) { UpdateGameStats(); return; }
    const auto now = GetTickCount64();
    if (!gameRunningSince_) gameRunningSince_ = now;
    if (pictureSeen_.load() || now - gameRunningSince_ > 60000) {
        // The game is drawing: fade the loading screen out.
        loadingHidden_ = true;
        auto fade = ref new DoubleAnimation();
        fade->From = 1.0;
        fade->To = 0.0;
        fade->Duration = DurationHelper::FromTimeSpan(Millis(350));
        auto story = ref new Storyboard();
        Storyboard::SetTarget(fade, loadingLayer_);
        Storyboard::SetTargetProperty(fade, L"Opacity");
        story->Children->Append(fade);
        story->Completed += ref new EventHandler<Platform::Object^>([this](Platform::Object^, Platform::Object^) {
            if (loadingLayer_) loadingLayer_->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        });
        story->Begin();
        return;
    }
    if (now - gameRunningSince_ > 8000 && loadingStage_) {
        loadingStage_->Text = L"The game is running and getting its first picture ready. Some games stay dark for a while at start.";
    }
    UpdateGameStats();
}

void CatalogShell::ApplyPerfMode() {
    const auto mode = Pref(L"overlay.mode", L"0");
    perfLastTick_ = 0;
    if (!perfOverlay_) return;
    const bool on = mode->Equals(L"1") || mode->Equals(L"2");
    perfOverlay_->Visibility = on ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
    if (perfText_) perfText_->Text = on ? L"\u2026" : L"";
}

void CatalogShell::UpdatePerfOverlay() {
    if (!perfOverlay_ || perfOverlay_->Visibility != Windows::UI::Xaml::Visibility::Visible) return;
    const bool detailed = Pref(L"overlay.mode", L"0")->Equals(L"2");
    const unsigned long long now = GetTickCount64();
    const unsigned long long shown = framesShown_.load();
    const unsigned long long guest = libretro_.GuestFrames();
    if (perfLastTick_ == 0 || guest < perfLastGuest_ || shown < perfLastShown_) {
        perfLastTick_ = now;
        perfLastShown_ = shown;
        perfLastGuest_ = guest;
        return;
    }
    const unsigned long long elapsed = now - perfLastTick_;
    if (elapsed < 1000) return;
    const double output = static_cast<double>(shown - perfLastShown_) * 1000.0 / static_cast<double>(elapsed);
    // The game's own frames when the core counts them, else what reached the screen
    const double game = guest ? static_cast<double>(guest - perfLastGuest_) * 1000.0 / static_cast<double>(elapsed) : output;
    perfLastTick_ = now;
    perfLastShown_ = shown;
    perfLastGuest_ = guest;
    wchar_t text[160] = {};
    if (!detailed) {
        swprintf_s(text, L"%.0f FPS", game);
    } else {
        double gigabytes = 0;
        try { gigabytes = static_cast<double>(Windows::System::MemoryManager::AppMemoryUsage) / (1024.0 * 1024.0 * 1024.0); } catch (...) {}
        const double frameMs = game > 0.5 ? 1000.0 / game : 0.0;
        swprintf_s(text, L"%.0f FPS  \u00B7  %.1f ms  \u00B7  Display %.0f  \u00B7  RAM %.2f GB", game, frameMs, output, gigabytes);
    }
    if (perfText_) perfText_->Text = ref new Platform::String(text);
}

void CatalogShell::UpdateGameStats() {
    std::wstring presenter;
    {
        std::lock_guard<std::mutex> guard(presentErrorMutex_);
        presenter = presentError_;
    }
    wchar_t text[256] = {};
    swprintf_s(text, L"Frames %llu  \u00B7  picture level %u (%u of 225 points lit)  \u00B7  display %s",
        framesShown_.load(), lastLevel_.load(), lastLit_.load(), presenter.empty() ? L"OK" : presenter.c_str());
    if (loadingHint_ && gameStarted_ && !loadFailed_)
        loadingHint_->Text = ref new Platform::String(text);
    if (statsLabel_) statsLabel_->Text = ref new Platform::String(text);
}

void CatalogShell::ShowLoadFailure(Platform::String^ detail) {
    loadFailed_ = true;
    if (gameTimer_) gameTimer_->Stop();
    if (!loadingLayer_) return;
    loadingLayer_->Visibility = Windows::UI::Xaml::Visibility::Visible;
    loadingLayer_->Opacity = 1.0;
    loadingBar_->IsIndeterminate = false;
    loadingBar_->Value = 0;
    loadingBar_->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    loadingStage_->Text = L"This game couldn't start.";
    std::wstring text = detail ? detail->Data() : L"";
    if (text.size() > 400) text = text.substr(0, 400) + L"...";
    loadingHint_->Text = ref new Platform::String((text + L"\nvita3k-runtime.log has the details.").c_str());
    loadingHint_->Opacity = 0.85;
    auto back = Button(L"Back to the library", true);
    back->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Center;
    back->Margin = Thickness(0, 24, 0, 0);
    back->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { ExitGameView(); });
    auto column = safe_cast<StackPanel^>(loadingLayer_->Children->GetAt(0));
    column->Children->Append(back);
    inGame_ = false; // the controller drives this button now
    back->Focus(Windows::UI::Xaml::FocusState::Programmatic);
}

void CatalogShell::ExitGameView() {
    if (gameTimer_) gameTimer_->Stop();
    inGame_ = false;
    libretro_.SetProgressSink(nullptr);
    libretro_.SetMenuSink(nullptr);
    DeleteFileW((std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\vita3k-session.txt").c_str());
    quickMenuOpen_ = false;
    quickMenu_ = nullptr;
    SetStatus(L"Closing the game...");
    // Unloading joins the core's threads; keep it off the UI thread.
    create_task([this]() {
        libretro_.StopRunning();
        libretro_.UnloadGame();
    }).then([this](task<void> done) {
        try { done.get(); } catch (...) {}
        renderer_.Shutdown();
        if (gameLayer_) {
            unsigned index = 0;
            if (root_->Children->IndexOf(gameLayer_, &index)) root_->Children->RemoveAt(index);
        }
        gameLayer_ = nullptr;
        loadingLayer_ = nullptr;
        loadingStage_ = nullptr;
        loadingHint_ = nullptr;
        loadingBar_ = nullptr;
        renderSurface_ = nullptr;
        statsLabel_ = nullptr;
        perfOverlay_ = nullptr;
        perfText_ = nullptr;
        booting_ = false;
        navigation_->Visibility = Windows::UI::Xaml::Visibility::Visible;
        if (contentScroll_) contentScroll_->Visibility = Windows::UI::Xaml::Visibility::Visible;
        if (statusBar_) statusBar_->Visibility = Windows::UI::Xaml::Visibility::Visible;
        backdrop_.SetActive(true);
        if (clockTimer_) clockTimer_->Start();
        if (track_ > 0) musicPlayer_->Play();
        SetStatus(L"Back in the library.");
        ShowPage(xmbMode_ ? 0 : 1);
    }, task_continuation_context::use_current());
}

void CatalogShell::ShowQuickMenu() {
    if (!inGame_ || !gameLayer_ || quickMenuOpen_) return;
    if (!gameStarted_ || loadFailed_) {
        // Still loading, or failed: nothing to pause.
        libretro_.SetPaused(false);
        return;
    }
    libretro_.SetPaused(true);
    quickMenuOpen_ = true;
    PlayCue(1);

    quickMenu_ = ref new Grid();
    // the paused game stays visible on the right, dimmed
    {
        auto dim = ref new LinearGradientBrush();
        dim->StartPoint = Point(0, 0.5);
        dim->EndPoint = Point(1, 0.5);
        auto s0 = ref new GradientStop(); s0->Offset = 0.0; s0->Color = Alpha(235, 4, 4, 8);
        auto s1 = ref new GradientStop(); s1->Offset = 0.45; s1->Color = Alpha(150, 4, 4, 8);
        auto s2 = ref new GradientStop(); s2->Offset = 1.0; s2->Color = Alpha(70, 4, 4, 8);
        dim->GradientStops->Append(s0);
        dim->GradientStops->Append(s1);
        dim->GradientStops->Append(s2);
        quickMenu_->Background = dim;
    }
    quickMenu_->Opacity = 0;

    // A sheet down the left side of the screen.
    auto card = ref new Border();
    card->Width = 460;
    card->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
    card->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Stretch;
    card->Padding = Thickness(56, 64, 40, 40);
    card->Background = Gradient(Card(), Alpha(235, Bg().R, Bg().G, Bg().B));
    card->BorderBrush = Solid(Alpha(28, 255, 255, 255));
    card->BorderThickness = Thickness(0, 0, 1, 0);

    auto column = Vertical(0);
    column->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
    {
        int shown = -1;
        for (size_t i = 0; i < games_.size(); ++i)
            if (gameTitle_ != nullptr && games_[i].name == gameTitle_) { shown = static_cast<int>(i); break; }
        if (shown >= 0) {
            auto cover = ref new Border();
            cover->Width = 96;
            cover->Height = 128;
            cover->CornerRadius = Windows::UI::Xaml::CornerRadius(10);
            cover->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
            cover->Margin = Thickness(0, 0, 0, 18);
            ConfigureCover(cover, static_cast<size_t>(shown));
            column->Children->Append(cover);
        }
    }
    auto kicker = Text(L"PAUSED", 10, true);
    kicker->CharacterSpacing = 300;
    kicker->Foreground = BrandBrush();
    column->Children->Append(kicker);
    auto name = Text(gameTitle_ ? gameTitle_ : ref new Platform::String(L"Your game"), 30, false);
    name->FontWeight = FontWeights::Light;
    name->TextWrapping = TextWrapping::Wrap;
    name->MaxLines = 2;
    name->Margin = Thickness(0, 4, 0, 6);
    column->Children->Append(name);
    if (gameRunningSince_) {
        const auto minutes = (GetTickCount64() - gameRunningSince_) / 60000ull;
        wchar_t played[64] = {};
        if (minutes < 1) swprintf_s(played, L"Playing for under a minute");
        else swprintf_s(played, L"Playing for %llu min", minutes);
        auto session = Text(ref new Platform::String(played), 12, false, true);
        session->Margin = Thickness(0, 0, 0, 22);
        column->Children->Append(session);
    } else {
        name->Margin = Thickness(0, 4, 0, 22);
    }

    auto add = [this,column](Platform::String^ label, bool highlighted) {
        auto button = Button(label, highlighted);
        button->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        button->HorizontalContentAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
        button->Padding = Thickness(18, 0, 18, 0);
        button->Height = 50;
        button->Margin = Thickness(0, 0, 0, 10);
        column->Children->Append(button);
        return button;
    };
    auto resume = add(L"\u25B6   Resume", true);
    resume->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { HideQuickMenu(); });

    const bool stretch = Pref(L"display.stretch", L"0")->Equals(L"1");
    auto screen = add(ref new Platform::String(stretch ? L"\u2B1A   Screen: Stretch" : L"\u2B1A   Screen: Fit"), false);
    screen->Click += ref new RoutedEventHandler([this,screen](Platform::Object^, RoutedEventArgs^) {
        const bool next = !Pref(L"display.stretch", L"0")->Equals(L"1");
        PutPref(L"display.stretch", ref new Platform::String(next ? L"1" : L"0"));
        screen->Content = ref new Platform::String(next ? L"\u2B1A   Screen: Stretch" : L"\u2B1A   Screen: Fit");
        if (renderSurface_)
            renderer_.SetViewport(static_cast<float>(renderSurface_->ActualWidth), static_cast<float>(renderSurface_->ActualHeight), next);
    });

    const wchar_t* perfLabels[] = { L"\u25F7   Performance overlay: Off", L"\u25F7   Performance overlay: FPS",
                                    L"\u25F7   Performance overlay: Detailed" };
    const auto perfMode = Pref(L"overlay.mode", L"0");
    const int perfIndex = perfMode->Equals(L"2") ? 2 : perfMode->Equals(L"1") ? 1 : 0;
    auto perf = add(ref new Platform::String(perfLabels[perfIndex]), false);
    perf->Click += ref new RoutedEventHandler([this,perf](Platform::Object^, RoutedEventArgs^) {
        const auto current = Pref(L"overlay.mode", L"0");
        const int next = current->Equals(L"2") ? 0 : current->Equals(L"1") ? 2 : 1;
        PutPref(L"overlay.mode", ref new Platform::String(std::to_wstring(next).c_str()));
        const wchar_t* labels[] = { L"\u25F7   Performance overlay: Off", L"\u25F7   Performance overlay: FPS",
                                    L"\u25F7   Performance overlay: Detailed" };
        perf->Content = ref new Platform::String(labels[next]);
        ApplyPerfMode();
    });

    auto log = add(L"\u2913   Save log to your drive", false);
    log->Click += ref new RoutedEventHandler([this,log](Platform::Object^, RoutedEventArgs^) {
        SaveLogToDrive();
        log->Content = ref new Platform::String(L"\u2713   Saving the log to your drive");
    });

    auto quit = add(L"\u2715   Quit game", false);
    quit->Margin = Thickness(0, 8, 0, 4);
    quit->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) {
        quickMenuOpen_ = false;
        ExitGameView();
    });

    auto hint = Text(L"B  Resume     \u00B7     A  Select", 11, false, true);
    hint->Opacity = 0.7;
    hint->Margin = Thickness(2, 14, 0, 0);
    column->Children->Append(hint);
    card->Child = column;
    quickMenu_->Children->Append(card);
    gameLayer_->Children->Append(quickMenu_);

    auto fade = ref new DoubleAnimation();
    fade->From = 0.0;
    fade->To = 1.0;
    fade->Duration = DurationHelper::FromTimeSpan(Millis(160));
    auto story = ref new Storyboard();
    Storyboard::SetTarget(fade, quickMenu_);
    Storyboard::SetTargetProperty(fade, L"Opacity");
    story->Children->Append(fade);
    story->Begin();
    Reveal(card, 0, 14);
    resume->Focus(Windows::UI::Xaml::FocusState::Programmatic);
}

void CatalogShell::HideQuickMenu() {
    if (!quickMenuOpen_) return;
    quickMenuOpen_ = false;
    if (gameLayer_ && quickMenu_) {
        unsigned index = 0;
        if (gameLayer_->Children->IndexOf(quickMenu_, &index)) gameLayer_->Children->RemoveAt(index);
    }
    quickMenu_ = nullptr;
    libretro_.SetPaused(false);
}

// A 32-bit BMP, top-down; the pixels' alpha is dropped.
static bool WriteBmp(const std::wstring& path, const uint8_t* pixels, unsigned width, unsigned height, bool rgba) {
    if (!pixels || !width || !height) return false;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return false;
    const uint32_t imageBytes = width * height * 4;
    uint8_t header[54] = {};
    auto put32 = [&header](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) header[at + i] = static_cast<uint8_t>(v >> (8 * i)); };
    header[0] = 'B'; header[1] = 'M';
    put32(2, 54 + imageBytes);
    put32(10, 54);
    put32(14, 40);
    put32(18, width);
    put32(22, static_cast<uint32_t>(-static_cast<int32_t>(height))); // negative: top-down rows
    header[26] = 1;
    header[28] = 32;
    put32(34, imageBytes);
    bool ok = fwrite(header, 1, sizeof(header), file) == sizeof(header);
    std::vector<uint8_t> row(static_cast<size_t>(width) * 4);
    for (unsigned y = 0; ok && y < height; ++y) {
        const uint8_t* in = pixels + static_cast<size_t>(y) * width * 4;
        for (unsigned x = 0; x < width; ++x) {
            row[x * 4 + 0] = rgba ? in[x * 4 + 2] : in[x * 4 + 0];
            row[x * 4 + 1] = in[x * 4 + 1];
            row[x * 4 + 2] = rgba ? in[x * 4 + 0] : in[x * 4 + 2];
            row[x * 4 + 3] = 255;
        }
        ok = fwrite(row.data(), 1, row.size(), file) == row.size();
    }
    fclose(file);
    return ok;
}

// A second game in one app session crashed in what the first left behind
// (Minecraft, then Terraria: an access violation inside Dozen at the next
// game's first frames). The app restarts itself and starts the game the
// player chose, on a fresh emulator; the console allows a foreground app
// to do this. If it refuses, the game starts in this session as before.
void CatalogShell::RestartInto(Platform::String^ contentPath) {
    PutPref(L"pendingBoot", contentPath);
    SetStatus(L"Restarting ReVita so this game starts on a fresh emulator...");
    libretro_.LogLine("Restarting the app to start the next game on a fresh emulator");
    create_task(Windows::ApplicationModel::Core::CoreApplication::RequestRestartAsync(L"boot"))
    .then([this,contentPath](task<Windows::ApplicationModel::Core::AppRestartFailureReason> work) {
        auto reason = Windows::ApplicationModel::Core::AppRestartFailureReason::Other;
        try { reason = work.get(); } catch (...) {}
        if (reason == Windows::ApplicationModel::Core::AppRestartFailureReason::RestartPending) return;
        PutPref(L"pendingBoot", L"");
        libretro_.LogLine("The console did not restart the app (reason " + std::to_string(static_cast<int>(reason)) +
            "); the game starts in this session");
        restartRefused_ = true;
        StartCore(contentPath);
    }, task_continuation_context::use_current());
}

void CatalogShell::StartCore(Platform::String^ contentPath) {
    if (libretro_.GameHasRun() && !restartRefused_) {
        RestartInto(contentPath);
        return;
    }
    const std::string system = NarrowUtf8(DataRoot());
    const std::string content = Utf8Of(contentPath);
    Platform::String^ title = L"Starting your game";
    bool titled = false;
    for (const auto& game : games_) {
        const std::wstring path = contentPath->Data();
        if ((game.directory && path == game.directory->Path->Data()) || (game.file && path == game.file->Path->Data())) {
            title = game.name;
            titled = true;
            break;
        }
    }
    // Started right after a restart, before the library is read: the name
    // the player chose it by
    if (!titled && Pref(L"lastPlayed", L"")->Length()) title = Pref(L"lastPlayed", L"");
    libretro_.StopRunning();
    renderer_.Shutdown();
    EnterGameView(title);
    {
        const std::wstring marker = std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\vita3k-session.txt";
        FILE* file = nullptr;
        if (_wfopen_s(&file, marker.c_str(), L"wb") == 0 && file) {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            const std::string line = NarrowUtf8(title->Data()) + ", started " + std::to_string(now.wHour) + ":" +
                (now.wMinute < 10 ? "0" : "") + std::to_string(now.wMinute);
            fwrite(line.data(), 1, line.size(), file);
            fclose(file);
        }
    }
    std::wstring displayError;
    if (!renderer_.Initialize(reinterpret_cast<IUnknown*>(renderSurface_), 960, 544, displayError)) {
        booting_ = false;
        if (displayError.find(L"2289696800") != std::wstring::npos || displayError.find(L"2289696773") != std::wstring::npos)
            // DXGI_ERROR_DRIVER_INTERNAL_ERROR / DEVICE_REMOVED: a game before
            // this one left the console's GPU driver failed for this app.
            displayError = L"The graphics driver stopped during an earlier game. Close ReVita completely (Menu button on its tile, then Quit) and open it again.";
        libretro_.LogLine("Display: the presenter could not start: " + NarrowUtf8(displayError));
        ShowLoadFailure(StringOf(displayError));
        return;
    }
    libretro_.LogLine("Display: presenter ready (" + NarrowUtf8(displayError) + ")");
    libretro_.LogLine(D3D12Host::DebugLayerOn()
        ? "Display: D3D12 debug layer on; its errors follow as [d3d12] lines"
        : "Display: D3D12 debug layer not available on this console (or a release build)");
    const std::wstring snapshotPath = std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\frame-300.bmp";
    {
        // A screenshot of the game for its page, once it is well into play
        const std::wstring id = TitleIdIn(contentPath->Data());
        screenshotPath_ = id.empty() ? std::wstring()
            : std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\Screens\\" + id + L".bmp";
        screenshotTaken_ = false;
    }
    libretro_.SetFrameSink([this,snapshotPath](const uint8_t* pixels, unsigned width, unsigned height, bool rgba) {
        std::wstring error;
        const auto cursor = libretro_.Cursor();
        renderer_.SetCursor(cursor.visible, cursor.x, cursor.y, cursor.pressed, cursor.rear);
        const bool presented = renderer_.PresentFrame(pixels, width, height, width * 4, rgba, error);
        const auto shown = ++framesShown_;
        if (shown % 3600 == 0) vita3k_uwp::ReassertCrashLog();
        if (shown <= 5 || shown % 30 == 0) {
            for (const auto& message : renderer_.TakeDebugMessages())
                libretro_.LogLine("[d3d12] " + message);
        }
        if (shown == 300) {
            // Exactly what the core handed over, next to the log: settles
            // whether a dark screen is in the frames or in the display.
            if (WriteBmp(snapshotPath, pixels, width, height, rgba))
                libretro_.LogLine("Display: frame 300 saved as frame-300.bmp next to this log");
        }
        if (!presented) {
            std::lock_guard<std::mutex> guard(presentErrorMutex_);
            presentError_ = error.empty() ? L"presenter not ready" : error;
        }
        if (!pictureSeen_.load() || shown == 60 || shown % 600 == 0) {
            // Sample a grid of the frame: is anything visible yet, and how bright is it?
            unsigned lit = 0, total = 0;
            unsigned long long sum = 0;
            for (unsigned gy = 1; gy < 16; ++gy) {
                const unsigned y = height * gy / 16;
                for (unsigned gx = 1; gx < 16; ++gx) {
                    const unsigned x = width * gx / 16;
                    const uint8_t* p = pixels + (static_cast<size_t>(y) * width + x) * 4;
                    const unsigned value = (static_cast<unsigned>(p[0]) + p[1] + p[2]) / 3;
                    sum += value;
                    ++total;
                    if (value > 20) ++lit;
                }
            }
            if (lit > 0) pictureSeen_ = true;
            lastLevel_ = static_cast<unsigned>(total ? sum / total : 0);
            lastLit_ = lit;
            if (!screenshotTaken_ && !screenshotPath_.empty() && shown >= 1800 && total && lit * 3 > total * 2) {
                screenshotTaken_ = true;
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::path(screenshotPath_).parent_path(), ec);
                if (WriteBmp(screenshotPath_, pixels, width, height, rgba))
                    libretro_.LogLine("Display: a screenshot was kept for the game's page");
            }
            {
                unsigned long long usedMb = 0, limitMb = 0;
                try {
                    usedMb = Windows::System::MemoryManager::AppMemoryUsage / (1024ull * 1024ull);
                    limitMb = Windows::System::MemoryManager::AppMemoryUsageLimit / (1024ull * 1024ull);
                } catch (...) {}
                libretro_.NotePicture(lit, total, lastLevel_.load(), usedMb, limitMb);
            }
            if (shown == 1 || shown == 60 || shown == 600 || shown % 3600 == 0) {
                // Memory too: the console ends an app over its limit with no
                // crash to log, so the climb towards it has to show here.
                unsigned long long usedMb = 0, limitMb = 0;
                try {
                    usedMb = Windows::System::MemoryManager::AppMemoryUsage / (1024ull * 1024ull);
                    limitMb = Windows::System::MemoryManager::AppMemoryUsageLimit / (1024ull * 1024ull);
                } catch (...) {}
                char line[220];
                std::snprintf(line, sizeof(line), "Display: frame %llu, %ux%u, %u of %u samples lit, average level %llu; app memory %llu of %llu MB",
                    shown, width, height, lit, total, total ? sum / total : 0ull, usedMb, limitMb);
                libretro_.LogLine(line);
            }
        }
        if (!error.empty() && (shown <= 3 || shown % 600 == 0))
            libretro_.LogLine("Display: " + NarrowUtf8(error));
    });
    auto dispatcher = Dispatcher;
    libretro_.SetMenuSink([this,dispatcher]() {
        dispatcher->RunAsync(Windows::UI::Core::CoreDispatcherPriority::High,
            ref new Windows::UI::Core::DispatchedHandler([this]() { ShowQuickMenu(); }));
    });
    libretro_.SetProgressSink([this,dispatcher](const std::string& stage, int percent) {
        const std::wstring text = WidenUtf8(stage);
        dispatcher->RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal,
            ref new Windows::UI::Core::DispatchedHandler([this,text,percent]() {
                if (!loadingStage_ || loadFailed_) return;
                if (!text.empty() && text[0] == L'!') {
                    // A stage that ends the game (the GPU stopped): say so.
                    if (loadingLayer_) loadingLayer_->Visibility = Windows::UI::Xaml::Visibility::Visible;
                    ShowLoadFailure(ref new Platform::String(text.c_str() + 1));
                    return;
                }
                loadingStage_->Text = ref new Platform::String(text.c_str());
                loadingBar_->IsIndeterminate = percent < 0;
                if (percent >= 0) loadingBar_->Value = percent;
            }));
    });
    ApplyCoreOptions(contentPath->Data());
    // No D3D12 pipeline library: on the Xbox, Dozen's attempt to make one
    // (ONYX_PSO_CACHE) came back with the GPU device removed, at the game's
    // first pipeline, and every later game then failed to start.
    VitaVulkanBridge::SetPipelineCachePath(L"");
    // No driver-side pipeline saving: the console's D3D12 driver removes the
    // GPU when a compiled pipeline is serialized (library or GetCachedBlob).
    VitaVulkanBridge::SetPipelineBlobDir(L"");
    const std::wstring addonsRoot = Pref(L"storage.addons", L"")->Data();
    const std::wstring titleId = TitleIdIn(contentPath->Data());
    create_task([this,system,content,addonsRoot,titleId]() {
        std::wstring detail;
        if (!libretro_.Initialize(system, detail)) return std::make_pair(false, detail);
        // Its updates and DLC dropped in the updates folder since it last ran
        if (!addonsRoot.empty() && !titleId.empty()) {
            int installed = 0, failed = 0;
            InstallPendingAddons(libretro_, system, addonsRoot, titleId, installed, failed,
                std::vector<std::wstring>{ WidenUtf8(content) });
        }
        {
            std::string build = "0.1.?";
            try {
                const auto version = Windows::ApplicationModel::Package::Current->Id->Version;
                build = std::to_string(version.Major) + "." + std::to_string(version.Minor) + "." + std::to_string(version.Build)
                    + " (build #" + std::to_string(version.Build) + ")";
            } catch (...) {}
            libretro_.SetBuildLabel(build);
            libretro_.LogLine("App version " + build);
        }
        if (!libretro_.LoadGame(content, detail)) return std::make_pair(false, detail);
        if (!libretro_.StartRunning(detail)) return std::make_pair(false, detail);
        return std::make_pair(true, detail);
    }).then([this](task<std::pair<bool, std::wstring>> result) {
        booting_ = false;
        std::pair<bool, std::wstring> outcome{ false, L"The emulator stopped while loading." };
        try { outcome = result.get(); } catch (...) {}
        if (!outcome.first) {
            ShowLoadFailure(ref new Platform::String(outcome.second.c_str()));
            return;
        }
        gameStarted_ = true;
        if (loadingStage_ && !pictureSeen_.load()) loadingStage_->Text = L"Starting the game";
    }, task_continuation_context::use_current());
}

void CatalogShell::ChooseGameFile() {
    auto picker = ref new FileOpenPicker();
    picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
    picker->FileTypeFilter->Append(L".vpk");
    picker->FileTypeFilter->Append(L".zip");
    picker->FileTypeFilter->Append(L".pkg");
    picker->FileTypeFilter->Append(L".vci");
    picker->FileTypeFilter->Append(L".bin");
    create_task(picker->PickSingleFileAsync()).then(
        [this](task<StorageFile^> result) {
            try {
                auto file = result.get();
                if (file == nullptr) return;
                const auto type = vita3k_uwp::ClassifyGameFile(std::wstring(file->Name->Data()));
                if (type == vita3k_uwp::GameFormat::Unknown) {
                    SetStatus(L"Unsupported filename. Choose VPK, ZIP, PKG, VCI or eboot.bin.");
                    return;
                }
                const auto countString = Pref(L"vita.file.count", L"0");
                unsigned count = 0;
                try { count = static_cast<unsigned>(std::stoul(countString->Data())); }
                catch (...) { count = 0; }
                if (count >= 64) {
                    SetStatus(L"64 individually selected file slots used. Choose a containing folder.");
                    return;
                }
                const auto token = L"vita-file-" + StringOf(std::to_wstring(count));
                StorageApplicationPermissions::FutureAccessList->AddOrReplace(token, file);
                PutPref(L"vita.file.count", StringOf(std::to_wstring(count + 1)));
                SetStatus(L"Saved access to " + file->Name + L". Scanning the library...");
                ScanGames();
            } catch (Platform::Exception^ error) {
                SetStatus(L"Failed to add game file: " + error->Message);
            }
        }, task_continuation_context::use_current());
}
void CatalogShell::RestorePinnedGameFiles(unsigned revision) {
    unsigned count = 0;
    try { count = static_cast<unsigned>(std::stoul(Pref(L"vita.file.count", L"0")->Data())); }
    catch (...) { count = 0; }
    const auto access = StorageApplicationPermissions::FutureAccessList;
    for (unsigned n = 0; n < std::min(count, 64u); ++n) {
        const auto token = L"vita-file-" + StringOf(std::to_wstring(n));
        if (!access->ContainsItem(token)) continue;
        create_task(access->GetFileAsync(token)).then(
            [this,revision](task<StorageFile^> work) {
                if (revision != libraryRevision_) return;
                try {
                    auto file = work.get();
                    if (file == nullptr) return;
                    const auto format = vita3k_uwp::ClassifyGameFile(std::wstring(file->Name->Data()));
                    if (format == vita3k_uwp::GameFormat::Unknown) return;
                    const auto id = StableKey(file->Path);
                    for (const auto& existing : games_) {
                        if (existing.key->Equals(id)) return;
                    }
                    LibraryGame game;
                    game.name = PrettyFileName(file->Name);
                    game.key = id;
                    game.sourceType = ref new Platform::String(vita3k_uwp::GameFormatLabel(format));
                    game.format = format;
                    game.validation = L"Checking selected file header...";
                    game.directory = nullptr;
                    game.file = file;
                    game.icon = nullptr;
                    games_.push_back(game);
                    const size_t index = games_.size() - 1;
                    CheckGameFile(index, revision);
                    Render();
                } catch (Platform::Exception^) {
                    // A pinned file may be on a disconnected external drive.
                }
            }, task_continuation_context::use_current());
    }
}
void CatalogShell::ReadSfo(size_t index, StorageFolder^ gameFolder) {
    const unsigned revision = libraryRevision_;
    create_task(gameFolder->GetFolderAsync(L"sce_sys")).then(
        [this,index,revision](StorageFolder^ sys) {
            create_task(sys->GetFileAsync(L"icon0.png")).then(
                [this,index,revision](task<StorageFile^> icon) {
                    try {
                        if (index >= games_.size() || revision != libraryRevision_) return;
                        games_[index].icon = icon.get();
                        Render();
                    } catch (Platform::Exception^) {}
                }, task_continuation_context::use_current());
            return create_task(sys->GetFileAsync(L"param.sfo"));
        }
    ).then([](StorageFile^ file) { return create_task(FileIO::ReadBufferAsync(file)); }
    ).then([this,index,revision](task<IBuffer^> work) {
        try {
            if (revision != libraryRevision_) return;
            auto buffer = work.get();
            auto reader = DataReader::FromBuffer(buffer);
            auto bytes = ref new Platform::Array<unsigned char>(buffer->Length);
            reader->ReadBytes(bytes);
            std::vector<std::uint8_t> data(bytes->Data, bytes->Data + bytes->Length);
            auto metadata = vita3k_uwp::ReadSfo(data);
            if (index >= games_.size()) return;
            if (!metadata.title.empty()) {
                const int wideChars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                    metadata.title.data(), static_cast<int>(metadata.title.size()),
                    nullptr, 0);
                if (wideChars > 0 && wideChars < 2000) {
                    std::wstring wide(static_cast<size_t>(wideChars), L'\0');
                    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        metadata.title.data(), static_cast<int>(metadata.title.size()),
                        &wide[0], wideChars);
                    games_[index].name = StringOf(wide);
                }
            }
            Render();
        } catch (Platform::Exception^) {
            // A folder without a valid param.sfo is only a library candidate.
        }
    }, task_continuation_context::use_current());
}
void CatalogShell::ChooseCustomArtwork(size_t index) {
    if (index >= games_.size()) return;
    auto picker = ref new FileOpenPicker();
    picker->SuggestedStartLocation = PickerLocationId::PicturesLibrary;
    picker->FileTypeFilter->Append(L".png");
    picker->FileTypeFilter->Append(L".jpg");
    picker->FileTypeFilter->Append(L".jpeg");
    const auto key = games_[index].key;
    create_task(picker->PickSingleFileAsync()).then(
        [this,key](task<StorageFile^> work) {
            try {
                auto file = work.get();
                if (file == nullptr) return;
                auto folderTask = create_task(ApplicationData::Current->LocalFolder->
                    CreateFolderAsync(L"Artwork", CreationCollisionOption::OpenIfExists));
                folderTask.then([this,file,key](StorageFolder^ dir) {
                    auto filename = key + file->FileType;
                    return create_task(file->CopyAsync(dir, filename, NameCollisionOption::ReplaceExisting));
                }).then([this,key](task<StorageFile^> copied) {
                    try {
                        auto file = copied.get();
                        PutPref(L"art.file." + key, file->Name);
                        bitmapCache_.clear();
                        SetStatus(L"Custom artwork saved for " + file->Name);
                        Render();
                    } catch (Platform::Exception^ e) {
                        SetStatus(L"Artwork copy failed: " + e->Message);
                    }
                }, task_continuation_context::use_current());
            } catch (Platform::Exception^ e) {
                SetStatus(L"Artwork picker failed: " + e->Message);
            }
        }, task_continuation_context::use_current());
}

void CatalogShell::SaveApiKey(Platform::String^ value) {
    try {
        auto vault = ref new Windows::Security::Credentials::PasswordVault();
        try {
            auto old = vault->Retrieve(L"Vita3KUWP.SteamGridDB", L"apikey");
            vault->Remove(old);
        } catch (Platform::Exception^) {}
        if (!value->IsEmpty()) {
            vault->Add(ref new Windows::Security::Credentials::PasswordCredential(
                L"Vita3KUWP.SteamGridDB", L"apikey", value));
            SetStatus(L"SteamGridDB API key saved securely in Windows PasswordVault.");
        } else {
            SetStatus(L"SteamGridDB API key cleared.");
        }
        Render();
    } catch (Platform::Exception^ e) {
        SetStatus(L"Could not save artwork API key: " + e->Message);
    }
}
Platform::String^ CatalogShell::ApiKey() {
    try {
        auto vault = ref new Windows::Security::Credentials::PasswordVault();
        auto cred = vault->Retrieve(L"Vita3KUWP.SteamGridDB", L"apikey");
        cred->RetrievePassword();
        return cred->Password;
    } catch (Platform::Exception^) {
        return L"";
    }
}
void CatalogShell::FetchMissingArtwork(size_t index) {
    if (index == 0) {
        if (artworkScanBusy_) {
            SetStatus(L"Artwork lookup is already running.");
            return;
        }
        if (ApiKey()->IsEmpty()) {
            SetStatus(L"Save your SteamGridDB API key in Settings to search covers.");
            return;
        }
        artworkScanBusy_ = true;
        SetStatus(L"Looking for matching game artwork...");
    }
    while (index < games_.size() && !ArtworkUri(index)->IsEmpty()) ++index;
    if (index == games_.size()) {
        artworkScanBusy_ = false;
        SetStatus(L"Artwork scan finished. Missing/ambiguous covers use themed placeholders.");
        Render();
        return;
    }
    FetchArtwork(index, true);
}
void CatalogShell::FetchArtwork(size_t index, bool continueBatch) {
    if (index >= games_.size()) {
        if (continueBatch) { artworkScanBusy_ = false; }
        return;
    }
    const auto key = ApiKey();
    if (key->IsEmpty()) {
        SetStatus(L"Enter a SteamGridDB API key under Settings first.");
        if (continueBatch) artworkScanBusy_ = false;
        return;
    }
    auto client = ref new Windows::Web::Http::HttpClient();
    client->DefaultRequestHeaders->TryAppendWithoutValidation(L"Authorization", L"Bearer " + key);
    const auto title = games_[index].name;
    const auto gameKey = games_[index].key;
    auto searchUri = ref new Uri(
        L"https://www.steamgriddb.com/api/v2/search/autocomplete/" + Uri::EscapeComponent(title));
    SetStatus(L"Searching SteamGridDB for: " + title);

    create_task(client->GetStringAsync(searchUri)).then(
        [this,client,index,gameKey,title,continueBatch](task<Platform::String^> search) {
            try {
                auto response = Windows::Data::Json::JsonObject::Parse(search.get());
                const auto list = response->GetNamedArray(L"data");
                unsigned gameId = 0;
                // Do not associate loosely matched art with a completely different title.
                std::wstring desired(title->Data());
                std::transform(desired.begin(), desired.end(), desired.begin(),
                    [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
                for (unsigned i = 0; i < list->Size; ++i) {
                    auto item = list->GetAt(i)->GetObject();
                    std::wstring candidate(item->GetNamedString(L"name")->Data());
                    std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                        [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
                    if (candidate == desired) {
                        gameId = static_cast<unsigned>(item->GetNamedNumber(L"id"));
                        break;
                    }
                }
                if (gameId == 0) {
                    SetStatus(L"No exact SteamGridDB match found for: " + title);
                    if (continueBatch) FetchMissingArtwork(index + 1);
                    return;
                }
                auto imageUri = ref new Uri(
                    L"https://www.steamgriddb.com/api/v2/grids/game/" +
                    StringOf(std::to_wstring(gameId)) + L"?dimensions=600x900&types=static");
                PutPref(L"art.sgdb." + gameKey, StringOf(std::to_wstring(gameId)));
                if (Pref(L"art.hero." + gameKey)->IsEmpty()) {
                    // and the wide background behind the game's page
                    auto heroUri = ref new Uri(L"https://www.steamgriddb.com/api/v2/heroes/game/" +
                        StringOf(std::to_wstring(gameId)) + L"?types=static");
                    create_task(client->GetStringAsync(heroUri)).then([gameKey,client](task<Platform::String^> heroes) {
                        try {
                            auto list = Windows::Data::Json::JsonObject::Parse(heroes.get())->GetNamedArray(L"data");
                            for (unsigned i = 0; i < list->Size; ++i) {
                                auto item = list->GetAt(i)->GetObject();
                                if (!item->HasKey(L"url")) continue;
                                const auto url = item->GetNamedString(L"url");
                                const std::wstring raw(url->Data());
                                if (raw.rfind(L"https://", 0) == 0 && raw.find(L"steamgriddb.com") != std::wstring::npos) {
                                    PutPref(L"art.hero." + gameKey, url);
                                    break;
                                }
                            }
                        } catch (Platform::Exception^) {}
                    });
                }
                create_task(client->GetStringAsync(imageUri)).then(
                    [this,index,gameKey,title,continueBatch,client](task<Platform::String^> art) {
                        try {
                            auto response = Windows::Data::Json::JsonObject::Parse(art.get());
                            auto list = response->GetNamedArray(L"data");
                            if (list->Size == 0) {
                                SetStatus(L"No portrait covers found for: " + title);
                            } else {
                                auto item = list->GetAt(0)->GetObject();
                                const auto uri = item->GetNamedString(L"url");
                                std::wstring raw(uri->Data());
                                if (raw.find(L"https://") == 0 &&
                                    raw.find(L"steamgriddb.com") != std::wstring::npos) {
                                    PutPref(L"art.remote." + gameKey, uri);
                                    SetStatus(L"SteamGridDB art saved for: " + title);
                                    Render();
                                } else {
                                    SetStatus(L"Artwork provider returned an unsupported image URL.");
                                }
                            }
                        } catch (Platform::Exception^ e) {
                            SetStatus(L"Art lookup failed for " + title + L": " + e->Message);
                        }
                        if (continueBatch) FetchMissingArtwork(index + 1);
                    }, task_continuation_context::use_current());
            } catch (Platform::Exception^ e) {
                SetStatus(L"Artwork search failed for " + title + L": " + e->Message);
                if (continueBatch) FetchMissingArtwork(index + 1);
            }
        }, task_continuation_context::use_current());
}

void CatalogShell::ProbeLibretro(bool initialize) {
    if (libretroResult_ == nullptr) return;
    std::wstring report;
    if (initialize) {
        // The Vita3K libretro core needs a writable private filesystem.
        // Never try to write into the read-only APPX install directory.
        const auto folder = ApplicationData::Current->LocalFolder;
        create_task(folder->CreateFolderAsync(L"Vita3KSystem",
            CreationCollisionOption::OpenIfExists)).then(
            [this](task<StorageFolder^> work) {
                if (libretroResult_ == nullptr) return;
                try {
                    auto system = work.get();
                    const auto path = system->Path;
                    const int bytes = WideCharToMultiByte(CP_UTF8, 0, path->Data(), -1,
                        nullptr, 0, nullptr, nullptr);
                    if (bytes <= 0) {
                        libretroResult_->Text = L"Cannot encode the UWP Vita3K system path.";
                        return;
                    }
                    std::string narrow(static_cast<size_t>(bytes), '\\0');
                    WideCharToMultiByte(CP_UTF8, 0, path->Data(), -1,
                        &narrow[0], bytes, nullptr, nullptr);
                    std::wstring detail;
                    const bool loaded = libretro_.Initialize(narrow, detail);
                    libretroResult_->Text = StringOf(detail);
                    SetStatus(loaded ? L"Vita3K core initialization returned. "
                        L"No game execution or Vulkan context is available." :
                        L"Vita3K core initialization blocked. See diagnostic details.");
                } catch (Platform::Exception^ e) {
                    libretroResult_->Text = L"Cannot create private Vita3K system directory: " + e->Message;
                }
            }, task_continuation_context::use_current());
    } else {
        const bool loaded = libretro_.Probe(report);
        libretroResult_->Text = StringOf(report);
        SetStatus(loaded ? L"Actual Vita3K core C ABI loaded; GPU and games untested." :
            L"Packaged native Vita3K core not present or not UWP compatible.");
    }
}
void CatalogShell::ProbeCore() {
    BitmapAllocator bitmap(64);
    std::uint32_t count = 4;
    const int first = bitmap.allocate_from(0, count);
    const int left = bitmap.free_slot_count(0, 64);
    const int reserved = bitmap.allocate_at(8, 2);
    bitmap.free(0, 4);
    const int afterFree = bitmap.free_slot_count(0, 64);
    const bool okay = first == 0 && count == 4 && left == 60 &&
        reserved == 0 && afterFree == 62;
    if (coreResult_ != nullptr)
        coreResult_->Text = okay ?
            L"PASS  /  Original Vita3K memory allocator executed." :
            L"FAIL  /  Upstream Vita3K allocator returned unexpected results.";
}
void CatalogShell::ProbeMemory() {
    const SIZE_T length = 4096;
    void* page = VirtualAllocFromApp(nullptr, length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!page) {
        if (memoryResult_ != nullptr)
            memoryResult_->Text = L"RW allocation failed. Windows error: " +
                StringOf(std::to_wstring(GetLastError()));
        return;
    }
    volatile unsigned char* bytes = static_cast<volatile unsigned char*>(page);
    bytes[0] = 0x5a;
    const bool writable = bytes[0] == 0x5a;
    DWORD oldProtect = 0;
    const BOOL executable = VirtualProtectFromApp(page, length, PAGE_EXECUTE_READ, &oldProtect);
    const DWORD protectError = executable ? 0 : GetLastError();
    if (executable) {
        DWORD ignored = 0;
        VirtualProtectFromApp(page, length, PAGE_READWRITE, &ignored);
    }
    VirtualFree(page, 0, MEM_RELEASE);
    constexpr SIZE_T guestSize = static_cast<SIZE_T>(1ULL << 32);
    void* guest = VirtualAllocFromApp(nullptr, guestSize, MEM_RESERVE, PAGE_NOACCESS);
    const DWORD reserveError = guest ? 0 : GetLastError();
    if (guest) VirtualFree(guest, 0, MEM_RELEASE);
    std::wstring result = L"RW page: ";
    result += writable ? L"PASS" : L"FAIL";
    result += L"  /  RW-to-RX: ";
    result += executable ? L"PASS" : L"FAIL";
    if (!executable) result += L" (error " + std::to_wstring(protectError) + L")";
    result += L"  /  4 GiB virtual reserve: ";
    result += guest ? L"PASS" : L"FAIL";
    if (!guest) result += L" (error " + std::to_wstring(reserveError) + L")";
    result += L". This is not Dynarmic/JIT compatibility proof.";
    if (memoryResult_ != nullptr) memoryResult_->Text = StringOf(result);
}
void CatalogShell::BootRenderer() {
    if (renderSurface_ == nullptr || rendererResult_ == nullptr) return;
    std::wstring output;
    const bool initialized = renderer_.Initialize(
        reinterpret_cast<IUnknown*>(renderSurface_), 960, 544, output);
    if (initialized) renderer_.DrawTestFrame(output);
    rendererResult_->Text = StringOf(output);
    SetStatus(initialized ? L"Native D3D12 device created. Check renderer status for present result." :
                            L"Native D3D12 initialization failed; see vita3k-runtime.log.");
}
void CatalogShell::PresentTestFrame() {
    if (rendererResult_ == nullptr) return;
    std::wstring output;
    if (!renderer_.Ready()) {
        rendererResult_->Text = L"Initialize D3D12 first.";
        return;
    }
    renderer_.DrawUploadTestFrame(output);
    rendererResult_->Text = StringOf(output);
}

void CatalogShell::SaveCueSettings() {
    PutPref(L"ui.sounds", uiSounds_ ? L"1" : L"0");
    PutPref(L"ui.sound.volume", StringOf(std::to_wstring(uiSoundVolume_)));
}
void CatalogShell::PlayCue(unsigned kind) {
    if (!bootstrapped_ || !uiSounds_ || uiSoundVolume_ <= 0) return;
    kind %= vita3k_uwp::kInterfaceCueCount;
    if (cueAudio_.Ready()) {
        cueAudio_.Play(kind, static_cast<float>(uiSoundVolume_));
        return;
    }
    if (!cueReady_[kind] || cuePlayers_[kind] == nullptr) return;
    // Dedicated player per cue means focus and confirm don't cut one another off.
    cuePlayers_[kind]->Stop();
    cuePlayers_[kind]->Play();
}
void CatalogShell::PrepareCues() {
    {
        std::vector<std::vector<std::uint8_t>> wavs;
        for (unsigned i = 0; i < vita3k_uwp::kInterfaceCueCount; ++i) wavs.push_back(vita3k_uwp::GenerateInterfaceCue(i));
        if (cueAudio_.Start(wavs, 32000)) return;
        libretro_.LogLine("Interface sounds: XAudio2 unavailable, using media elements");
    }
    // Small UI sounds also use durable file-backed sources. MediaOpened is
    // the readiness gate; a generated byte buffer alone is not proof of audio.
    create_task(ApplicationData::Current->LocalFolder->CreateFolderAsync(
        L"MenuAudio", CreationCollisionOption::OpenIfExists))
    .then([this](task<StorageFolder^> result) {
        try {
            auto folder = result.get();
            for (unsigned i = 0; i < vita3k_uwp::kInterfaceCueCount; ++i) {
                auto bytes = vita3k_uwp::GenerateInterfaceCue(i);
                auto buffer = ref new Platform::Array<unsigned char>(
                    static_cast<unsigned>(bytes.size()));
                std::memcpy(buffer->Data, bytes.data(), bytes.size());
                const auto name = L"ui" + StringOf(std::to_wstring(i)) + L".wav";
                create_task(folder->CreateFileAsync(
                    name, CreationCollisionOption::ReplaceExisting))
                .then([buffer](StorageFile^ file) {
                    return create_task(FileIO::WriteBytesAsync(file, buffer))
                        .then([file]() { return file; });
                }).then([this,i](task<StorageFile^> completed) {
                    try {
                        const auto file = completed.get();
                        cueReady_[i] = false;
                        cuePlayers_[i]->Source = ref new Uri(
                            L"ms-appdata:///local/MenuAudio/" + file->Name);
                    } catch (Platform::Exception^) {
                        cueReady_[i] = false;
                    }
                }, task_continuation_context::use_current());
            }
        } catch (Platform::Exception^ error) {
            SetStatus(L"Could not prepare UI audio: " + error->Message);
        }
    }, task_continuation_context::use_current());
}

Platform::String^ CatalogShell::BackdropUri(size_t index) {
    if (index >= games_.size()) return L"";
    const auto hero = Pref(L"art.hero." + games_[index].key);
    if (!hero->IsEmpty()) return hero;
    const auto id = GameTitleId(index);
    if (!id.empty()) {
        std::error_code ec;
        const std::wstring path = std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\Screens\\" + id + L".bmp";
        if (std::filesystem::exists(path, ec)) return StringOf(L"ms-appdata:///local/Screens/" + id + L".bmp");
    }
    return L"";
}

namespace {
// SteamGridDB's images are on its own hosts; anything else is not used.
bool SteamGridDbImage(Platform::String^ url) {
    if (url == nullptr) return false;
    const std::wstring raw(url->Data());
    return raw.rfind(L"https://", 0) == 0 && raw.find(L"steamgriddb.com") != std::wstring::npos;
}
std::wstring LowerOf(Platform::String^ s) {
    std::wstring out = s ? s->Data() : L"";
    std::transform(out.begin(), out.end(), out.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return out;
}
}

void CatalogShell::CloseArtPicker() {
    if (artPicker_ == nullptr) return;
    unsigned at = 0;
    if (root_->Children->IndexOf(artPicker_, &at)) root_->Children->RemoveAt(at);
    artPicker_ = nullptr;
    artResults_ = nullptr;
    artCovers_ = nullptr;
    artHeroes_ = nullptr;
    artNote_ = nullptr;
    ++artRequest_;
    // what was picked shows on the page now
    Render();
}

void CatalogShell::ShowArtPicker(size_t index) {
    if (index >= games_.size()) return;
    if (ApiKey()->IsEmpty()) {
        SetStatus(L"Save your SteamGridDB API key first: Settings, Artwork.");
        return;
    }
    if (artPicker_ != nullptr) {
        unsigned at = 0;
        if (root_->Children->IndexOf(artPicker_, &at)) root_->Children->RemoveAt(at);
        artPicker_ = nullptr;
    }
    PlayCue(1);
    auto layer = ref new Grid();
    artPicker_ = layer;
    layer->Background = Solid(Alpha(244, Bg().R, Bg().G, Bg().B));
    Grid::SetColumnSpan(layer, 2);
    Grid::SetRowSpan(layer, 2);
    Canvas::SetZIndex(layer, 40);
    auto scroll = ref new ScrollViewer();
    scroll->IsTabStop = false;
    scroll->VerticalScrollBarVisibility = ScrollBarVisibility::Hidden;
    scroll->HorizontalScrollBarVisibility = ScrollBarVisibility::Disabled;
    auto body = Vertical(10);
    body->Margin = Thickness(90, 46, 90, 40);

    auto eyebrow = Text(L"STEAMGRIDDB", 10, true);
    eyebrow->CharacterSpacing = 220;
    eyebrow->Foreground = BrandBrush();
    body->Children->Append(eyebrow);
    auto heading = Text(L"Art for " + games_[index].name, 28, false);
    heading->FontWeight = FontWeights::Light;
    body->Children->Append(heading);
    artNote_ = Text(L"Pick the game, then its cover and the background behind its page. B closes.", 13, false, true);
    body->Children->Append(artNote_);

    auto searchRow = ref new StackPanel();
    searchRow->Orientation = Orientation::Horizontal;
    searchRow->Margin = Thickness(0, 8, 0, 6);
    auto box = ref new TextBox();
    box->Text = games_[index].name;
    box->Width = 520;
    box->Height = 40;
    box->FontSize = 16;
    searchRow->Children->Append(box);
    auto go = Button(L"Search");
    go->Margin = Thickness(12, 0, 0, 0);
    go->Click += ref new RoutedEventHandler([this,index,box](Platform::Object^, RoutedEventArgs^) {
        ArtPickerSearch(index, box->Text);
    });
    searchRow->Children->Append(go);
    body->Children->Append(searchRow);

    auto label = [this](const wchar_t* text) {
        auto t = Text(ref new Platform::String(text), 12, true);
        t->CharacterSpacing = 140;
        t->Margin = Thickness(0, 14, 0, 4);
        t->Opacity = 0.8;
        return t;
    };
    body->Children->Append(label(L"GAMES FOUND"));
    artResults_ = Vertical(6);
    body->Children->Append(artResults_);

    body->Children->Append(label(L"COVERS"));
    auto coverScroll = ref new ScrollViewer();
    coverScroll->IsTabStop = false;
    coverScroll->HorizontalScrollMode = ScrollMode::Enabled;
    coverScroll->VerticalScrollMode = ScrollMode::Disabled;
    coverScroll->HorizontalScrollBarVisibility = ScrollBarVisibility::Hidden;
    coverScroll->VerticalScrollBarVisibility = ScrollBarVisibility::Disabled;
    artCovers_ = ref new StackPanel();
    artCovers_->Orientation = Orientation::Horizontal;
    coverScroll->Content = artCovers_;
    body->Children->Append(coverScroll);

    body->Children->Append(label(L"BACKGROUNDS"));
    auto heroScroll = ref new ScrollViewer();
    heroScroll->IsTabStop = false;
    heroScroll->HorizontalScrollMode = ScrollMode::Enabled;
    heroScroll->VerticalScrollMode = ScrollMode::Disabled;
    heroScroll->HorizontalScrollBarVisibility = ScrollBarVisibility::Hidden;
    heroScroll->VerticalScrollBarVisibility = ScrollBarVisibility::Disabled;
    artHeroes_ = ref new StackPanel();
    artHeroes_->Orientation = Orientation::Horizontal;
    heroScroll->Content = artHeroes_;
    body->Children->Append(heroScroll);

    auto done = Button(L"Done", true);
    done->Margin = Thickness(0, 22, 0, 0);
    done->MinWidth = 160;
    done->Click += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) {
        PlayCue(2);
        CloseArtPicker();
    });
    body->Children->Append(done);

    scroll->Content = body;
    layer->Children->Append(scroll);
    root_->Children->Append(layer);
    go->Loaded += ref new RoutedEventHandler([go](Platform::Object^, RoutedEventArgs^) {
        go->Focus(Windows::UI::Xaml::FocusState::Programmatic);
    });
    ArtPickerSearch(index, games_[index].name);
}

void CatalogShell::ArtPickerSearch(size_t index, Platform::String^ term) {
    if (artPicker_ == nullptr || index >= games_.size() || term == nullptr || term->IsEmpty()) return;
    const unsigned request = ++artRequest_;
    artResults_->Children->Clear();
    artCovers_->Children->Clear();
    artHeroes_->Children->Clear();
    artNote_->Text = L"Searching SteamGridDB for " + term + L"...";
    auto client = ref new Windows::Web::Http::HttpClient();
    client->DefaultRequestHeaders->TryAppendWithoutValidation(L"Authorization", L"Bearer " + ApiKey());
    auto uri = ref new Uri(L"https://www.steamgriddb.com/api/v2/search/autocomplete/" + Uri::EscapeComponent(term));
    const auto wanted = LowerOf(games_[index].name);
    create_task(client->GetStringAsync(uri)).then([this,index,request,client,wanted](task<Platform::String^> work) {
        if (artPicker_ == nullptr || request != artRequest_) return;
        try {
            auto list = Windows::Data::Json::JsonObject::Parse(work.get())->GetNamedArray(L"data");
            if (list->Size == 0) {
                artNote_->Text = L"Nothing found. Try the game's name another way (Search).";
                return;
            }
            artNote_->Text = L"Pick the right game; its covers and backgrounds load below.";
            Windows::UI::Xaml::Controls::Button^ first = nullptr;
            unsigned exactId = 0;
            Platform::String^ exactName = nullptr;
            for (unsigned i = 0; i < list->Size && i < 12; ++i) {
                auto item = list->GetAt(i)->GetObject();
                auto name = item->GetNamedString(L"name");
                const unsigned id = static_cast<unsigned>(item->GetNamedNumber(L"id"));
                std::wstring text = name->Data();
                if (item->HasKey(L"release_date") &&
                    item->GetNamedValue(L"release_date")->ValueType == Windows::Data::Json::JsonValueType::Number) {
                    const double seconds = item->GetNamedNumber(L"release_date");
                    if (seconds > 0) text += L"   " + std::to_wstring(1970 + static_cast<int>(seconds / 31557600.0));
                }
                auto pick = Button(StringOf(text));
                pick->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
                pick->MinWidth = 460;
                pick->Click += ref new RoutedEventHandler([this,index,id,name](Platform::Object^, RoutedEventArgs^) {
                    PlayCue(1);
                    ArtPickerLoad(index, id, name);
                });
                artResults_->Children->Append(pick);
                if (!first) first = pick;
                if (!exactId && LowerOf(name) == wanted) { exactId = id; exactName = name; }
            }
            if (first && !InWindow(dynamic_cast<DependencyObject^>(Windows::UI::Xaml::Input::FocusManager::GetFocusedElement())))
                first->Focus(Windows::UI::Xaml::FocusState::Programmatic);
            // The game's own name matched: its art comes up straight away.
            if (exactId) ArtPickerLoad(index, exactId, exactName);
        } catch (Platform::Exception^ e) {
            artNote_->Text = L"SteamGridDB search failed: " + e->Message;
        }
    }, task_continuation_context::use_current());
}

void CatalogShell::ArtPickerLoad(size_t index, unsigned gameId, Platform::String^ name) {
    if (artPicker_ == nullptr || index >= games_.size()) return;
    const unsigned request = ++artRequest_;
    const auto key = games_[index].key;
    const auto idText = StringOf(std::to_wstring(gameId));
    PutPref(L"art.sgdb." + key, idText);
    artCovers_->Children->Clear();
    artHeroes_->Children->Clear();
    artNote_->Text = L"Loading the art for " + name + L"...";
    auto client = ref new Windows::Web::Http::HttpClient();
    client->DefaultRequestHeaders->TryAppendWithoutValidation(L"Authorization", L"Bearer " + ApiKey());
    auto grids = ref new Uri(L"https://www.steamgriddb.com/api/v2/grids/game/" + idText + L"?dimensions=600x900&types=static");
    auto heroes = ref new Uri(L"https://www.steamgriddb.com/api/v2/heroes/game/" + idText + L"?types=static");

    // One row of pictures; choosing one keeps its full-size address.
    auto fill = [this,key](Windows::Data::Json::JsonArray^ list, StackPanel^ row, double width, double height,
                           bool cover) {
        unsigned shown = 0;
        for (unsigned i = 0; i < list->Size && shown < 16; ++i) {
            auto item = list->GetAt(i)->GetObject();
            if (!item->HasKey(L"url")) continue;
            auto url = item->GetNamedString(L"url");
            auto thumb = item->HasKey(L"thumb") ? item->GetNamedString(L"thumb") : url;
            if (!SteamGridDbImage(url) || !SteamGridDbImage(thumb)) continue;
            auto pick = ref new Windows::UI::Xaml::Controls::Button();
            pick->Padding = Thickness(0);
            pick->Margin = Thickness(0, 0, 12, 6);
            pick->BorderThickness = Thickness(2);
            Skin(pick, 10);
            auto image = ref new Image();
            image->Width = width;
            image->Height = height;
            image->Stretch = Stretch::UniformToFill;
            image->Source = ref new BitmapImage(ref new Uri(thumb));
            pick->Content = image;
            pick->Click += ref new RoutedEventHandler([this,key,url,cover](Platform::Object^, RoutedEventArgs^) {
                PlayCue(1);
                if (cover) {
                    PutPref(L"art.remote." + key, url);
                    PutPref(L"art.file." + key, L"");
                    if (artNote_) artNote_->Text = L"Cover set. Pick a background too, or Done.";
                } else {
                    PutPref(L"art.hero." + key, url);
                    if (artNote_) artNote_->Text = L"Background set. Done shows it behind the game's page.";
                }
            });
            row->Children->Append(pick);
            ++shown;
        }
        return shown;
    };

    create_task(client->GetStringAsync(grids)).then([this,request,client,heroes,fill,key](task<Platform::String^> work) {
        if (artPicker_ == nullptr || request != artRequest_) return task_from_result<Platform::String^>(nullptr);
        try {
            auto list = Windows::Data::Json::JsonObject::Parse(work.get())->GetNamedArray(L"data");
            if (fill(list, artCovers_, 120, 180, true) == 0) {
                auto none = Text(L"No portrait covers for this game.", 13, false, true);
                artCovers_->Children->Append(none);
            }
        } catch (Platform::Exception^) {
            artCovers_->Children->Append(Text(L"Covers could not be loaded.", 13, false, true));
        }
        return create_task(client->GetStringAsync(heroes));
    }, task_continuation_context::use_current())
    .then([this,request,fill,key](task<Platform::String^> work) {
        if (artPicker_ == nullptr || request != artRequest_) return;
        try {
            auto text = work.get();
            if (text == nullptr) return;
            // first: no background at all
            auto clear = Button(L"No background");
            clear->Margin = Thickness(0, 0, 12, 6);
            clear->Height = 110;
            clear->Click += ref new RoutedEventHandler([this,key](Platform::Object^, RoutedEventArgs^) {
                PlayCue(2);
                PutPref(L"art.hero." + key, L"");
                if (artNote_) artNote_->Text = L"No background: a screenshot of the game shows once it has been played.";
            });
            artHeroes_->Children->Append(clear);
            auto list = Windows::Data::Json::JsonObject::Parse(text)->GetNamedArray(L"data");
            if (fill(list, artHeroes_, 340, 110, false) == 0)
                artHeroes_->Children->Append(Text(L"No backgrounds for this game.", 13, false, true));
            artNote_->Text = L"Pick a cover and a background. B or Done closes.";
        } catch (Platform::Exception^) {
            artHeroes_->Children->Append(Text(L"Backgrounds could not be loaded.", 13, false, true));
        }
    }, task_continuation_context::use_current());
}


// ── The PS3-style home (XMB) ────────────────────────────────────────────────

void CatalogShell::ApplyXmbSky() {
    if (xmb_ == nullptr) return;
    const XmbTheme& theme = kXmbThemes[(xmbTheme_ >= 0 && xmbTheme_ < kXmbThemeCount) ? xmbTheme_ : 0];
    auto sky = ref new LinearGradientBrush();
    sky->StartPoint = Point(0.5, 0);
    sky->EndPoint = Point(0.5, 1);
    auto stop = [&sky](double offset, Color color) {
        auto s = ref new GradientStop();
        s->Offset = offset;
        s->Color = color;
        sky->GradientStops->Append(s);
    };
    if (theme.monthly) {
        // The month's colour, darker at night, as on the PS3.
        SYSTEMTIME now{};
        GetLocalTime(&now);
        const double light = (now.wHour >= 7 && now.wHour < 18) ? 1.0 : (now.wHour >= 5 && now.wHour < 21) ? 0.82 : 0.64;
        const Color base = XmbMonthColor(now.wMonth);
        stop(0.0, Shade(base, 0.34 * light));
        stop(0.55, Shade(base, 0.78 * light));
        stop(1.0, Shade(base, 0.42 * light));
    } else {
        const Color top = RgbColor(theme.top[0], theme.top[1], theme.top[2]);
        const Color bottom = RgbColor(theme.bottom[0], theme.bottom[1], theme.bottom[2]);
        stop(0.0, top);
        stop(0.58, bottom);
        stop(1.0, Shade(bottom, 0.5));
    }
    xmb_->Background = sky;
}

void CatalogShell::SaveXmbTheme(int theme) {
    xmbTheme_ = ((theme % kXmbThemeCount) + kXmbThemeCount) % kXmbThemeCount;
    PutPref(L"xmb.theme", StringOf(std::to_wstring(xmbTheme_)));
    PlayCue(5);
    if (xmb_ != nullptr && xmb_->Visibility == Windows::UI::Xaml::Visibility::Visible) {
        ApplyXmbSky();
        LayoutXmb(0, 0);
    } else if (page_ == 2) {
        Render();
    }
    SetStatus(ref new Platform::String((std::wstring(L"XMB colour: ") + kXmbThemes[xmbTheme_].name + L".").c_str()));
}

void CatalogShell::SaveLayout(bool xmb) {
    xmbMode_ = xmb;
    PutPref(L"ui.layout", xmb ? L"xmb" : L"cinematic");
    PlayCue(5);
    if (xmb) {
        SetStatus(L"Home screen: PS3 XMB.");
        ShowPage(0);
    } else {
        if (xmb_ != nullptr) HideXmb();
        SetStatus(L"Home screen: cinematic.");
        Render();
    }
}

std::vector<XmbCategory> CatalogShell::XmbCategories() {
    std::vector<XmbCategory> cats;
    {
        XmbCategory c;
        c.name = L"Settings";
        c.glyph = L"\uE713";
        auto settings = [this]() { ShowPage(2); };
        c.items.push_back(XmbEntry{ std::wstring(L"Theme: ") + kXmbThemes[xmbTheme_ % kXmbThemeCount].name,
            L"Press A for the next colour", L"\uE790", -1, [this]() { SaveXmbTheme(xmbTheme_ + 1); }, nullptr });
        c.items.push_back(XmbEntry{ L"App Settings", L"Themes, music, sounds and the home screen", L"\uE771", -1, settings, nullptr });
        c.items.push_back(XmbEntry{ L"Emulation Settings", L"Renderer, resolution, shaders and the CPU", L"\uE7FC", -1, settings, nullptr });
        c.items.push_back(XmbEntry{ L"System Software", L"The PS Vita firmware ReVita runs on", L"\uE896", -1, settings, nullptr });
        c.items.push_back(XmbEntry{ L"Storage", L"Where games, saves and ReVita's data live", L"\uEDA2", -1, settings, nullptr });
        c.items.push_back(XmbEntry{ L"Cinematic Home", L"Go back to the cinematic home screen", L"\uE80F", -1, [this]() { SaveLayout(false); }, nullptr });
        cats.push_back(std::move(c));
    }
    {
        XmbCategory c;
        c.name = L"Music";
        c.glyph = L"\uE8D6";
        const wchar_t* names[] = { L"Match the Theme", L"Starlight Drive", L"Afterlight", L"Neon Bloom",
                                   L"Midnight Arcade", L"Coastline '87", L"Pastel Horizon", L"Cotton Sky",
                                   L"Ultraviolet", L"No Music" };
        const int trackOf[] = { 10, 1, 2, 3, 4, 6, 7, 8, 9, 0 };
        c.initial = 0;
        for (int slot = 0; slot < 10; ++slot) {
            const int i = trackOf[slot];
            if (track_ == i) c.initial = slot;
            c.items.push_back(XmbEntry{ names[slot], track_ == i ? L"Now playing" : L"Home menu music", L"\uE8D6", -1,
                [this, i]() { SetMusic(i); Render(); }, nullptr });
        }
        cats.push_back(std::move(c));
    }
    {
        XmbCategory c;
        c.name = L"Game";
        c.glyph = L"\uE7FC";
        const auto last = Pref(L"lastPlayed", L"");
        for (size_t i = 0; i < games_.size(); ++i) {
            const auto& game = games_[i];
            std::wstring subtitle = std::wstring(L"PlayStation\u00AEVita  \u2022  ") + (game.sourceType ? game.sourceType->Data() : L"");
            if (!last->IsEmpty() && game.name == last) {
                subtitle = L"Last played  \u2022  " + subtitle;
                c.initial = static_cast<int>(c.items.size());
            }
            c.items.push_back(XmbEntry{ game.name ? game.name->Data() : L"Game", subtitle, L"\uE7FC", static_cast<int>(i),
                [this, i]() { AttemptBoot(i); },
                [this, i]() { selectedGame_ = static_cast<int>(i); ShowPage(4); } });
        }
        c.items.push_back(XmbEntry{ L"Add a Game", L"A VPK, ZIP, PKG or game folder", L"\uE710", -1, [this]() { ChooseGameFile(); }, nullptr });
        c.items.push_back(XmbEntry{ L"Add a Folder of Games", L"Every game in a folder", L"\uE8F4", -1, [this]() { ChooseGameFolder(); }, nullptr });
        c.items.push_back(XmbEntry{ L"Game Library", L"All games, with search", L"\uE8F1", -1, [this]() { ShowPage(1); }, nullptr });
        cats.push_back(std::move(c));
    }
    {
        XmbCategory c;
        c.name = L"Network";
        c.glyph = L"\uE774";
        c.items.push_back(XmbEntry{ L"Find Covers and Backgrounds", L"SteamGridDB, for every game without art", L"\uEB9F", -1,
            [this]() { FetchMissingArtwork(0); }, nullptr });
        c.items.push_back(XmbEntry{ L"Install Updates and DLC", L"New ones in your updates folder", L"\uE896", -1,
            [this]() { InstallAddons(); }, nullptr });
        cats.push_back(std::move(c));
    }
#if !defined(VITA3K_RELEASE_BUILD)
    {
        XmbCategory c;
        c.name = L"Tools";
        c.glyph = L"\uE90F";
        c.items.push_back(XmbEntry{ L"Diagnostics", L"Developer checks", L"\uE90F", -1, [this]() { ShowPage(3); }, nullptr });
        cats.push_back(std::move(c));
    }
#endif
    return cats;
}

void CatalogShell::ShowXmb() {
    if (!xmbMode_ || inGame_) return;
    navigation_->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    if (contentScroll_) contentScroll_->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    if (statusBar_) statusBar_->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    backdrop_.SetActive(false);
    if (xmb_ == nullptr) {
        xmb_ = ref new Grid();
        Grid::SetColumnSpan(xmb_, 2);
        Grid::SetRowSpan(xmb_, 2);
        Canvas::SetZIndex(xmb_, 30);
        xmbWaves_ = ref new Canvas();
        xmbWaves_->IsHitTestVisible = false;
        xmb_->Children->Append(xmbWaves_);
        // the flowing waves, run by the system compositor (no frame work on
        // the UI thread: the old point-by-point waves were redrawn here 30
        // times a second and hitched the menu)
        xmbRibbons_.Attach(xmbWaves_);
        // a game's own picture, behind the menu while it is chosen
        auto picture = ref new Image();
        picture->Stretch = Stretch::UniformToFill;
        picture->Opacity = 0;
        picture->IsHitTestVisible = false;
        picture->Name = L"xmbPicture";
        xmb_->Children->Append(picture);
        xmbStage_ = ref new Canvas();
        xmb_->Children->Append(xmbStage_);
        xmbClock_ = Text(L"", 17, false);
        xmbClock_->FontWeight = FontWeights::Light;
        xmbClock_->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Right;
        xmbClock_->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Top;
        xmbClock_->Margin = Thickness(0, 28, 52, 0);
        xmbClock_->Foreground = Solid(Colors::White);
        xmb_->Children->Append(xmbClock_);
        xmb_->SizeChanged += ref new SizeChangedEventHandler([this](Platform::Object^, SizeChangedEventArgs^) {
            LayoutXmb(0, 0);
        });
        root_->Children->Append(xmb_);
        xmbTimer_ = ref new DispatcherTimer();
        xmbTimer_->Interval = Millis(33);
        xmbTimer_->Tick += ref new EventHandler<Platform::Object^>([this](Platform::Object^, Platform::Object^) { XmbWaves(); });
    }
    ApplyXmbSky();
    xmb_->Visibility = Windows::UI::Xaml::Visibility::Visible;
    xmbRibbons_.SetActive(true);
    UpdateClock();
    LayoutXmb(0, 0);
}

void CatalogShell::HideXmb() {
    if (xmb_ == nullptr) return;
    xmb_->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    if (xmbTimer_) xmbTimer_->Stop();
    xmbRibbons_.SetActive(false);
    if (!inGame_) {
        navigation_->Visibility = Windows::UI::Xaml::Visibility::Visible;
        if (contentScroll_) contentScroll_->Visibility = Windows::UI::Xaml::Visibility::Visible;
        if (statusBar_) statusBar_->Visibility = Windows::UI::Xaml::Visibility::Visible;
        backdrop_.SetActive(true);
    }
}

void CatalogShell::XmbWaves() {
    if (xmb_ == nullptr || xmbWaves_ == nullptr) return;
    const double width = xmb_->ActualWidth > 0 ? xmb_->ActualWidth : 960;
    const double height = xmb_->ActualHeight > 0 ? xmb_->ActualHeight : 540;
    xmbPhase_ += 0.033;
    const double pi = 3.14159265358979;
    const int steps = 48;
    std::vector<std::vector<Point>> tops(3), bottoms(3);
    for (int b = 0; b < 3; ++b) {
        const double centre = height * (0.56 + 0.03 * b);
        const double a1 = height * (0.075 - 0.015 * b);
        const double k1 = 2 * pi / (width * (1.15 + 0.3 * b));
        const double p1 = xmbPhase_ * (0.32 + 0.1 * b) + b * 1.9;
        const double a2 = height * 0.022;
        const double k2 = 2 * pi / (width * 0.47);
        const double p2 = -xmbPhase_ * 0.45 + b * 0.8;
        for (int s = 0; s <= steps; ++s) {
            const double x = width * s / steps;
            const double top = centre + a1 * std::sin(k1 * x + p1) + a2 * std::sin(k2 * x + p2);
            const double thick = height * (0.05 + 0.032 * std::sin(k1 * 0.7 * x + p1 * 1.3 + b));
            tops[b].push_back(Point(static_cast<float>(x), static_cast<float>(top)));
            bottoms[b].push_back(Point(static_cast<float>(x), static_cast<float>(top + thick)));
        }
    }
    auto children = xmbWaves_->Children;
    for (int b = 0; b < 3 && b < static_cast<int>(children->Size); ++b) {
        auto band = dynamic_cast<Polygon^>(children->GetAt(b));
        if (!band) continue;
        auto points = ref new PointCollection();
        for (const auto& p : tops[b]) points->Append(p);
        for (auto it = bottoms[b].rbegin(); it != bottoms[b].rend(); ++it) points->Append(*it);
        band->Points = points;
    }
    for (int l = 0; l < 2 && 3 + l < static_cast<int>(children->Size); ++l) {
        auto edge = dynamic_cast<Polyline^>(children->GetAt(3 + l));
        if (!edge) continue;
        auto points = ref new PointCollection();
        for (const auto& p : tops[l]) points->Append(p);
        edge->Points = points;
    }
}

void CatalogShell::LayoutXmb(int dx, int dy) {
    if (xmb_ == nullptr || xmbStage_ == nullptr) return;
    auto cats = XmbCategories();
    if (cats.empty()) return;
    const int count = static_cast<int>(cats.size());
    if (static_cast<int>(xmbItem_.size()) != count) xmbItem_.assign(count, -1);
    if (xmbCat_ < 0 || xmbCat_ >= count) xmbCat_ = std::min(2, count - 1);
    for (int k = 0; k < count; ++k) {
        const int items = static_cast<int>(cats[k].items.size());
        if (xmbItem_[k] < 0 || xmbItem_[k] >= items) xmbItem_[k] = std::max(0, std::min(cats[k].initial, items - 1));
    }
    const int oldCat = xmbCat_, oldItem = xmbItem_[xmbCat_];
    xmbCat_ = std::max(0, std::min(count - 1, xmbCat_ + dx));
    {
        const int items = static_cast<int>(cats[xmbCat_].items.size());
        if (dx == 0) xmbItem_[xmbCat_] = std::max(0, std::min(items - 1, xmbItem_[xmbCat_] + dy));
    }
    if (xmbCat_ != oldCat || xmbItem_[xmbCat_] != oldItem) PlayCue(0);
    else if (dx != 0 || dy != 0) return;  // at an end: nothing moves

    const double width = xmb_->ActualWidth > 0 ? xmb_->ActualWidth : 960;
    const double height = xmb_->ActualHeight > 0 ? xmb_->ActualHeight : 540;
    const double anchor = width * 0.25;   // the chosen column's centre
    const double barTop = height * 0.2;   // the category icons
    const double column = 112;
    xmbStage_->Children->Clear();

    auto slide = [](UIElement^ element, double fromX, double fromY) {
        if (fromX == 0 && fromY == 0) return;
        auto move = ref new TranslateTransform();
        element->RenderTransform = move;
        auto story = ref new Storyboard();
        auto make = [&story, move](Platform::String^ property, double from) {
            if (from == 0) return;
            auto animation = ref new DoubleAnimation();
            animation->From = from;
            animation->To = 0.0;
            animation->Duration = DurationHelper::FromTimeSpan(Millis(150));
            auto ease = ref new CubicEase();
            ease->EasingMode = EasingMode::EaseOut;
            animation->EasingFunction = ease;
            Storyboard::SetTarget(animation, move);
            Storyboard::SetTargetProperty(animation, property);
            story->Children->Append(animation);
        };
        make(L"X", fromX);
        make(L"Y", fromY);
        story->Begin();
    };
    const double shiftX = (xmbCat_ - oldCat) * column;

    for (int k = 0; k < count; ++k) {
        const bool chosen = k == xmbCat_;
        const double x = anchor + (k - xmbCat_) * column;
        auto icon = ref new TextBlock();
        icon->Text = ref new Platform::String(cats[k].glyph.c_str());
        icon->FontFamily = ref new Windows::UI::Xaml::Media::FontFamily(L"Segoe MDL2 Assets");
        icon->FontSize = chosen ? 46 : 34;
        icon->Foreground = Solid(Colors::White);
        icon->Opacity = chosen ? 1.0 : 0.55;
        icon->Width = column;
        icon->TextAlignment = TextAlignment::Center;
        Canvas::SetLeft(icon, x - column / 2);
        Canvas::SetTop(icon, barTop + (chosen ? 0 : 8));
        xmbStage_->Children->Append(icon);
        slide(icon, shiftX, 0);
        if (chosen) {
            auto name = Text(ref new Platform::String(cats[k].name.c_str()), 15, false);
            name->Foreground = Solid(Colors::White);
            name->Width = column + 40;
            name->TextAlignment = TextAlignment::Center;
            Canvas::SetLeft(name, x - (column + 40) / 2);
            Canvas::SetTop(name, barTop + 56);
            xmbStage_->Children->Append(name);
        }
    }

    // The chosen category's items: the chosen one under the icons, those
    // before it above them, as the XMB lists them.
    const auto& items = cats[xmbCat_].items;
    const int chosenItem = xmbItem_[xmbCat_];
    const double left = anchor - 34;
    auto row = [&](int i, double top) {
        const auto& entry = items[i];
        const bool chosen = i == chosenItem;
        auto line = ref new StackPanel();
        line->Orientation = Orientation::Horizontal;
        const double size = chosen ? 64 : 46;
        Windows::UI::Xaml::FrameworkElement^ picture = nullptr;
        if (entry.game >= 0 && static_cast<size_t>(entry.game) < games_.size() && games_[entry.game].icon != nullptr) {
            const std::wstring key = games_[entry.game].key ? games_[entry.game].key->Data() : L"";
            auto found = xmbIcons_.find(key);
            BitmapImage^ bitmap = nullptr;
            if (found != xmbIcons_.end()) {
                bitmap = found->second;
            } else {
                bitmap = ref new BitmapImage();
                xmbIcons_[key] = bitmap;
                create_task(games_[entry.game].icon->OpenAsync(FileAccessMode::Read)).then(
                    [bitmap](task<IRandomAccessStream^> opened) {
                        try { bitmap->SetSource(opened.get()); } catch (Platform::Exception^) {}
                    }, task_continuation_context::use_current());
            }
            auto image = ref new Image();
            image->Source = bitmap;
            image->Width = size;
            image->Height = size;
            image->Stretch = Stretch::Uniform;
            picture = image;
        } else {
            auto glyph = ref new TextBlock();
            glyph->Text = ref new Platform::String(entry.glyph.c_str());
            glyph->FontFamily = ref new Windows::UI::Xaml::Media::FontFamily(L"Segoe MDL2 Assets");
            glyph->FontSize = chosen ? 34 : 26;
            glyph->Width = size;
            glyph->TextAlignment = TextAlignment::Center;
            glyph->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
            glyph->Foreground = Solid(Colors::White);
            picture = glyph;
        }
        picture->Margin = Thickness(0, 0, 16, 0);
        picture->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        line->Children->Append(picture);
        auto words = Vertical();
        words->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        auto title = Text(ref new Platform::String(entry.title.c_str()), chosen ? 20 : 15, false);
        title->Foreground = Solid(Colors::White);
        title->MaxWidth = width - left - 140;
        title->TextTrimming = TextTrimming::CharacterEllipsis;
        title->TextWrapping = TextWrapping::NoWrap;
        words->Children->Append(title);
        if (chosen && !entry.subtitle.empty()) {
            auto sub = Text(ref new Platform::String(entry.subtitle.c_str()), 12, false);
            sub->Foreground = Solid(Alpha(200, 255, 255, 255));
            sub->Margin = Thickness(0, 3, 0, 0);
            words->Children->Append(sub);
        }
        line->Children->Append(words);
        line->Opacity = chosen ? 1.0 : 0.62;
        Canvas::SetLeft(line, left - (size - 46) / 2);
        Canvas::SetTop(line, top);
        xmbStage_->Children->Append(line);
        slide(line, dx != 0 ? shiftX * 0.25 : 0, dy != 0 ? dy * 22.0 : 0);
        return size;
    };
    double y = barTop + 86;
    for (int i = chosenItem; i < static_cast<int>(items.size()) && y < height + 10; ++i)
        y += row(i, y) + 14;
    double above = barTop - 14;
    for (int i = chosenItem - 1; i >= 0 && above > -60; --i) {
        above -= 46 + 14;
        row(i, above);
    }

    // A chosen game shows its picture behind the menu, after a moment.
    for (UIElement^ child : xmb_->Children) {
        auto picture = dynamic_cast<Image^>(child);
        if (!picture || picture->Name == nullptr || !picture->Name->Equals(L"xmbPicture")) continue;
        const auto& entry = items[chosenItem];
        const auto uri = entry.game >= 0 ? BackdropUri(static_cast<size_t>(entry.game)) : ref new Platform::String(L"");
        if (uri->IsEmpty()) {
            picture->Opacity = 0;
            picture->Source = nullptr;
        } else {
            try {
                picture->Source = CachedBitmap(uri, 1280);
                auto fade = ref new DoubleAnimation();
                fade->From = 0.0;
                fade->To = 0.55;
                fade->BeginTime = Millis(450);
                fade->Duration = DurationHelper::FromTimeSpan(Millis(500));
                auto story = ref new Storyboard();
                Storyboard::SetTarget(fade, picture);
                Storyboard::SetTargetProperty(fade, L"Opacity");
                story->Children->Append(fade);
                picture->Opacity = 0;
                story->Begin();
            } catch (Platform::Exception^) {
                picture->Opacity = 0;
            }
        }
    }
}

bool CatalogShell::XmbKey(Windows::System::VirtualKey key) {
    using Windows::System::VirtualKey;
    switch (key) {
    case VirtualKey::GamepadDPadLeft: case VirtualKey::GamepadLeftThumbstickLeft: case VirtualKey::Left:
        LayoutXmb(-1, 0);
        return true;
    case VirtualKey::GamepadDPadRight: case VirtualKey::GamepadLeftThumbstickRight: case VirtualKey::Right:
        LayoutXmb(1, 0);
        return true;
    case VirtualKey::GamepadDPadUp: case VirtualKey::GamepadLeftThumbstickUp: case VirtualKey::Up:
        LayoutXmb(0, -1);
        return true;
    case VirtualKey::GamepadDPadDown: case VirtualKey::GamepadLeftThumbstickDown: case VirtualKey::Down:
        LayoutXmb(0, 1);
        return true;
    case VirtualKey::GamepadA: case VirtualKey::Enter: case VirtualKey::Space:
    case VirtualKey::GamepadY: case VirtualKey::GamepadMenu: {
        auto cats = XmbCategories();
        if (xmbCat_ < 0 || xmbCat_ >= static_cast<int>(cats.size())) return true;
        const auto& items = cats[xmbCat_].items;
        const int chosen = xmbItem_.size() > static_cast<size_t>(xmbCat_) ? xmbItem_[xmbCat_] : 0;
        if (chosen < 0 || chosen >= static_cast<int>(items.size())) return true;
        const bool options = key == VirtualKey::GamepadY || key == VirtualKey::GamepadMenu;
        const auto& entry = items[chosen];
        if (options) {
            if (entry.options) { PlayCue(1); entry.options(); }
        } else if (entry.action) {
            PlayCue(1);
            entry.action();
        }
        return true;
    }
    default:
        return false;
    }
}

} // namespace Vita3KUWP
