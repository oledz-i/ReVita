// SPDX-License-Identifier: GPL-2.0-or-later
#include "D3D12Host.h"
#include <algorithm>
#include <cmath>
#include <tmmintrin.h>
#include <windows.ui.xaml.media.dxinterop.h>
#include <dxgi1_3.h>
#include <vector>
#include <cstring>
#include <algorithm>

using Microsoft::WRL::ComPtr;

D3D12Host::~D3D12Host() { Shutdown(); }

namespace { bool g_debugLayer = false; }
bool D3D12Host::EnableDebugLayer() {
    ComPtr<ID3D12Debug> debug;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.GetAddressOf()))) || !debug) return false;
    debug->EnableDebugLayer();
    g_debugLayer = true;
    return true;
}
bool D3D12Host::DebugLayerOn() { return g_debugLayer; }

std::vector<std::string> D3D12Host::TakeDebugMessages() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> out;
    if (!infoQueue_) return out;
    // Each distinct message once (the first time it appears), then how many
    // more of it came, every 2000: one repeated warning filled the old cap
    // of 400 lines before the first refused pipeline, and its reason was lost.
    const UINT64 count = infoQueue_->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T size = 0;
        if (FAILED(infoQueue_->GetMessage(i, nullptr, &size)) || size == 0) continue;
        std::vector<char> raw(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(raw.data());
        if (FAILED(infoQueue_->GetMessage(i, message, &size))) continue;
        if (message->Severity > D3D12_MESSAGE_SEVERITY_WARNING) continue; // info and messages
        auto& seen = debugSeen_[static_cast<int>(message->ID)];
        ++seen;
        const char* level = message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR ? "error" : "warning";
        if (seen == 1 && debugSeen_.size() <= 300) {
            out.push_back(std::string(level) + " " + std::to_string(static_cast<int>(message->ID)) + ": " +
                (message->pDescription ? std::string(message->pDescription, message->DescriptionByteLength ? message->DescriptionByteLength - 1 : 0) : std::string()));
        } else if (seen % 2000 == 0) {
            out.push_back(std::string(level) + " " + std::to_string(static_cast<int>(message->ID)) + " seen " + std::to_string(seen) + " times so far");
        }
    }
    infoQueue_->ClearStoredMessages();
    return out;
}

bool D3D12Host::Fail(const wchar_t* stage, HRESULT code, std::wstring& result) {
    result = std::wstring(stage) + L" failed (HRESULT " +
        std::to_wstring(static_cast<unsigned long>(code)) + L")";
    Shutdown();
    return false;
}

void D3D12Host::Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // Frames may still be in flight (PresentFrame does not wait for its own
    // copy): let them finish before their buffers go, but never for long.
    if (queue_ && fence_ && fenceEvent_) {
        const UINT64 value = ++fenceValue_;
        if (SUCCEEDED(queue_->Signal(fence_.Get(), value)) && fence_->GetCompletedValue() < value &&
            SUCCEEDED(fence_->SetEventOnCompletion(value, fenceEvent_)))
            WaitForSingleObjectEx(fenceEvent_, 2000, FALSE);
    }
    for (unsigned i = 0; i < kBufferCount; ++i) {
        slotAllocator_[i].Reset();
        slotUpload_[i].Reset();
        slotUploadBytes_[i] = 0;
        slotFence_[i] = 0;
    }
    slot_ = 0;
    uploadBytes_ = 0;
    format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (fenceEvent_ != nullptr) {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
    frameUpload_.Reset();
    infoQueue_.Reset();
    width_ = 0;
    height_ = 0;
    fence_.Reset();
    for (auto& buffer : buffers_) buffer.Reset();
    rtvHeap_.Reset();
    commands_.Reset();
    allocator_.Reset();
    swapChain_.Reset();
    queue_.Reset();
    device_.Reset();
    rtvIncrement_ = 0;
    fenceValue_ = 0;
}

