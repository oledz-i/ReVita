// SPDX-License-Identifier: GPL-2.0-or-later
// Native Direct3D 12 presentation host for Xbox UWP.
// This is a working API-path experiment, not yet a PS Vita GXM renderer.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <mutex>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <map>
#include <cstdint>

class D3D12Host final {
public:
    D3D12Host() = default;
    ~D3D12Host();
    D3D12Host(const D3D12Host&) = delete;
    D3D12Host& operator=(const D3D12Host&) = delete;

    bool Initialize(IUnknown* panel, unsigned width, unsigned height, std::wstring& result);
    bool DrawTestFrame(std::wstring& result);
    bool PresentBgraFrame(const std::uint8_t* pixels, unsigned width, unsigned height,
                          unsigned strideBytes, std::wstring& result);
    bool DrawUploadTestFrame(std::wstring& result);
    bool PresentScaledBgraFrame(const std::uint8_t* pixels, unsigned width,
        unsigned height, unsigned strideBytes, bool sourceRgba,
        std::wstring& result);
    // A game frame: the swap chain follows its size and format (RGBA or
    // BGRA), so higher internal resolutions keep their detail and no pixel
    // conversion happens on the CPU. Safe to call from the game thread.
    bool PresentFrame(const std::uint8_t* pixels, unsigned width, unsigned height,
                      unsigned strideBytes, bool sourceRgba, std::wstring& result);
    // The panel's size in DIPs: the frame is scaled to fit it (keeping its
    // shape, or filling it when stretch is set) by the compositor, on the GPU.
    void SetViewport(float width, float height, bool stretch);
    // The controller's touch cursor, drawn over the next game frames at x, y
    // (0..1 across the frame): a ring, filled while touching (orange on the
    // front screen, blue on the rear touchpad).
    void SetCursor(bool visible, float x, float y, bool pressed, bool rear);
    void Shutdown();
    bool Ready() const { return swapChain_ != nullptr; }
    // The D3D12 debug layer, which says in words why a call failed (an
    // E_INVALIDARG from CreatePipelineState, for one). Only before the first
    // D3D12 device of the process; false when the console has no layer.
    static bool EnableDebugLayer();
    static bool DebugLayerOn();
    // Errors and warnings D3D12 recorded since the last call. The device is
    // one per GPU in a process, so these include Dozen's (the game's) calls.
    std::vector<std::string> TakeDebugMessages();

private:
    struct Cursor {
        bool visible = false;
        float x = 0.5f;
        float y = 0.5f;
        bool pressed = false;
        bool rear = false;
    } cursor_;
    static void DrawCursor(std::uint8_t* bgra, unsigned rowPitch, unsigned width, unsigned height, const Cursor& cursor);
    static constexpr unsigned kBufferCount = 2;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffers_[kBufferCount];
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    Microsoft::WRL::ComPtr<ID3D12Resource> frameUpload_;
    // Game frames: one upload buffer and allocator per back buffer, so the
    // CPU fills the next frame while the GPU still copies the last one
    // (PresentFrame waits only for the frame that used the same slot).
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> slotAllocator_[kBufferCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> slotUpload_[kBufferCount];
    UINT64 slotUploadBytes_[kBufferCount] = {};
    UINT64 slotFence_[kBufferCount] = {};
    unsigned slot_ = 0;
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue_;
    std::map<int, unsigned long long> debugSeen_; // message ID -> times seen (whole session)
    unsigned width_ = 0;
    unsigned height_ = 0;
    HANDLE fenceEvent_ = nullptr;
    UINT64 fenceValue_ = 0;
    UINT rtvIncrement_ = 0;

    DXGI_FORMAT format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    UINT64 uploadBytes_ = 0;
    float viewWidth_ = 0, viewHeight_ = 0;
    bool stretch_ = false;
    std::recursive_mutex mutex_;
    bool Resize(unsigned width, unsigned height, DXGI_FORMAT format, std::wstring& result);
    void ApplyTransform();
    bool Fail(const wchar_t* stage, HRESULT code, std::wstring& result);
    bool WaitForGpu(std::wstring& result);
    bool WaitForValue(UINT64 value, std::wstring& result);
};
