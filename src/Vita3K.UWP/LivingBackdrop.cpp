// SPDX-License-Identifier: GPL-2.0-or-later
#include <collection.h>
#include "LivingBackdrop.h"
#include <WindowsNumerics.h>
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

using namespace Windows::Foundation;
using namespace Windows::Foundation::Numerics;
using namespace Windows::UI;
using namespace Windows::UI::Composition;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Hosting;

namespace vita3k_uwp {
namespace {
TimeSpan Seconds(double s) {
    TimeSpan t;
    t.Duration = static_cast<long long>(s * 10000000.0);
    return t;
}
Color WithAlpha(Color c, int a) {
    return ColorHelper::FromArgb(static_cast<unsigned char>((std::max)(0, (std::min)(255, a))), c.R, c.G, c.B);
}
Color Scale(Color c, double f) {
    const auto ch = [f](unsigned char v) {
        return static_cast<unsigned char>((std::max)(0.0, (std::min)(255.0, v * f)));
    };
    return ColorHelper::FromArgb(255, ch(c.R), ch(c.G), ch(c.B));
}
Color Mix(Color a, Color b, double t) {
    const auto ch = [t](unsigned char x, unsigned char y) {
        return static_cast<unsigned char>(x + (y - x) * t);
    };
    return ColorHelper::FromArgb(255, ch(a.R, b.R), ch(a.G, b.G), ch(a.B, b.B));
}

struct Scene {
    Compositor^ c;
    ContainerVisual^ root;
    float w;
    float h;
    std::mt19937 rng;
    CompositionEasingFunction^ smooth;

    float Rand(float lo, float hi) {
        std::uniform_real_distribution<float> d(lo, hi);
        return d(rng);
    }

    // A soft round light: bright core fading to nothing.
    CompositionBrush^ Glow(Color color, int core, float mid = 0.45f) {
        auto brush = c->CreateRadialGradientBrush();
        brush->ColorStops->Append(c->CreateColorGradientStop(0.0f, WithAlpha(color, core)));
        brush->ColorStops->Append(c->CreateColorGradientStop(mid, WithAlpha(color, core / 3)));
        brush->ColorStops->Append(c->CreateColorGradientStop(1.0f, WithAlpha(color, 0)));
        return brush;
    }

    // A ring with a lit rim, like the bubbles of the Vita's home screen.
    CompositionBrush^ Ring(Color color, int rim) {
        auto brush = c->CreateRadialGradientBrush();
        brush->ColorStops->Append(c->CreateColorGradientStop(0.0f, WithAlpha(color, rim / 10)));
        brush->ColorStops->Append(c->CreateColorGradientStop(0.70f, WithAlpha(color, rim / 6)));
        brush->ColorStops->Append(c->CreateColorGradientStop(0.88f, WithAlpha(color, rim)));
        brush->ColorStops->Append(c->CreateColorGradientStop(0.96f, WithAlpha(color, rim / 3)));
        brush->ColorStops->Append(c->CreateColorGradientStop(1.0f, WithAlpha(color, 0)));
        return brush;
    }

    SpriteVisual^ Sprite(CompositionBrush^ brush, float width, float height, float x, float y) {
        auto sprite = c->CreateSpriteVisual();
        sprite->Brush = brush;
        sprite->Size = float2(width, height);
        sprite->Offset = float3(x, y, 0);
        root->Children->InsertAtTop(sprite);
        return sprite;
    }

    void Loop(Visual^ visual, Platform::String^ property, const std::vector<float3>& points, double seconds, double delay = 0) {
        auto animation = c->CreateVector3KeyFrameAnimation();
        const size_t count = points.size();
        for (size_t i = 0; i < count; ++i)
            animation->InsertKeyFrame(count == 1 ? 1.0f : static_cast<float>(i) / static_cast<float>(count - 1), points[i], smooth);
        animation->Duration = Seconds(seconds);
        animation->DelayTime = Seconds(delay);
        animation->IterationBehavior = AnimationIterationBehavior::Forever;
        visual->StartAnimation(property, animation);
    }