bool D3D12Host::Initialize(IUnknown* panel, unsigned width, unsigned height, std::wstring& result) {
    if (Ready()) {
        result = L"D3D12 surface already initialized";
        return true;
    }
    Shutdown();
    if (panel == nullptr || width == 0 || height == 0) {
        result = L"Invalid native SwapChainPanel or dimensions";
        return false;
    }

    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(device_.GetAddressOf()));
    if (FAILED(hr)) return Fail(L"D3D12CreateDevice", hr, result);
    if (g_debugLayer && SUCCEEDED(device_.As(&infoQueue_)) && infoQueue_) {
        infoQueue_->SetMessageCountLimit(4096);
        infoQueue_->ClearStoredMessages();
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue_.GetAddressOf()));
    if (FAILED(hr)) return Fail(L"CreateCommandQueue", hr, result);

    ComPtr<IDXGIFactory4> factory;
    hr = CreateDXGIFactory2(0, IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) return Fail(L"CreateDXGIFactory2", hr, result);

    ComPtr<ISwapChainPanelNative> panelNative;
    hr = panel->QueryInterface(IID_PPV_ARGS(panelNative.GetAddressOf()));
    if (FAILED(hr)) return Fail(L"ISwapChainPanelNative", hr, result);

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kBufferCount;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> initialSwapChain;
    hr = factory->CreateSwapChainForComposition(queue_.Get(), &desc, nullptr,
        initialSwapChain.GetAddressOf());
    if (FAILED(hr)) return Fail(L"CreateSwapChainForComposition", hr, result);

    hr = panelNative->SetSwapChain(initialSwapChain.Get());
    if (FAILED(hr)) return Fail(L"SetSwapChain", hr, result);

    hr = initialSwapChain.As(&swapChain_);
    if (FAILED(hr)) return Fail(L"IDXGISwapChain3", hr, result);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = kBufferCount;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(rtvHeap_.GetAddressOf()));
    if (FAILED(hr)) return Fail(L"Create RTV heap", hr, result);
    rtvIncrement_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (unsigned i = 0; i < kBufferCount; ++i) {
        hr = swapChain_->GetBuffer(i, IID_PPV_ARGS(buffers_[i].GetAddressOf()));
        if (FAILED(hr)) return Fail(L"GetBuffer", hr, result);
        device_->CreateRenderTargetView(buffers_[i].Get(), nullptr, handle);
        handle.ptr += rtvIncrement_;
    }

    hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(allocator_.GetAddressOf()));
    if (FAILED(hr)) return Fail(L"CreateCommandAllocator", hr, result);
    hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(),
        nullptr, IID_PPV_ARGS(commands_.GetAddressOf()));
    if (FAILED(hr)) return Fail(L"CreateCommandList", hr, result);
    hr = commands_->Close();
    if (FAILED(hr)) return Fail(L"Close initial commands", hr, result);

    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.GetAddressOf()));
    if (FAILED(hr)) return Fail(L"CreateFence", hr, result);
    fenceEvent_ = CreateEventExW(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (fenceEvent_ == nullptr) {
        return Fail(L"CreateEventExW", HRESULT_FROM_WIN32(GetLastError()), result);
    }

    width_ = width;
    height_ = height;
    result = L"D3D12 device, command queue, 2-buffer flip swap chain and RTVs initialized";
    return true;
}

bool D3D12Host::WaitForGpu(std::wstring& result) {
    const UINT64 value = ++fenceValue_;
    HRESULT hr = queue_->Signal(fence_.Get(), value);
    if (FAILED(hr)) return Fail(L"Queue::Signal", hr, result);
    return WaitForValue(value, result);
}

bool D3D12Host::WaitForValue(UINT64 value, std::wstring& result) {
    if (value == 0) return true;
    if (fence_->GetCompletedValue() < value) {
        const HRESULT hr = fence_->SetEventOnCompletion(value, fenceEvent_);
        if (FAILED(hr)) return Fail(L"SetEventOnCompletion", hr, result);
        const DWORD wait = WaitForSingleObjectEx(fenceEvent_, 5000, FALSE);
        if (wait != WAIT_OBJECT_0) {
            result = L"GPU synchronization timed out or failed";
            Shutdown();
            return false;
        }
    }
    return true;
}

