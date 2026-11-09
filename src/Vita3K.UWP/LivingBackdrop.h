// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

// The moving scene behind the menus: drifting glows, a personality per theme
// (bubbles, aurora, embers, snow, a starfield) and a vignette. Built from
// Windows.UI.Composition visuals, so every animation runs on the system
// compositor: no frame work on the UI thread, and nothing at all while a game
// runs (the scene is torn down then and rebuilt on the way back).
namespace vita3k_uwp {
class LivingBackdrop {
public:
    // host: an empty element stretched behind the whole UI.
    void Attach(Windows::UI::Xaml::FrameworkElement^ host);
    void SetTheme(int theme, Windows::UI::Color base, Windows::UI::Color accent, Windows::UI::Color glow);
    // false while a game runs: the scene is removed, so it costs nothing.
    void SetActive(bool active);

private:
    void Build();
    Windows::UI::Xaml::FrameworkElement^ host_ = nullptr;
    Windows::UI::Composition::Compositor^ compositor_ = nullptr;
    Windows::UI::Composition::ContainerVisual^ scene_ = nullptr;
    int theme_ = 0;
    Windows::UI::Color base_{};
    Windows::UI::Color accent_{};
    Windows::UI::Color glow_{};
    bool active_ = true;
    float width_ = 0;
    float height_ = 0;
};

// The XMB's flowing waves, drawn by the system compositor (no UI-thread
// frames). Attach to an element stretched where the waves go.
class XmbRibbons {
public:
    void Attach(Windows::UI::Xaml::FrameworkElement^ host);
    void SetActive(bool active);

private:
    void Build();
    Windows::UI::Xaml::FrameworkElement^ host_ = nullptr;
    Windows::UI::Composition::Compositor^ compositor_ = nullptr;
    Windows::UI::Composition::ContainerVisual^ scene_ = nullptr;
    bool active_ = true;
    float width_ = 0;
    float height_ = 0;
};
}