    void LoopScalar(Visual^ visual, Platform::String^ property, const std::vector<float>& values, double seconds, double delay = 0,
                    bool linear = false) {
        auto animation = c->CreateScalarKeyFrameAnimation();
        const size_t count = values.size();
        for (size_t i = 0; i < count; ++i) {
            const float progress = count == 1 ? 1.0f : static_cast<float>(i) / static_cast<float>(count - 1);
            if (linear) animation->InsertKeyFrame(progress, values[i], c->CreateLinearEasingFunction());
            else animation->InsertKeyFrame(progress, values[i], smooth);
        }
        animation->Duration = Seconds(seconds);
        animation->DelayTime = Seconds(delay);
        animation->IterationBehavior = AnimationIterationBehavior::Forever;
        visual->StartAnimation(property, animation);
    }

    // Large lights wandering slowly across the screen.
    void Wanderers(Color a, Color b, int count, int core) {
        for (int i = 0; i < count; ++i) {
            const float size = Rand(0.55f, 0.95f) * (std::max)(w, h);
            const Color color = i % 2 == 0 ? a : b;
            auto glow = Sprite(Glow(color, core), size, size, 0, 0);
            glow->CenterPoint = float3(size / 2, size / 2, 0);
            std::vector<float3> path;
            for (int k = 0; k < 4; ++k)
                path.push_back(float3(Rand(-0.45f, 0.75f) * w, Rand(-0.5f, 0.6f) * h, 0));
            path.push_back(path.front());
            Loop(glow, L"Offset", path, Rand(38, 70), 0);
            Loop(glow, L"Scale", { float3(1, 1, 1), float3(1.18f, 1.18f, 1), float3(0.92f, 0.92f, 1), float3(1, 1, 1) }, Rand(18, 30));
        }
    }

    // Particles that travel from one edge to the other, fading in and out.
    void Stream(CompositionBrush^ brush, int count, float minSize, float maxSize, bool upward, float sway,
                float minSeconds, float maxSeconds, float peakOpacity) {
        for (int i = 0; i < count; ++i) {
            const float size = Rand(minSize, maxSize);
            const float x = Rand(-0.05f, 1.0f) * w;
            const float start = upward ? h + size : -size;
            const float end = upward ? -size * 2 : h + size;
            auto particle = Sprite(brush, size, size, x, start);
            particle->Opacity = 0;
            const float drift = Rand(-sway, sway);
            const double seconds = Rand(minSeconds, maxSeconds);
            const double delay = Rand(0, static_cast<float>(seconds));
            Loop(particle, L"Offset", {
                float3(x, start, 0),
                float3(x + drift, start + (end - start) * 0.33f, 0),
                float3(x - drift * 0.6f, start + (end - start) * 0.66f, 0),
                float3(x + drift * 0.4f, end, 0) }, seconds, delay);
            const float peak = peakOpacity * Rand(0.55f, 1.0f);
            LoopScalar(particle, L"Opacity", { 0.0f, peak, peak * 0.85f, 0.0f }, seconds, delay);
        }
    }

    // Points that stay put and twinkle.
    void Stars(Color color, int count) {
        auto brush = Glow(color, 255, 0.25f);
        for (int i = 0; i < count; ++i) {
            const float size = Rand(3, 9);
            auto star = Sprite(brush, size, size, Rand(0, 1) * w, Rand(0, 1) * h);
            const float low = Rand(0.05f, 0.3f);
            const float high = Rand(0.5f, 1.0f);
            LoopScalar(star, L"Opacity", { low, high, low }, Rand(2.5f, 7.0f), Rand(0, 4));
        }
    }

    // Things that fall turning over (petals, leaves): sway, spin and fade.
    void Tumble(CompositionBrush^ brush, int count, float minSize, float maxSize, float sway,
                float minSeconds, float maxSeconds, float peakOpacity) {
        for (int i = 0; i < count; ++i) {
            const float size = Rand(minSize, maxSize);
            const float x = Rand(-0.1f, 1.05f) * w;
            auto petal = Sprite(brush, size, size * 0.62f, x, -size);
            petal->CenterPoint = float3(size / 2, size * 0.31f, 0);
            petal->Opacity = 0;
            const float drift = Rand(sway * 0.4f, sway) * (Rand(0, 1) < 0.5f ? -1.0f : 1.0f);
            const double seconds = Rand(minSeconds, maxSeconds);
            const double delay = Rand(0, static_cast<float>(seconds));
            Loop(petal, L"Offset", {
                float3(x, -size, 0),
                float3(x + drift, h * 0.3f, 0),
                float3(x - drift * 0.4f, h * 0.62f, 0),
                float3(x + drift * 1.2f, h + size, 0) }, seconds, delay);
            const float spin = Rand(0, 1) < 0.5f ? -360.0f : 360.0f;
            LoopScalar(petal, L"RotationAngleInDegrees", { 0.0f, spin }, Rand(5.0f, 11.0f), delay, true);
            const float peak = peakOpacity * Rand(0.6f, 1.0f);
            LoopScalar(petal, L"Opacity", { 0.0f, peak, peak, 0.0f }, seconds, delay);
        }
    }