bool D3D12Host::DrawTestFrame(std::wstring& result) {
    if (!Ready()) {
        result = L"D3D12 has not initialized";
        return false;
    }
    HRESULT hr = allocator_->Reset();
    if (FAILED(hr)) return Fail(L"Allocator reset", hr, result);
    hr = commands_->Reset(allocator_.Get(), nullptr);
    if (FAILED(hr)) return Fail(L"Command list reset", hr, result);

    const UINT frame = swapChain_->GetCurrentBackBufferIndex();
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = buffers_[frame].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commands_->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += frame * rtvIncrement_;
    const float clearColor[4] = {0.055f, 0.105f, 0.22f, 1.0f};
    commands_->ClearRenderTargetView(handle, clearColor, 0, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commands_->ResourceBarrier(1, &barrier);
    hr = commands_->Close();
    if (FAILED(hr)) return Fail(L"Command list close", hr, result);
    ID3D12CommandList* lists[] = { commands_.Get() };
    queue_->ExecuteCommandLists(1, lists);

    hr = swapChain_->Present(1, 0);
    if (FAILED(hr)) return Fail(L"SwapChain::Present", hr, result);
    if (!WaitForGpu(result)) return false;

    result = L"D3D12 clear and swap-chain present succeeded (graphics bootstrap; no Vita draws)";
    return true;
}

bool D3D12Host::PresentBgraFrame(const std::uint8_t* pixels, unsigned width,
                                 unsigned height, unsigned strideBytes, std::wstring& result) {
    if (!Ready() || pixels == nullptr || width == 0 || height == 0 ||
        width != width_ || height != height_ || strideBytes < width * 4u) {
        result = L"Invalid BGRA source frame or graphics device not ready";
        return false;
    }

    // D3D12 requires the buffer copy pitch to be aligned to 256 bytes.
    const unsigned rowPitch = (width * 4u + 255u) & ~255u;
    const UINT64 uploadBytes = static_cast<UINT64>(rowPitch) * height;
    if (!frameUpload_) {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = uploadBytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(frameUpload_.GetAddressOf()));
        if (FAILED(hr)) return Fail(L"Create framebuffer upload resource", hr, result);
    }

    D3D12_RANGE emptyReadRange = { 0, 0 };
    void* destination = nullptr;
    HRESULT hr = frameUpload_->Map(0, &emptyReadRange, &destination);
    if (FAILED(hr)) return Fail(L"Map framebuffer upload", hr, result);
    for (unsigned y = 0; y < height; ++y) {
        std::memcpy(static_cast<std::uint8_t*>(destination) +
                        static_cast<size_t>(y) * rowPitch,
                    pixels + static_cast<size_t>(y) * strideBytes, width * 4u);
    }
    D3D12_RANGE writtenRange = { 0, static_cast<SIZE_T>(uploadBytes) };
    frameUpload_->Unmap(0, &writtenRange);

    hr = allocator_->Reset();
    if (FAILED(hr)) return Fail(L"Framebuffer allocator reset", hr, result);
    hr = commands_->Reset(allocator_.Get(), nullptr);
    if (FAILED(hr)) return Fail(L"Framebuffer command list reset", hr, result);

    const UINT frame = swapChain_->GetCurrentBackBufferIndex();
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = buffers_[frame].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    commands_->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = frameUpload_.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = width;
    src.PlacedFootprint.Footprint.Height = height;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = rowPitch;

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = buffers_[frame].Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    commands_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commands_->ResourceBarrier(1, &barrier);
    hr = commands_->Close();
    if (FAILED(hr)) return Fail(L"Framebuffer command list close", hr, result);
    ID3D12CommandList* lists[] = { commands_.Get() };
    queue_->ExecuteCommandLists(1, lists);
    hr = swapChain_->Present(1, 0);
    if (FAILED(hr)) return Fail(L"Framebuffer Present", hr, result);
    if (!WaitForGpu(result)) return false;

    result = L"BGRA framebuffer upload, copy and GPU presentation succeeded (not Vita GPU rendering)";
    return true;
}

