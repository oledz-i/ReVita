// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#define VK_NO_PROTOTYPES
#include <libretro_vulkan.h>
#include <windows.h>
#include <functional>
#include <mutex>
#include <vector>
#include <string>

// Offscreen images read back to the CPU in a ring. No Win32 window or Vulkan WSI.
class VitaVulkanBridge {
public:
    using FrameSink = std::function<void(const uint8_t*, unsigned, unsigned, bool)>;
    ~VitaVulkanBridge();
    bool Accept(retro_hw_render_callback* callback);
    bool Negotiate(const retro_hw_render_context_negotiation_interface_vulkan* negotiation);
    bool Initialize(std::string& error);
    void Shutdown();
    bool Present(unsigned width, unsigned height, std::string& error);
    // What the readback uses (memory type, image format), for the log.
    const std::string& ReadbackInfo() const { return readbackInfo_; }
    const retro_hw_render_interface_vulkan* Interface() const { return device_ ? &interface_ : nullptr; }
    FrameSink sink;
    // Dozen's own messages (refused pipelines, D3D12 failures), through the
    // onyx_set_log_hook export of the Onyx Mesa build. Set before Initialize.
    static void SetDriverLog(std::function<void(int, const char*)> log);
    // Where Dozen keeps compiled D3D12 pipelines for this game (empty: none).
    // Set before Initialize; Dozen reads it when the device is made.
    static void SetPipelineCachePath(const std::wstring& path);
    // Folder Dozen keeps each compiled pipeline in (empty: none)
    static void SetPipelineBlobDir(const std::wstring& path);
private:
    template<class T> T InstanceProc(const char* name) const;
    template<class T> T DeviceProc(const char* name) const;
    HMODULE icd_ = nullptr;
    PFN_vkGetInstanceProcAddr ipa_ = nullptr;
    PFN_vkGetDeviceProcAddr dpa_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    retro_hw_render_callback callback_{};
    const retro_hw_render_context_negotiation_interface_vulkan* negotiation_ = nullptr;
    retro_hw_render_interface_vulkan interface_{};
    retro_vulkan_image image_{};
    bool haveImage_ = false;
    bool reset_ = false;
    std::mutex queueMutex_;
    std::vector<VkSemaphore> waits_;
    std::vector<VkCommandBuffer> pending_;
    VkSemaphore signal_ = VK_NULL_HANDLE;
    // Frames are read back through a ring of slots, one per sync index: a
    // frame's copy is submitted with its own fence and handed to the sink when
    // that fence has signalled, at the latest when its slot comes round again.
    // The emulation thread used to wait for the whole GPU queue to drain twice
    // a frame (vkQueueWaitIdle in wait_sync_index, then the readback fence),
    // which tied the game's vblank to the GPU's speed.
    static constexpr uint32_t kSlots = 3;
    struct Slot {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        void* mapped = nullptr;
        bool coherent = true;
        VkFence fence = VK_NULL_HANDLE;
        VkCommandBuffer command = VK_NULL_HANDLE;
        bool pending = false;   // submitted, not yet known to be done
        bool ready = false;     // done, not yet handed to the sink
        unsigned width = 0, height = 0;
        bool rgba = true;
        uint64_t serial = 0;
    };
    Slot slots_[kSlots];
    uint32_t index_ = 0;
    uint64_t serial_ = 0;
    uint64_t shownSerial_ = 0;
    uint32_t backlogTicks_ = 0;
    std::string readbackInfo_;
    bool gpuStalled_ = false;
    void ReleaseReadback();
    // Waits (bounded) for one slot's copy; false when the GPU stopped.
    bool WaitSlot(Slot& slot, std::string& error);
    // Hands the newest finished frame to the sink; never waits.
    void DeliverReady();
    static void SetImage(void*, const retro_vulkan_image*, uint32_t, const VkSemaphore*, uint32_t);
    static uint32_t SyncIndex(void* p) { return static_cast<VitaVulkanBridge*>(p)->index_; }
    static uint32_t SyncMask(void*) { return (1u << kSlots) - 1u; }
    static void Commands(void*, uint32_t, const VkCommandBuffer*);
    static void Wait(void*);
    static void Lock(void* p) { static_cast<VitaVulkanBridge*>(p)->queueMutex_.lock(); }
    static void Unlock(void* p) { static_cast<VitaVulkanBridge*>(p)->queueMutex_.unlock(); }
    static void Signal(void* p, VkSemaphore s) { static_cast<VitaVulkanBridge*>(p)->signal_ = s; }
};
