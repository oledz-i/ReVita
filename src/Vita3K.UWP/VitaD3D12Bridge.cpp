// SPDX-License-Identifier: GPL-3.0-or-later
#include "VitaD3D12Bridge.h"
#include <libretro.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

VitaD3D12Bridge::~VitaD3D12Bridge() { Shutdown(); }

bool VitaD3D12Bridge::Accept(retro_hw_render_callback* callback) {
    accepted_ = callback && callback->context_type == RETRO_HW_CONTEXT_D3D12;
    if (accepted_) {
        contextReset_ = callback->context_reset;
        contextDestroy_ = callback->context_destroy;
    }
    return accepted_;
}

bool VitaD3D12Bridge::Initialize(std::string& error) {
    try {
        if (!accepted_ || !contextReset_) throw std::runtime_error("the core did not ask for Direct3D 12");
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_))))
            throw std::runtime_error("D3D12CreateDevice failed");
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device_->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue_))))
            throw std::runtime_error("CreateCommandQueue failed");
        if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
            throw std::runtime_error("CreateFence failed");
        event_ = CreateEventExW(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
        if (!event_) throw std::runtime_error("CreateEventEx failed");
        for (auto& slot : slots_) {
            if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)))
                || FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr, IID_PPV_ARGS(&slot.list))))
                throw std::runtime_error("readback command list could not be made");
            slot.list->Close();
        }
        interface_.interface_type = RETRO_HW_RENDER_INTERFACE_D3D12;
        interface_.interface_version = 1;
        interface_.handle = this;
        interface_.device = device_.Get();
        interface_.queue = queue_.Get();
        // the core builds its shaders with it (HLSL from SPIR-V, shader model 5.1)
        interface_.D3DCompile = reinterpret_cast<void*>(&::D3DCompile);
        interface_.required_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        interface_.set_texture = &VitaD3D12Bridge::SetTexture;
        contextReset_();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

void VitaD3D12Bridge::SetTexture(void* handle, ID3D12Resource* texture, int format) {
    auto& self = *static_cast<VitaD3D12Bridge*>(handle);
    self.texture_ = texture;
    self.format_ = static_cast<DXGI_FORMAT>(format);
}

bool VitaD3D12Bridge::Present(unsigned width, unsigned height, std::string& error) {
    if (!device_ || !texture_ || !sink) return false;
    if (!width || !height || width > 4096 || height > 4096) { error = "Invalid core frame size"; return false; }
    const bool rgba = format_ == DXGI_FORMAT_R8G8B8A8_UNORM || format_ == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (!rgba && format_ != DXGI_FORMAT_B8G8R8A8_UNORM && format_ != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        error = "Unsupported core texture format";
        return false;
    }
    Slot& slot = slots_[index_];
    if (slot.pending && fence_->GetCompletedValue() < slot.done) {
        if (SUCCEEDED(fence_->SetEventOnCompletion(slot.done, event_))
            && WaitForSingleObjectEx(event_, 2000, FALSE) != WAIT_OBJECT_0) {
            error = "GPU did not finish a frame within 2 seconds";
            return false;
        }
    }
    slot.pending = false;

    const D3D12_RESOURCE_DESC desc = texture_->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total = 0;
    device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    if (!slot.buffer || slot.size < total) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = total;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.Format = DXGI_FORMAT_UNKNOWN;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        slot.buffer.Reset();
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.buffer)))) {
            error = "readback buffer could not be made";
            return false;
        }
        slot.size = total;
    }
    slot.allocator->Reset();
    slot.list->Reset(slot.allocator.Get(), nullptr);
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = texture_;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = slot.buffer.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = footprint;
    // the core leaves its frame in COPY_SOURCE (interface_.required_state)
    D3D12_BOX box{ 0, 0, 0, std::min<UINT>(width, static_cast<UINT>(desc.Width)), std::min<UINT>(height, desc.Height), 1 };
    slot.list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    if (FAILED(slot.list->Close())) { error = "readback commands could not be closed"; return false; }
    ID3D12CommandList* lists[] = { slot.list.Get() };
    queue_->ExecuteCommandLists(1, lists);
    slot.done = ++fenceValue_;
    queue_->Signal(fence_.Get(), slot.done);
    slot.pending = true;
    slot.rowPitch = footprint.Footprint.RowPitch;
    slot.width = box.right;
    slot.height = box.bottom;
    slot.rgba = rgba;
    slot.serial = ++serial_;
    index_ = (index_ + 1) % kSlots;
    // whatever has finished by now (usually the previous frame) is shown
    DeliverReady();
    return true;
}

void VitaD3D12Bridge::DeliverReady() {
    if (!sink) return;
    Slot* pick = nullptr;
    for (auto& slot : slots_) {
        if (!slot.pending || slot.serial <= shownSerial_) continue;
        if (fence_->GetCompletedValue() < slot.done) continue;
        if (!pick || slot.serial < pick->serial) pick = &slot;
    }
    if (!pick) return;
    void* mapped = nullptr;
    const D3D12_RANGE read{ 0, static_cast<SIZE_T>(pick->size) };
    if (FAILED(pick->buffer->Map(0, &read, &mapped))) return;
    // rows tightly packed for the presenter
    const size_t row = static_cast<size_t>(pick->width) * 4;
    packed_.resize(row * pick->height);
    for (unsigned y = 0; y < pick->height; ++y)
        std::memcpy(packed_.data() + row * y, static_cast<const uint8_t*>(mapped) + static_cast<size_t>(pick->rowPitch) * y, row);
    const D3D12_RANGE none{ 0, 0 };
    pick->buffer->Unmap(0, &none);
    sink(packed_.data(), pick->width, pick->height, pick->rgba);
    shownSerial_ = pick->serial;
    pick->pending = false;
}

void VitaD3D12Bridge::Shutdown() {
    if (fence_ && event_ && fenceValue_ && fence_->GetCompletedValue() < fenceValue_) {
        if (SUCCEEDED(fence_->SetEventOnCompletion(fenceValue_, event_)))
            WaitForSingleObjectEx(event_, 2000, FALSE);
    }
    if (device_ && contextDestroy_) contextDestroy_();
    for (auto& slot : slots_) slot = Slot{};
    texture_ = nullptr;
    fence_.Reset();
    queue_.Reset();
    device_.Reset();
    if (event_) { CloseHandle(event_); event_ = nullptr; }
    accepted_ = false;
    contextReset_ = nullptr;
    contextDestroy_ = nullptr;
    index_ = 0; serial_ = 0; shownSerial_ = 0; fenceValue_ = 0;
}