bool D3D12Host::PresentScaledBgraFrame(const std::uint8_t* pixels, unsigned width,
    unsigned height, unsigned strideBytes, bool sourceRgba, std::wstring& result) {
    if (!Ready() || !pixels || width == 0 || height == 0 ||
        width > 4096u || height > 4096u || strideBytes < width * 4u) {
        result = L"Invalid Vita guest framebuffer dimensions";
        return false;
    }
    if (width == width_ && height == height_ && !sourceRgba)
        return PresentBgraFrame(pixels, width, height, strideBytes, result);

    // Prototype path: CPU nearest-neighbor scaling after Vulkan->CPU readback.
    // This preserves Vita's output while the faster GPU-copy path is ported.
    // Letterbox to retain the source aspect ratio (960x544 typically).
    std::vector<std::uint8_t> scaled(static_cast<size_t>(width_) * height_ * 4, 0);
    const double factor = std::min(
        static_cast<double>(width_) / width, static_cast<double>(height_) / height);
    const unsigned fitW = std::min(width_, static_cast<unsigned>(width * factor));
    const unsigned fitH = std::min(height_, static_cast<unsigned>(height * factor));
    if (!fitW || !fitH) {
        result = L"Guest framebuffer cannot fit the Xbox display";
        return false;
    }
    const unsigned xOffset = (width_ - fitW) / 2;
    const unsigned yOffset = (height_ - fitH) / 2;
    for (unsigned y = 0; y < fitH; ++y) {
        const unsigned srcY = static_cast<unsigned>(
            (static_cast<std::uint64_t>(y) * height) / fitH);
        const auto* srcRow = pixels + static_cast<size_t>(srcY) * strideBytes;
        auto* dstRow = scaled.data() +
            (static_cast<size_t>(y + yOffset) * width_ + xOffset) * 4;
        for (unsigned x = 0; x < fitW; ++x) {
            const unsigned srcX = static_cast<unsigned>(
                (static_cast<std::uint64_t>(x) * width) / fitW);
            const auto* p = srcRow + static_cast<size_t>(srcX) * 4;
            auto* d = dstRow + static_cast<size_t>(x) * 4;
            d[0] = sourceRgba ? p[2] : p[0];
            d[1] = p[1];
            d[2] = sourceRgba ? p[0] : p[2];
            d[3] = 255;
        }
    }
    return PresentBgraFrame(
        scaled.data(), width_, height_, width_ * 4u, result);
}

bool D3D12Host::DrawUploadTestFrame(std::wstring& result) {
    if (!Ready()) {
        result = L"D3D12 must initialize before framebuffer upload";
        return false;
    }
    std::vector<std::uint8_t> pixels(static_cast<size_t>(width_) * height_ * 4u);
    for (unsigned y = 0; y < height_; ++y) {
        for (unsigned x = 0; x < width_; ++x) {
            auto* p = &pixels[(static_cast<size_t>(y) * width_ + x) * 4u];
            p[0] = static_cast<std::uint8_t>(25 + (160u * x) / width_); // blue
            p[1] = static_cast<std::uint8_t>(20 + (110u * y) / height_); // green
            p[2] = static_cast<std::uint8_t>(40 + (110u * x) / width_); // red
            p[3] = 255;
            if (y > height_ / 2 - 5 && y < height_ / 2 + 5) {
                p[0] = 255; p[1] = 210; p[2] = 70;
            }
        }
    }
    return PresentBgraFrame(pixels.data(), width_, height_, width_ * 4u, result);
}

void D3D12Host::ApplyTransform() {
    if (!swapChain_ || viewWidth_ <= 0 || viewHeight_ <= 0 || !width_ || !height_) return;
    ComPtr<IDXGISwapChain2> chain;
    if (FAILED(swapChain_.As(&chain))) return;
    // One swap-chain pixel is one DIP of the panel; scale and centre it.
    float sx = viewWidth_ / static_cast<float>(width_);
    float sy = viewHeight_ / static_cast<float>(height_);
    if (!stretch_) sx = sy = std::min(sx, sy);
    DXGI_MATRIX_3X2_F matrix = {};
    matrix._11 = sx;
    matrix._22 = sy;
    matrix._31 = (viewWidth_ - static_cast<float>(width_) * sx) / 2.0f;
    matrix._32 = (viewHeight_ - static_cast<float>(height_) * sy) / 2.0f;
    chain->SetMatrixTransform(&matrix);
}