    // Small lights wandering on their own and blinking (fireflies).
    void Fireflies(Color color, int count) {
        auto brush = Glow(color, 255, 0.22f);
        for (int i = 0; i < count; ++i) {
            const float size = Rand(8, 18);
            auto fly = Sprite(brush, size, size, Rand(0, 1) * w, Rand(0.25f, 1.0f) * h);
            std::vector<float3> path;
            for (int k = 0; k < 4; ++k)
                path.push_back(float3(Rand(0.0f, 1.0f) * w, Rand(0.2f, 1.0f) * h, 0));
            path.push_back(path.front());
            Loop(fly, L"Offset", path, Rand(26, 48), Rand(0, 6));
            LoopScalar(fly, L"Opacity", { 0.05f, 0.05f, 0.95f, 0.3f, 0.05f }, Rand(2.8f, 6.5f), Rand(0, 5));
        }
    }

    // Soft beams of light leaning in from the top.
    void Beams(Color color, int count) {
        for (int i = 0; i < count; ++i) {
            const float bw = w * Rand(0.12f, 0.22f);
            const float bh = h * 1.5f;
            auto brush = c->CreateLinearGradientBrush();
            brush->StartPoint = float2(0.5f, 0);
            brush->EndPoint = float2(0.5f, 1);
            brush->ColorStops->Append(c->CreateColorGradientStop(0.0f, WithAlpha(color, 70)));
            brush->ColorStops->Append(c->CreateColorGradientStop(0.6f, WithAlpha(color, 18)));
            brush->ColorStops->Append(c->CreateColorGradientStop(1.0f, WithAlpha(color, 0)));
            auto beam = Sprite(brush, bw, bh, w * (0.15f + 0.28f * i), -h * 0.15f);
            beam->CenterPoint = float3(bw / 2, 0, 0);
            beam->RotationAngleInDegrees = Rand(18, 28);
            LoopScalar(beam, L"Opacity", { 0.25f, 0.8f, 0.4f, 0.25f }, Rand(10, 18), Rand(0, 4));
            LoopScalar(beam, L"RotationAngleInDegrees", { 20.0f, 26.0f, 20.0f }, Rand(24, 36));
        }
    }

    // A wide shimmer that sweeps across now and then.
    void Sweep(Color color, double seconds) {
        auto brush = c->CreateLinearGradientBrush();
        brush->StartPoint = float2(0, 0.5f);
        brush->EndPoint = float2(1, 0.5f);
        brush->ColorStops->Append(c->CreateColorGradientStop(0.0f, WithAlpha(color, 0)));
        brush->ColorStops->Append(c->CreateColorGradientStop(0.5f, WithAlpha(color, 46)));
        brush->ColorStops->Append(c->CreateColorGradientStop(1.0f, WithAlpha(color, 0)));
        const float sw = w * 0.35f;
        auto band = Sprite(brush, sw, h * 1.6f, -sw, -h * 0.3f);
        band->CenterPoint = float3(sw / 2, h * 0.8f, 0);
        band->RotationAngleInDegrees = 18;
        Loop(band, L"Offset", { float3(-sw * 1.5f, -h * 0.3f, 0), float3(-sw * 1.5f, -h * 0.3f, 0),
                                float3(w + sw, -h * 0.3f, 0) }, seconds);
    }

