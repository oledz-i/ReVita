// SPDX-License-Identifier: GPL-2.0-or-later
// Xbox UWP application entry point. The frontend is in CatalogShell.cpp.
#include <collection.h>
#include "CatalogShell.h"
#include "CrashLog.h"
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI::Xaml;

namespace Vita3KUWP {
public ref class App sealed : Application {
public:
    App() {
        // First, so the core's guest fault filter chains to it when it loads.
        vita3k_uwp::InstallCrashLog(std::wstring(Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data()) +
            L"\\vita3k-runtime.log");
        UnhandledException += ref new UnhandledExceptionEventHandler([](Platform::Object^, UnhandledExceptionEventArgs^ e) {
            std::wstring message = e->Message ? e->Message->Data() : L"";
            char line[600] = {};
            WideCharToMultiByte(CP_UTF8, 0, (L"\nCRASH: unhandled app exception 0x" + std::to_wstring(static_cast<unsigned long>(e->Exception.Value)) +
                L": " + message + L"\n").c_str(), -1, line, sizeof(line) - 1, nullptr, nullptr);
            vita3k_uwp::WriteCrashLine(line);
        });
#if !defined(VITA3K_RELEASE_BUILD)
        // Before any D3D12 device exists (turning it on later removes the
        // device): D3D12 then says in words why a call failed, which goes to
        // the log as [d3d12] lines while a game runs. It checks every call
        // the graphics driver makes and costs frame time, so only when
        // Settings, Diagnostics turned it on.
        {
            auto values = Windows::Storage::ApplicationData::Current->LocalSettings->Values;
            auto key = ref new Platform::String(L"debug.d3d12");
            if (values->HasKey(key)) {
                auto value = dynamic_cast<Platform::String^>(values->Lookup(key));
                if (value && value->Equals(L"1")) D3D12Host::EnableDebugLayer();
            }
        }
#endif
        RequiresPointerMode = ApplicationRequiresPointerMode::WhenRequested;
        ElementSoundPlayer::State = ElementSoundPlayerState::Off;
    }
protected:
    virtual void OnLaunched(LaunchActivatedEventArgs^ args) override {
        if (args->PrelaunchActivated) return;
        // Draw edge to edge; the shell keeps its own title-safe margins, so
        // backgrounds reach the screen edge instead of stopping at a border.
        Windows::UI::ViewManagement::ApplicationView::GetForCurrentView()->SetDesiredBoundsMode(
            Windows::UI::ViewManagement::ApplicationViewBoundsMode::UseCoreWindow);
        if (dynamic_cast<CatalogShell^>(Window::Current->Content) != nullptr) {
            Window::Current->Activate();
            return;
        }
        Window::Current->Content = ref new CatalogShell();
        Window::Current->Activate();
    }
};
}
[Platform::MTAThread]
int main(Platform::Array<Platform::String^>^) {
    Application::Start(ref new ApplicationInitializationCallback([](ApplicationInitializationCallbackParams^) {
        ref new Vita3KUWP::App();
    }));
    return 0;
}