void D3D12Host::SetViewport(float width, float height, bool stretch) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    viewWidth_ = width;
    viewHeight_ = height;
    stretch_ = stretch;
    ApplyTransform();
}

bool D3D12Host::Resize(unsigned width, unsigned height, DXGI_FORMAT format, std::wstring& result) {
    if (!WaitForGpu(result)) return false;
    for (auto& buffer : buffers_) buffer.Reset();
    HRESULT hr = swapChain_->ResizeBuffers(kBufferCount, width, height, format, 0);
    if (FAILED(hr)) return Fail(L"ResizeBuffers", hr, result);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (unsigned i = 0; i < kBufferCount; ++i) {
        hr = swapChain_->GetBuffer(i, IID_PPV_ARGS(buffers_[i].GetAddressOf()));
        if (FAILED(hr)) return Fail(L"GetBuffer after resize", hr, result);
        device_->CreateRenderTargetView(buffers_[i].Get(), nullptr, handle);
        handle.ptr += rtvIncrement_;
    }
    width_ = width;
    height_ = height;
    format_ = format;
    ApplyTransform();
    return true;
}

void D3D12Host::SetCursor(bool visible, float x, float y, bool pressed, bool rear) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    cursor_.visible = visible;
    cursor_.x = x;
    cursor_.y = y;
    cursor_.pressed = pressed;
    cursor_.rear = rear;
}

void D3D12Host::DrawCursor(std::uint8_t* bgra, unsigned rowPitch, unsigned width, unsigned height, const Cursor& cursor) {
    // Sized for the frame: about 11 pixels across on the Vita's 960x544.
    const float radius = (std::max)(5.0f, static_cast<float>(width) / 88.0f);
    const float cx = cursor.x * static_cast<float>(width - 1);
    const float cy = cursor.y * static_cast<float>(height - 1);
    const std::uint32_t fill = cursor.rear ? 0xFFFF9030u : 0xFF30A0FFu; // BGRA: blue / orange
    const int x0 = (std::max)(0, static_cast<int>(cx - radius - 3.0f));
    const int x1 = (std::min)(static_cast<int>(width) - 1, static_cast<int>(cx + radius + 3.0f));
    const int y0 = (std::max)(0, static_cast<int>(cy - radius - 3.0f));
    const int y1 = (std::min)(static_cast<int>(height) - 1, static_cast<int>(cy + radius + 3.0f));
    for (int y = y0; y <= y1; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(bgra + static_cast<size_t>(y) * rowPitch);
        for (int x = x0; x <= x1; ++x) {
            const float dx = static_cast<float>(x) - cx;
            const float dy = static_cast<float>(y) - cy;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d <= radius - 2.0f) {
                if (cursor.pressed)
                    row[x] = fill;
                else if (d <= 1.0f)
                    row[x] = 0xFFFFFFFFu;
            } else if (d <= radius) {
                row[x] = 0xFFFFFFFFu;
            } else if (d <= radius + 1.5f) {
                row[x] = 0xFF000000u;
            }
        }
    }
}

