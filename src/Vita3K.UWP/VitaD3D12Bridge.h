// SPDX-License-Identifier: GPL-3.0-or-later
// The frontend side of the core's native Direct3D 12 renderer (experimental).
// The core draws into textures on a device and queue made here and hands each
// finished frame over with set_texture; a frame is copied to the CPU in a
// ring of readback buffers, as the Vulkan bridge does, and given to the
// presenter. A shared texture straight into the swap chain is the next step.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

struct retro_hw_render_callback;

// libretro_d3d.h's retro_hw_render_interface_d3d12, with the shader compiler
// pointer left untyped (the core does not use it)
struct VitaD3D12Interface {
    unsigned interface_type;
    unsigned interface_version;
    void* handle;
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    void* D3DCompile;
    unsigned required_state;
    void (*set_texture)(void* handle, ID3D12Resource* texture, int format);
};

class VitaD3D12Bridge {
public:
    using FrameSink = std::function<void(const uint8_t*, unsigned, unsigned, bool)>;
    ~VitaD3D12Bridge();
    bool Accept(retro_hw_render_callback* callback);
    bool Initialize(std::string& error);
    void Shutdown();
    bool Present(unsigned width, unsigned height, std::string& error);
    const VitaD3D12Interface* Interface() const { return device_ ? &interface_ : nullptr; }
    bool Active() const { return accepted_; }
    FrameSink sink;
private:
    static void SetTexture(void* handle, ID3D12Resource* texture, int format);
    void DeliverReady();
    bool accepted_ = false;
    void (*contextReset_)() = nullptr;
    void (*contextDestroy_)() = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    UINT64 fenceValue_ = 0;
    VitaD3D12Interface interface_{};
    ID3D12Resource* texture_ = nullptr;   // the core's, valid until the next frame it hands over
    DXGI_FORMAT format_ = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr unsigned kSlots = 3;
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        UINT64 size = 0;
        UINT rowPitch = 0;
        unsigned width = 0, height = 0;
        bool rgba = true;
        UINT64 done = 0;     // fence value of its copy
        bool pending = false;
        uint64_t serial = 0;
    };
    Slot slots_[kSlots];
    unsigned index_ = 0;
    uint64_t serial_ = 0, shownSerial_ = 0;
    std::vector<uint8_t> packed_;
};