    void Vignette(Color edge) {
        auto brush = c->CreateRadialGradientBrush();
        brush->EllipseRadius = float2(0.75f, 0.75f);
        brush->ColorStops->Append(c->CreateColorGradientStop(0.0f, WithAlpha(edge, 0)));
        brush->ColorStops->Append(c->CreateColorGradientStop(0.55f, WithAlpha(edge, 0)));
        brush->ColorStops->Append(c->CreateColorGradientStop(1.0f, WithAlpha(edge, 200)));
        Sprite(brush, w, h, 0, 0);
    }
};
}

void XmbRibbons::Attach(FrameworkElement^ host) {
    host_ = host;
    compositor_ = ElementCompositionPreview::GetElementVisual(host)->Compositor;
    scene_ = compositor_->CreateContainerVisual();
    ElementCompositionPreview::SetElementChildVisual(host, scene_);
    host->SizeChanged += ref new SizeChangedEventHandler([this](Platform::Object^, SizeChangedEventArgs^ e) {
        const float w = static_cast<float>(e->NewSize.Width);
        const float h = static_cast<float>(e->NewSize.Height);
        if (std::fabs(w - width_) < 1 && std::fabs(h - height_) < 1) return;
        width_ = w;
        height_ = h;
        Build();
    });
}

void XmbRibbons::SetActive(bool active) {
    if (active == active_) return;
    active_ = active;
    Build();
}

// The PS3's flowing ribbons, all on the compositor: each is a long band with
// a bright thin core that drifts, tilts and breathes on its own clock, so the
// bands cross and part like the real waves without any frame work here.
void XmbRibbons::Build() {
    if (!scene_) return;
    scene_->Children->RemoveAll();
    if (!active_ || width_ < 2 || height_ < 2) return;
    scene_->Size = float2(width_, height_);
    Scene s{ compositor_, scene_, width_, height_, std::mt19937(4242u), nullptr };
    s.smooth = compositor_->CreateCubicBezierEasingFunction(float2(0.42f, 0.0f), float2(0.58f, 1.0f));
    const Color white = ColorHelper::FromArgb(255, 255, 255, 255);
    struct Band { float y, thick, core, tilt, seconds; };
    const Band bands[] = {
        { 0.54f, 0.20f, 120, -5.0f, 21 },
        { 0.57f, 0.14f, 90, 3.5f, 27 },
        { 0.60f, 0.26f, 60, -2.0f, 33 },
        { 0.52f, 0.10f, 140, 1.5f, 17 },
        { 0.62f, 0.18f, 45, 4.5f, 39 },
    };
    for (const Band& b : bands) {
        const float bw = width_ * 2.6f;
        const float bh = height_ * b.thick;
        auto brush = compositor_->CreateLinearGradientBrush();
        brush->StartPoint = float2(0.5f, 0);
        brush->EndPoint = float2(0.5f, 1);
        brush->ColorStops->Append(compositor_->CreateColorGradientStop(0.0f, WithAlpha(white, 0)));
        brush->ColorStops->Append(compositor_->CreateColorGradientStop(0.38f, WithAlpha(white, static_cast<int>(b.core / 6))));
        brush->ColorStops->Append(compositor_->CreateColorGradientStop(0.49f, WithAlpha(white, static_cast<int>(b.core))));
        brush->ColorStops->Append(compositor_->CreateColorGradientStop(0.52f, WithAlpha(white, static_cast<int>(b.core / 2))));
        brush->ColorStops->Append(compositor_->CreateColorGradientStop(0.72f, WithAlpha(white, static_cast<int>(b.core / 9))));
        brush->ColorStops->Append(compositor_->CreateColorGradientStop(1.0f, WithAlpha(white, 0)));
        const float x = -width_ * 0.8f;
        const float y = height_ * b.y - bh / 2;
        auto band = s.Sprite(brush, bw, bh, x, y);
        band->CenterPoint = float3(bw / 2, bh / 2, 0);
        band->RotationAngleInDegrees = b.tilt;
        s.Loop(band, L"Offset", { float3(x, y, 0), float3(x + width_ * 0.22f, y - height_ * 0.035f, 0),
                                  float3(x - width_ * 0.1f, y + height_ * 0.03f, 0), float3(x, y, 0) }, b.seconds);
        s.LoopScalar(band, L"RotationAngleInDegrees", { b.tilt, -b.tilt * 0.8f, b.tilt * 0.5f, b.tilt }, b.seconds * 1.3);
        s.Loop(band, L"Scale", { float3(1, 1, 1), float3(1.05f, 1.45f, 1), float3(0.97f, 0.75f, 1), float3(1, 1, 1) }, b.seconds * 0.8);
    }
    // glints travelling along the waves
    for (int i = 0; i < 16; ++i) {
        const float size = s.Rand(3, 7);
        const float y = height_ * s.Rand(0.5f, 0.64f);
        auto glint = s.Sprite(s.Glow(white, 255, 0.25f), size, size, -size, y);
        const double seconds = s.Rand(14, 26);
        const double delay = s.Rand(0, static_cast<float>(seconds));
        s.Loop(glint, L"Offset", { float3(-size, y, 0), float3(width_ * 0.5f, y - height_ * 0.03f, 0), float3(width_ + size, y + height_ * 0.02f, 0) }, seconds, delay);
        s.LoopScalar(glint, L"Opacity", { 0.0f, 0.8f, 0.0f }, seconds, delay);
    }
}

void LivingBackdrop::Attach(FrameworkElement^ host) {
    host_ = host;
    compositor_ = ElementCompositionPreview::GetElementVisual(host)->Compositor;
    scene_ = compositor_->CreateContainerVisual();
    ElementCompositionPreview::SetElementChildVisual(host, scene_);
    host->SizeChanged += ref new SizeChangedEventHandler([this](Platform::Object^, SizeChangedEventArgs^ e) {
        const float w = static_cast<float>(e->NewSize.Width);
        const float h = static_cast<float>(e->NewSize.Height);
        if (std::fabs(w - width_) < 1 && std::fabs(h - height_) < 1) return;
        width_ = w;
        height_ = h;
        Build();
    });
}

void LivingBackdrop::SetTheme(int theme, Color base, Color accent, Color glow) {
    theme_ = theme;
    base_ = base;
    accent_ = accent;
    glow_ = glow;
    Build();
}

void LivingBackdrop::SetActive(bool active) {
    if (active == active_) return;
    active_ = active;
    Build();
}

void LivingBackdrop::Build() {
    if (!scene_) return;
    // Removing the visuals ends their animations as well.
    scene_->Children->RemoveAll();
    if (!active_ || width_ < 2 || height_ < 2) return;
    scene_->Size = float2(width_, height_);

    Scene s{ compositor_, scene_, width_, height_, std::mt19937(static_cast<unsigned>(theme_) * 7919u + 17u), nullptr };
    s.smooth = compositor_->CreateCubicBezierEasingFunction(float2(0.45f, 0.0f), float2(0.55f, 1.0f));

    // The floor: the theme's base colour, lifted towards the top corner.
    auto floor = compositor_->CreateLinearGradientBrush();
    floor->StartPoint = float2(0, 0);
    floor->EndPoint = float2(1, 1);
    floor->ColorStops->Append(compositor_->CreateColorGradientStop(0.0f, Mix(Scale(base_, 1.6), glow_, 0.35)));
    floor->ColorStops->Append(compositor_->CreateColorGradientStop(0.55f, base_));
    floor->ColorStops->Append(compositor_->CreateColorGradientStop(1.0f, Scale(base_, 0.6)));
    s.Sprite(floor, width_, height_, 0, 0);

    const Color white = ColorHelper::FromArgb(255, 255, 255, 255);
    // Synthwave and Miami Night: a striped sun on the horizon, a neon floor rolling towards you
    auto synthwave = [&](Color sunTop, Color sunBottom, Color neonColor) {
        s.Stars(white, 60);
        const float horizon = height_ * 0.58f;
        const float sun = (std::min)(width_, height_) * 0.62f;
        auto sunBrush = compositor_->CreateLinearGradientBrush();
        sunBrush->StartPoint = float2(0.5f, 0);
        sunBrush->EndPoint = float2(0.5f, 1);
        sunBrush->ColorStops->Append(compositor_->CreateColorGradientStop(0.0f, sunTop));
        sunBrush->ColorStops->Append(compositor_->CreateColorGradientStop(1.0f, sunBottom));
        auto disc = compositor_->CreateEllipseGeometry();
        disc->Radius = float2(sun / 2, sun / 2);
        disc->Center = float2(sun / 2, sun / 2);
        auto discShape = compositor_->CreateSpriteShape(disc);
        discShape->FillBrush = sunBrush;
        auto sunVisual = compositor_->CreateShapeVisual();
        sunVisual->Size = float2(sun, sun);
        sunVisual->Shapes->Append(discShape);
        sunVisual->Offset = float3(width_ / 2 - sun / 2, horizon - sun * 0.72f, 0);
        scene_->Children->InsertAtTop(sunVisual);
        auto halo = s.Sprite(s.Glow(accent_, 150, 0.35f), sun * 2.2f, sun * 2.2f, width_ / 2 - sun * 1.1f, horizon - sun * 1.6f);
        s.LoopScalar(halo, L"Opacity", { 0.6f, 0.95f, 0.6f }, 7);
        // stripes cut across the lower half of the sun, sliding down
        for (int i = 0; i < 6; ++i) {
            const float stripeH = 3.0f + i * 1.6f;
            auto stripe = s.Sprite(compositor_->CreateColorBrush(base_), sun * 1.05f, stripeH,
                                   width_ / 2 - sun * 0.525f, horizon - sun * 0.4f + i * sun * 0.06f);
            const float y = stripe->Offset.y;
            s.Loop(stripe, L"Offset", { float3(stripe->Offset.x, y, 0), float3(stripe->Offset.x, y + sun * 0.06f, 0) }, 3.2, 0);
        }
        // the floor
        s.Sprite(compositor_->CreateColorBrush(Scale(base_, 0.8)), width_, height_ - horizon, 0, horizon);
        const Color neon = neonColor;
        for (int i = 0; i < 9; ++i) {
            auto line = s.Sprite(compositor_->CreateColorBrush(WithAlpha(neon, 210)), width_, 2, 0, horizon);
            line->Opacity = 0;
            s.Loop(line, L"Offset", { float3(0, horizon, 0), float3(0, horizon + (height_ - horizon) * 0.25f, 0),
                                      float3(0, static_cast<float>(height_), 0) }, 4.5, i * 0.5);
            s.LoopScalar(line, L"Opacity", { 0.0f, 0.5f, 0.9f }, 4.5, i * 0.5);
        }
        for (int i = -7; i <= 7; ++i) {
            const float len = (height_ - horizon) * 2.2f;
            auto ray = s.Sprite(compositor_->CreateColorBrush(WithAlpha(neon, 150)), 2, len, width_ / 2, horizon);
            ray->CenterPoint = float3(1, 0, 0);
            ray->RotationAngleInDegrees = -i * 9.0f;
        }
        auto haze = s.Sprite(s.Glow(accent_, 120, 0.4f), width_ * 1.4f, height_ * 0.5f, -width_ * 0.2f, horizon - height_ * 0.22f);
        s.LoopScalar(haze, L"Opacity", { 0.5f, 0.85f, 0.5f }, 9);
    };

    // ReVita's eight themes, each drawn by one of the scenes below
    //   0 ReVita, 1 Synthwave, 2 Miami Night, 3 Pastel Dream, 4 Ultraviolet,
    //   5 Cotton Sky, 6 Midnight Arcade, 7 Starfall
    static const int sceneOf[8] = { 0, 6, 12, 5, 1, 13, 11, 4 };
    const int scene = (theme_ >= 0 && theme_ < 8) ? sceneOf[theme_] : 0;
    switch (scene) {
    case 1: { // Aurora: curtains of light swaying over a dark sea, drifting motes
        s.Wanderers(glow_, Mix(glow_, accent_, 0.4), 2, 150);
        for (int i = 0; i < 3; ++i) {
            const float bandW = width_ * s.Rand(0.9f, 1.3f);
            const float bandH = height_ * s.Rand(0.28f, 0.42f);
            auto band = s.Sprite(s.Glow(i == 1 ? Mix(accent_, white, 0.25) : accent_, 105, 0.35f), bandW, bandH,
                                 s.Rand(-0.3f, 0.1f) * width_, height_ * (0.08f + 0.17f * i));
            band->CenterPoint = float3(bandW / 2, bandH / 2, 0);
            band->RotationAngleInDegrees = s.Rand(-14, -4);
            const float x = band->Offset.x;
            const float y = band->Offset.y;
            s.Loop(band, L"Offset", { float3(x, y, 0), float3(x + width_ * 0.18f, y - height_ * 0.05f, 0),
                                      float3(x - width_ * 0.08f, y + height_ * 0.04f, 0), float3(x, y, 0) }, s.Rand(26, 40));
            s.LoopScalar(band, L"Opacity", { 0.35f, 0.9f, 0.5f, 0.35f }, s.Rand(14, 22));
            s.LoopScalar(band, L"RotationAngleInDegrees", { -12.0f, -4.0f, -12.0f }, s.Rand(30, 44));
        }
        s.Stream(s.Glow(Mix(accent_, white, 0.5), 230, 0.3f), 26, 4, 10, true, 70, 22, 38, 0.7f);
        break;
    }
    case 2: { // Ember: a warm stage, sparks rising from below
        s.Wanderers(glow_, Mix(glow_, accent_, 0.5), 3, 170);
        auto heat = s.Sprite(s.Glow(accent_, 120, 0.4f), width_ * 1.4f, height_ * 0.9f, -width_ * 0.2f, height_ * 0.55f);
        s.LoopScalar(heat, L"Opacity", { 0.55f, 0.9f, 0.6f, 0.55f }, 9);
        s.Stream(s.Glow(Mix(accent_, ColorHelper::FromArgb(255, 255, 214, 140), 0.5), 255, 0.25f), 46, 3, 9, true, 90, 7, 15, 0.95f);
        break;
    }
    case 3: { // Glacier: cold light and snow falling past
        s.Wanderers(glow_, Mix(glow_, accent_, 0.3), 3, 140);
        s.Stream(s.Glow(white, 235, 0.35f), 58, 3, 11, false, 110, 14, 30, 0.85f);
        break;
    }
    case 4: { // Orbit: a starfield, a planet glow and a ring turning around it
        s.Stars(white, 90);
        s.Wanderers(glow_, Mix(glow_, accent_, 0.5), 2, 130);
        const float planet = (std::min)(width_, height_) * 0.62f;
        const float px = width_ * 0.66f;
        const float py = height_ * 0.18f;
        auto halo = s.Sprite(s.Glow(accent_, 140, 0.3f), planet, planet, px, py);
        s.LoopScalar(halo, L"Opacity", { 0.6f, 1.0f, 0.6f }, 12);
        const float ringW = planet * 1.9f;
        const float ringH = planet * 0.5f;
        auto ring = s.Sprite(s.Ring(Mix(accent_, white, 0.3), 170), ringW, ringH,
                             px + planet / 2 - ringW / 2, py + planet / 2 - ringH / 2);
        ring->CenterPoint = float3(ringW / 2, ringH / 2, 0);
        s.LoopScalar(ring, L"RotationAngleInDegrees", { -18.0f, -8.0f, -18.0f }, 26);
        auto moon = s.Sprite(s.Glow(white, 255, 0.2f), 18, 18, px, py);
        const float cx = px + planet / 2 - 9;
        const float cy = py + planet / 2 - 9;
        s.Loop(moon, L"Offset", { float3(cx - ringW * 0.46f, cy + ringH * 0.1f, 0), float3(cx, cy + ringH * 0.42f, 0),
                                  float3(cx + ringW * 0.46f, cy - ringH * 0.1f, 0), float3(cx, cy - ringH * 0.42f, 0),
                                  float3(cx - ringW * 0.46f, cy + ringH * 0.1f, 0) }, 24);
        break;
    }
    case 5: { // Sakura: plum dusk, blossoms tumbling down, a few sparkles
        s.Wanderers(glow_, Mix(glow_, accent_, 0.5), 3, 150);
        s.Tumble(s.Glow(Mix(accent_, white, 0.45), 235, 0.55f), 34, 10, 22, 160, 12, 22, 0.9f);
        s.Tumble(s.Glow(Mix(accent_, white, 0.2), 200, 0.5f), 14, 18, 30, 220, 16, 26, 0.6f);
        s.Stream(s.Glow(white, 220, 0.25f), 14, 3, 6, true, 40, 18, 30, 0.5f);
        break;
    }
    case 6: { // Synthwave: golden-pink sun over a magenta grid
        synthwave(ColorHelper::FromArgb(255, 255, 206, 120), WithAlpha(accent_, 255), Mix(accent_, white, 0.15));
        break;
    }
    case 12: { // Miami Night: a pink sun sinking into a cyan grid
        synthwave(ColorHelper::FromArgb(255, 255, 150, 210), ColorHelper::FromArgb(255, 255, 70, 160), Mix(accent_, white, 0.1));
        break;
    }
    case 13: { // Cotton Sky: blue and pink clouds drifting past soft stars
        s.Stars(white, 40);
        s.Wanderers(glow_, Mix(glow_, accent_, 0.5), 4, 190);
        s.Tumble(s.Glow(Mix(accent_, white, 0.4), 160, 0.6f), 10, 40, 80, 260, 26, 40, 0.5f);
        s.Tumble(s.Glow(Mix(glow_, white, 0.45), 150, 0.6f), 8, 50, 90, 300, 30, 46, 0.45f);
        break;
    }
    case 7: { // Verdant: green mist, light through the canopy, fireflies
        s.Wanderers(glow_, Mix(glow_, accent_, 0.35), 3, 150);
        s.Beams(Mix(accent_, ColorHelper::FromArgb(255, 255, 248, 200), 0.6), 3);
        s.Fireflies(Mix(accent_, ColorHelper::FromArgb(255, 255, 250, 150), 0.65), 34);
        s.Tumble(s.Glow(accent_, 170, 0.5f), 8, 12, 20, 180, 18, 30, 0.45f);
        break;
    }
    case 8: { // Gilded: black and gold, slow warm light, gold dust and a shimmer
        s.Wanderers(glow_, Mix(glow_, accent_, 0.4), 3, 160);
        s.Stream(s.Glow(Mix(accent_, white, 0.35), 240, 0.25f), 48, 2, 7, true, 140, 16, 32, 0.8f);
        s.Stars(Mix(accent_, white, 0.5), 30);
        s.Sweep(Mix(accent_, white, 0.4), 11);
        break;
    }
    case 9: { // Abyss: light falling through deep water, bubbles rising, drifting plankton
        s.Wanderers(glow_, Mix(glow_, accent_, 0.3), 3, 160);
        s.Beams(Mix(accent_, white, 0.5), 4);
        s.Stream(s.Ring(Mix(accent_, white, 0.4), 160), 22, 8, 30, true, 50, 14, 28, 0.75f);
        s.Fireflies(Mix(accent_, white, 0.3), 26);
        break;
    }
    case 10: { // Crimson: black and red, a slow red tide and ash drifting down
        s.Wanderers(glow_, Mix(glow_, accent_, 0.55), 3, 175);
        auto tide = s.Sprite(s.Glow(accent_, 110, 0.4f), width_ * 1.5f, height_ * 0.8f, -width_ * 0.25f, height_ * 0.6f);
        s.LoopScalar(tide, L"Opacity", { 0.4f, 0.85f, 0.4f }, 11);
        s.Stream(s.Glow(Mix(accent_, white, 0.2), 230, 0.3f), 34, 2, 7, false, 120, 16, 30, 0.8f);
        s.Sweep(Mix(accent_, white, 0.3), 13);
        break;
    }
    case 11: { // Prism: graphite glass, light sweeping across and soft glints
        s.Wanderers(glow_, Mix(glow_, accent_, 0.4), 3, 150);
        s.Sweep(Mix(accent_, white, 0.6), 9);
        s.Stars(Mix(accent_, white, 0.7), 46);
        s.Stream(s.Ring(Mix(accent_, white, 0.5), 120), 12, 30, 110, true, 90, 30, 52, 0.6f);
        break;
    }
    default: { // ReVita: violet night, stars, cyan and pink light, bubbles floating up
        s.Stars(white, 50);
        s.Wanderers(glow_, Mix(glow_, accent_, 0.45), 3, 165);
        s.Wanderers(ColorHelper::FromArgb(255, 47, 150, 200), ColorHelper::FromArgb(255, 200, 60, 150), 2, 120);
        s.Stream(s.Ring(Mix(accent_, white, 0.35), 150), 18, 40, 140, true, 120, 26, 48, 0.7f);
        s.Stream(s.Glow(Mix(accent_, white, 0.6), 220, 0.3f), 20, 4, 9, true, 60, 18, 30, 0.6f);
        break;
    }
    }
    s.Vignette(Scale(base_, 0.35));
}
}