bool D3D12Host::PresentFrame(const std::uint8_t* pixels, unsigned width, unsigned height,
                             unsigned strideBytes, bool sourceRgba, std::wstring& result) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!Ready() || !pixels || width == 0 || height == 0 || width > 8192u || height > 8192u ||
        strideBytes < width * 4u) {
        result = L"Invalid game frame";
        return false;
    }
    // The swap chain stays BGRA, the format every composition swap chain
    // takes; RGBA frames are swizzled while they are copied below. Switching
    // the swap chain to RGBA meant a ResizeBuffers on the first game frame,
    // and one failure there shut the presenter down for the whole session.
    const DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (width != width_ || height != height_ || format != format_) {
        if (!Resize(width, height, format, result)) return false;
    }
    const unsigned rowPitch = (width * 4u + 255u) & ~255u;
    const UINT64 bytes = static_cast<UINT64>(rowPitch) * height;
    const unsigned slot = slot_;
    slot_ = (slot_ + 1) % kBufferCount;
    // Only the frame that last used this slot has to be done with it.
    if (!WaitForValue(slotFence_[slot], result)) return false;
    if (!slotAllocator_[slot]) {
        const HRESULT made = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(slotAllocator_[slot].GetAddressOf()));
        if (FAILED(made)) return Fail(L"Create frame allocator", made, result);
    }
    if (!slotUpload_[slot] || bytes > slotUploadBytes_[slot]) {
        slotUpload_[slot].Reset();
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const HRESULT created = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(slotUpload_[slot].GetAddressOf()));
        if (FAILED(created)) return Fail(L"Create frame upload buffer", created, result);
        slotUploadBytes_[slot] = bytes;
    }
    ID3D12Resource* upload = slotUpload_[slot].Get();
    D3D12_RANGE none = { 0, 0 };
    void* destination = nullptr;
    HRESULT hr = upload->Map(0, &none, &destination);
    if (FAILED(hr)) return Fail(L"Map frame upload", hr, result);
    auto* out = static_cast<std::uint8_t*>(destination);
    if (!sourceRgba && strideBytes == rowPitch) {
        std::memcpy(out, pixels, static_cast<size_t>(bytes));
    } else if (!sourceRgba) {
        for (unsigned y = 0; y < height; ++y)
            std::memcpy(out + static_cast<size_t>(y) * rowPitch, pixels + static_cast<size_t>(y) * strideBytes, width * 4u);
    } else {
        // RGBA to BGRA with alpha forced opaque (the core's alpha means
        // nothing for display), four pixels per SSSE3 shuffle.
        const __m128i order = _mm_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
        const __m128i opaque = _mm_set1_epi32(static_cast<int>(0xFF000000u));
        for (unsigned y = 0; y < height; ++y) {
            const auto* in = pixels + static_cast<size_t>(y) * strideBytes;
            auto* row = out + static_cast<size_t>(y) * rowPitch;
            unsigned x = 0;
            for (; x + 4 <= width; x += 4) {
                const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + x * 4u));
                _mm_storeu_si128(reinterpret_cast<__m128i*>(row + x * 4u), _mm_or_si128(_mm_shuffle_epi8(v, order), opaque));
            }
            for (; x < width; ++x) {
                std::uint32_t v;
                std::memcpy(&v, in + x * 4u, 4);
                const std::uint32_t o = 0xFF000000u | ((v & 0xFFu) << 16) | (v & 0xFF00u) | ((v >> 16) & 0xFFu);
                std::memcpy(row + x * 4u, &o, 4);
            }
        }
    }
    if (cursor_.visible) DrawCursor(out, rowPitch, width, height, cursor_);
    D3D12_RANGE written = { 0, static_cast<SIZE_T>(bytes) };
    upload->Unmap(0, &written);

    hr = slotAllocator_[slot]->Reset();
    if (FAILED(hr)) return Fail(L"Frame allocator reset", hr, result);
    hr = commands_->Reset(slotAllocator_[slot].Get(), nullptr);
    if (FAILED(hr)) return Fail(L"Frame command list reset", hr, result);
    const UINT frame = swapChain_->GetCurrentBackBufferIndex();
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = buffers_[frame].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    commands_->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = format;
    src.PlacedFootprint.Footprint.Width = width;
    src.PlacedFootprint.Footprint.Height = height;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = rowPitch;
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = buffers_[frame].Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commands_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commands_->ResourceBarrier(1, &barrier);
    hr = commands_->Close();
    if (FAILED(hr)) return Fail(L"Frame command list close", hr, result);
    ID3D12CommandList* lists[] = { commands_.Get() };
    queue_->ExecuteCommandLists(1, lists);
    // The emulation loop paces itself; do not wait for a vertical blank here as well.
    hr = swapChain_->Present(0, 0);
    if (FAILED(hr)) return Fail(L"Frame Present", hr, result);
    // No wait here: the next frame waits only for its own slot (above).
    const UINT64 value = ++fenceValue_;
    hr = queue_->Signal(fence_.Get(), value);
    if (FAILED(hr)) return Fail(L"Queue::Signal", hr, result);
    slotFence_[slot] = value;
    return true;
}
