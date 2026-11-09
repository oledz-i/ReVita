// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include "VitaVulkanBridge.h"
#include <stdexcept>
#include <cstring>
#include <cstdlib>

namespace {
std::mutex g_driverLogMutex;
std::function<void(int, const char*)> g_driverLog;
unsigned g_driverLines = 0;
void __cdecl DriverLogHook(int level, const char* message) {
    std::lock_guard<std::mutex> guard(g_driverLogMutex);
    // A driver failing on every draw would otherwise fill the log.
    if (!g_driverLog || !message || ++g_driverLines > 4000) return;
    g_driverLog(level, message);
}
using SetLogHook = void(__cdecl*)(void (*)(int, const char*));

// Dozen has its own C runtime: only the Win32 environment block is shared
// with it, and it reads these once, when it loads and makes its device.
void SetDriverVariable(const wchar_t* name, const std::wstring& value) {
    SetEnvironmentVariableW(name, value.empty() ? nullptr : value.c_str());
    std::string narrowName, narrowValue;
    for (const wchar_t* c = name; *c; ++c) narrowName.push_back(static_cast<char>(*c));
    for (wchar_t c : value) narrowValue.push_back(static_cast<char>(c));
    _putenv_s(narrowName.c_str(), narrowValue.c_str());
}
void Check(VkResult result, const char* stage) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(stage) + ": VkResult " + std::to_string(result));
}
}
template<class T> T VitaVulkanBridge::InstanceProc(const char* name) const {
    auto fn = reinterpret_cast<T>(ipa_(instance_, name));
    if (!fn) throw std::runtime_error(std::string("Missing Vulkan instance entry: ") + name);
    return fn;
}
template<class T> T VitaVulkanBridge::DeviceProc(const char* name) const {
    auto fn = reinterpret_cast<T>(dpa_(device_, name));
    if (!fn) throw std::runtime_error(std::string("Missing Vulkan device entry: ") + name);
    return fn;
}
VitaVulkanBridge::~VitaVulkanBridge() { Shutdown(); }
void VitaVulkanBridge::SetDriverLog(std::function<void(int, const char*)> log) {
    std::lock_guard<std::mutex> guard(g_driverLogMutex);
    g_driverLog = std::move(log);
    g_driverLines = 0;
}
void VitaVulkanBridge::SetPipelineCachePath(const std::wstring& path) {
    SetDriverVariable(L"ONYX_PSO_CACHE", path);
}
void VitaVulkanBridge::SetPipelineBlobDir(const std::wstring& path) {
    SetDriverVariable(L"VITA3K_PSO_DIR", path);
}
bool VitaVulkanBridge::Accept(retro_hw_render_callback* cb) {
    if (!cb || cb->context_type != RETRO_HW_CONTEXT_VULKAN) return false;
    callback_ = *cb;
    return true;
}
bool VitaVulkanBridge::Negotiate(const retro_hw_render_context_negotiation_interface_vulkan* n) {
    if (!n || n->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN || !n->create_device) return false;
    negotiation_ = n;
    return true;
}
bool VitaVulkanBridge::Initialize(std::string& error) {
    if (device_) return true;
    try {
        if (!negotiation_ || !callback_.context_reset) throw std::runtime_error("Core did not supply Vulkan context negotiation");
        // The Xbox's DXIL validator refuses Dozen's shaders for many games;
        // unsigned, every pipeline built from them is refused by D3D12 and
        // Dozen skips its draws (Onyx patches 0008, 0016, 0018): the game
        // runs behind a black screen. This makes Dozen sign them itself, as
        // OnyxEMU does. Set before the DLL loads; it reads it once.
        SetDriverVariable(L"ONYX_DXIL_SELFSIGN", L"1");
        icd_ = LoadPackagedLibrary(L"vulkan_dzn.dll", 0);
        if (!icd_) throw std::runtime_error("Packaged Dozen ICD could not load: " + std::to_string(GetLastError()));
        if (auto setHook = reinterpret_cast<SetLogHook>(GetProcAddress(icd_, "onyx_set_log_hook")))
            setHook(&DriverLogHook); // Dozen answers with a "log hook installed" line
        else
            DriverLogHook(1, "Dozen has no onyx_set_log_hook export: its errors are not logged");
        using NegotiateIcd = VkResult(VKAPI_PTR*)(uint32_t*);
        auto negotiate = reinterpret_cast<NegotiateIcd>(GetProcAddress(icd_, "vk_icdNegotiateLoaderICDInterfaceVersion"));
        if (negotiate) { uint32_t version = 5; Check(negotiate(&version), "ICD negotiation"); }
        ipa_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(icd_, "vk_icdGetInstanceProcAddr"));
        if (!ipa_) throw std::runtime_error("Dozen ICD instance dispatch is missing");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Vita3K UWP"; app.apiVersion = VK_API_VERSION_1_1;
        const auto* info = negotiation_->get_application_info ? negotiation_->get_application_info() : &app;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo = info;
        Check(InstanceProc<PFN_vkCreateInstance>("vkCreateInstance")(&create, nullptr, &instance_), "Create instance");
        auto enumerate = InstanceProc<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        uint32_t count = 0; Check(enumerate(instance_, &count, nullptr), "Enumerate GPUs");
        if (!count) throw std::runtime_error("Dozen reports no Vulkan GPU");
        std::vector<VkPhysicalDevice> devices(count); Check(enumerate(instance_, &count, devices.data()), "Enumerate GPUs");
        retro_vulkan_context context{};
        VkPhysicalDeviceFeatures required{};
        bool made = false;
        for (auto gpu : devices) {
            context = {};
            if (negotiation_->create_device(&context, instance_, gpu, VK_NULL_HANDLE, ipa_, nullptr, 0, nullptr, 0, &required)) { made = true; break; }
        }
        if (!made || !context.device || !context.queue) throw std::runtime_error("Vita3K could not create its Vulkan device with the required features");
        gpu_ = context.gpu; device_ = context.device; queue_ = context.queue; family_ = context.queue_family_index;
        dpa_ = InstanceProc<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pool.queueFamilyIndex = family_;
        Check(DeviceProc<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device_, &pool, nullptr, &pool_), "Create readback pool");
        interface_.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
        interface_.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
        interface_.handle = this; interface_.instance = instance_; interface_.gpu = gpu_; interface_.device = device_;
        interface_.get_device_proc_addr = dpa_; interface_.get_instance_proc_addr = ipa_;
        interface_.queue = queue_; interface_.queue_index = family_;
        interface_.set_image = SetImage; interface_.get_sync_index = SyncIndex; interface_.get_sync_index_mask = SyncMask;
        interface_.set_command_buffers = Commands; interface_.wait_sync_index = Wait;
        interface_.lock_queue = Lock; interface_.unlock_queue = Unlock; interface_.set_signal_semaphore = Signal;
        reset_ = true;
        callback_.context_reset();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        // The caller unloads the core before Shutdown. A partially initialized
        // renderer can still own allocations on this device after context_reset.
        return false;
    }
}
void VitaVulkanBridge::SetImage(void* p, const retro_vulkan_image* image, uint32_t count, const VkSemaphore* waits, uint32_t family) {
    auto& self = *static_cast<VitaVulkanBridge*>(p);
    // The pinned Vita3K core submits on our graphics queue and passes IGNORED.
    // Reject unsupported ownership transfers rather than reading an unowned image.
    self.haveImage_ = image && (family == VK_QUEUE_FAMILY_IGNORED || family == self.family_);
    if (!self.haveImage_) return;
    self.image_ = *image;
    self.waits_.clear();
    if (count && waits) self.waits_.assign(waits, waits + count);
}
void VitaVulkanBridge::Commands(void* p, uint32_t n, const VkCommandBuffer* commands) {
    auto& self = *static_cast<VitaVulkanBridge*>(p);
    self.pending_.clear(); if (n && commands) self.pending_.assign(commands, commands + n);
}
void VitaVulkanBridge::Wait(void* p) {
    // The core is about to reuse this sync index's image: only the copy made
    // from it last time round has to be done, not the whole queue.
    // The slots are the emulation thread's alone; the queue lock (which the
    // core's render thread takes to submit) is not held while waiting.
    auto& self = *static_cast<VitaVulkanBridge*>(p);
    if (!self.device_) return;
    std::string error;
    // Not shown yet and about to be overwritten: it goes out now (rare: only
    // when the GPU fell three frames behind).
    Slot& slot = self.slots_[self.index_];
    self.WaitSlot(slot, error);
    if (slot.ready && slot.serial > self.shownSerial_) self.DeliverReady();
}
bool VitaVulkanBridge::WaitSlot(Slot& slot, std::string& error) {
    if (!slot.pending) return !gpuStalled_;
    if (gpuStalled_) return false;
    // Never wait forever: a hung GPU frame becomes a logged error, not a frozen app.
    const VkResult waited = DeviceProc<PFN_vkWaitForFences>("vkWaitForFences")(device_, 1, &slot.fence, VK_TRUE, 2000000000ull);
    if (waited == VK_TIMEOUT) {
        gpuStalled_ = true; // its command buffer may still be in use: leave it
        error = "GPU did not finish a frame within 2 seconds";
        return false;
    }
    if (waited != VK_SUCCESS) {
        error = "Wait for readback: VkResult " + std::to_string(waited);
        return false;
    }
    slot.pending = false;
    slot.ready = true;
    return true;
}
void VitaVulkanBridge::DeliverReady() {
    // One frame a tick, in the order they were made, a tick behind. Showing
    // whichever copy had finished most recently (as before) skipped a frame
    // whenever the GPU was a little late with one, then showed nothing new
    // the tick after: a 30 fps game (two ticks a frame) came out as 1-3
    // instead of 2-2, the judder seen in Ultra Despair Girls' cutscenes.
    if (!device_ || !sink) return;
    auto status = DeviceProc<PFN_vkGetFenceStatus>("vkGetFenceStatus");
    Slot* order[kSlots] = {};
    uint32_t unshown = 0;
    for (auto& slot : slots_) {
        if (slot.pending && status(device_, slot.fence) == VK_SUCCESS) {
            slot.pending = false;
            slot.ready = true;
        }
        if ((slot.pending || slot.ready) && slot.serial > shownSerial_) order[unshown++] = &slot;
    }
    if (unshown == 0) return;
    std::sort(order, order + unshown, [](const Slot* x, const Slot* y) { return x->serial < y->serial; });
    Slot* pick = order[0];
    if (pick->pending) {
        // Usually it is a millisecond or two from done: worth a short wait to
        // keep the cadence, never a long one.
        const VkResult waited = DeviceProc<PFN_vkWaitForFences>("vkWaitForFences")(device_, 1, &pick->fence, VK_TRUE, 4000000ull);
        if (waited != VK_SUCCESS) { backlogTicks_ = 0; return; }
        pick->pending = false;
        pick->ready = true;
    }
    // Two or more finished and waiting for a while: a hitch left the picture
    // a tick further behind than it needs to be. Catch up once.
    uint32_t finished = 0;
    for (uint32_t i = 0; i < unshown; ++i)
        if (order[i]->ready || (order[i]->pending && status(device_, order[i]->fence) == VK_SUCCESS)) ++finished;
    backlogTicks_ = finished >= 2 ? backlogTicks_ + 1 : 0;
    if (backlogTicks_ > 30 && unshown >= 2) {
        Slot* next = order[1];
        if (next->pending && status(device_, next->fence) == VK_SUCCESS) { next->pending = false; next->ready = true; }
        if (next->ready) { pick = next; backlogTicks_ = 0; }
    }
    if (!pick->coherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory = pick->memory; range.size = VK_WHOLE_SIZE;
        DeviceProc<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(device_, 1, &range);
    }
    sink(static_cast<const uint8_t*>(pick->mapped), pick->width, pick->height, pick->rgba);
    shownSerial_ = pick->serial;
    for (auto& slot : slots_)
        if (slot.ready && slot.serial <= shownSerial_) slot.ready = false;
}
bool VitaVulkanBridge::Present(unsigned width, unsigned height, std::string& error) {
    if (!device_ || !haveImage_ || !sink) return false;
    if (!width || !height || width > 4096 || height > 4096) { error = "Invalid core frame size"; return false; }
    const auto format = image_.create_info.format;
    const bool rgba = format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB;
    if (!rgba && format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_B8G8R8A8_SRGB) { error = "Unsupported core image format"; return false; }
    if (gpuStalled_) { error = "GPU stopped responding earlier; restart the game"; return false; }
    Slot& slot = slots_[index_];
    // The core normally waited for this slot already (wait_sync_index).
    if (slot.pending && !WaitSlot(slot, error)) {
        waits_.clear(); pending_.clear(); signal_ = VK_NULL_HANDLE; haveImage_ = false;
        return false;
    }
    std::unique_lock<std::mutex> lock(queueMutex_);
    bool submitted = false;
    try {
#define FN(name) DeviceProc<PFN_##name>(#name)
        const VkDeviceSize size = VkDeviceSize(width) * height * 4;
        if (size > slot.size) {
            if (slot.mapped) FN(vkUnmapMemory)(device_, slot.memory);
            if (slot.buffer) FN(vkDestroyBuffer)(device_, slot.buffer, nullptr);
            if (slot.memory) FN(vkFreeMemory)(device_, slot.memory, nullptr);
            slot.mapped = nullptr; slot.buffer = VK_NULL_HANDLE; slot.memory = VK_NULL_HANDLE; slot.size = 0; slot.ready = false;
            VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bc.size = size; bc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            Check(FN(vkCreateBuffer)(device_, &bc, nullptr, &slot.buffer), "Create readback buffer");
            VkMemoryRequirements requirements{}; FN(vkGetBufferMemoryRequirements)(device_, slot.buffer, &requirements);
            VkPhysicalDeviceMemoryProperties properties{};
            InstanceProc<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(gpu_, &properties);
            // GPU-to-CPU copies belong in cached host memory (D3D12's readback
            // heap). The first host-visible type Dozen lists is write-combined
            // upload memory, which the GPU is not meant to copy into and the
            // CPU reads very slowly: frames read from it can come back black.
            uint32_t type = properties.memoryTypeCount;
            const VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            const VkMemoryPropertyFlags cached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            const VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            const VkMemoryPropertyFlags preferences[] = { visible | cached | coherent, visible | cached, visible | coherent, visible };
            for (const auto wanted : preferences) {
                for (uint32_t i = 0; i < properties.memoryTypeCount && type == properties.memoryTypeCount; ++i)
                    if ((requirements.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted) type = i;
                if (type != properties.memoryTypeCount) break;
            }
            if (type == properties.memoryTypeCount) throw std::runtime_error("No host-visible readback memory");
            slot.coherent = (properties.memoryTypes[type].propertyFlags & coherent) != 0;
            const auto flags = properties.memoryTypes[type].propertyFlags;
            readbackInfo_ = "readback memory type " + std::to_string(type) + " of " + std::to_string(properties.memoryTypeCount) +
                ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? ", device-local" : "") + ((flags & cached) ? ", cached" : ", uncached") +
                ((flags & coherent) ? ", coherent" : ", not coherent") + "; image format " + std::to_string(format) +
                ", layout " + std::to_string(image_.image_layout) + "; " + std::to_string(kSlots) + " frames in flight";
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = type;
            Check(FN(vkAllocateMemory)(device_, &allocate, nullptr, &slot.memory), "Allocate readback memory");
            Check(FN(vkBindBufferMemory)(device_, slot.buffer, slot.memory, 0), "Bind readback buffer");
            Check(FN(vkMapMemory)(device_, slot.memory, 0, VK_WHOLE_SIZE, 0, &slot.mapped), "Map readback");
            slot.size = size;
        }
        if (!slot.fence) {
            VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            Check(FN(vkCreateFence)(device_, &fc, nullptr, &slot.fence), "Create readback fence");
        }
        Check(FN(vkResetFences)(device_, 1, &slot.fence), "Reset readback fence");
        if (!slot.command) {
            VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool_; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
            Check(FN(vkAllocateCommandBuffers)(device_, &ca, &slot.command), "Allocate readback command");
        }
        VkCommandBuffer command = slot.command;
        Check(FN(vkResetCommandBuffer)(command, 0), "Reset readback command");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(FN(vkBeginCommandBuffer)(command, &begin), "Begin readback");
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.image = image_.create_info.image; barrier.oldLayout = image_.image_layout;
        barrier.newLayout = barrier.oldLayout == VK_IMAGE_LAYOUT_GENERAL ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = image_.create_info.subresourceRange;
        barrier.subresourceRange.levelCount = 1; barrier.subresourceRange.layerCount = 1;
        FN(vkCmdPipelineBarrier)(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, barrier.subresourceRange.baseMipLevel, barrier.subresourceRange.baseArrayLayer, 1}; copy.imageExtent = {width, height, 1};
        FN(vkCmdCopyImageToBuffer)(command, barrier.image, barrier.newLayout, slot.buffer, 1, &copy);
        // Back to the core's layout; later work on the queue (the core's next
        // frame drawing into this image) waits for the copy's reads here.
        auto layout = barrier.oldLayout; barrier.oldLayout = barrier.newLayout; barrier.newLayout = layout;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT; host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; host.buffer = slot.buffer; host.size = VK_WHOLE_SIZE;
        FN(vkCmdPipelineBarrier)(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 1, &barrier);
        Check(FN(vkEndCommandBuffer)(command), "End readback");
        pending_.push_back(command);
        std::vector<VkPipelineStageFlags> stages(waits_.size(), VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.waitSemaphoreCount = static_cast<uint32_t>(waits_.size()); submit.pWaitSemaphores = waits_.data(); submit.pWaitDstStageMask = stages.data(); submit.commandBufferCount = static_cast<uint32_t>(pending_.size()); submit.pCommandBuffers = pending_.data();
        if (signal_) { submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &signal_; }
        Check(FN(vkQueueSubmit)(queue_, 1, &submit, slot.fence), "Submit readback"); submitted = true;
        slot.pending = true;
        slot.ready = false;
        slot.width = width; slot.height = height; slot.rgba = rgba;
        slot.serial = ++serial_;
        waits_.clear(); pending_.clear(); signal_ = VK_NULL_HANDLE; haveImage_ = false;
        index_ = (index_ + 1) % kSlots;
        lock.unlock();
        // Whatever has finished by now (usually the previous frame) is shown;
        // nothing here waits for the GPU, nor holds the queue.
        DeliverReady();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        if (submitted) {
            slot.pending = true;
            std::string ignored;
            WaitSlot(slot, ignored);
        }
        pending_.clear(); waits_.clear(); signal_ = VK_NULL_HANDLE; haveImage_ = false;
        return false;
    }
#undef FN
}
void VitaVulkanBridge::ReleaseReadback() {
    if (!device_) return;
    for (auto& slot : slots_) {
        if (slot.mapped) DeviceProc<PFN_vkUnmapMemory>("vkUnmapMemory")(device_, slot.memory);
        if (slot.buffer) DeviceProc<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device_, slot.buffer, nullptr);
        if (slot.memory) DeviceProc<PFN_vkFreeMemory>("vkFreeMemory")(device_, slot.memory, nullptr);
        if (slot.fence) DeviceProc<PFN_vkDestroyFence>("vkDestroyFence")(device_, slot.fence, nullptr);
        if (slot.command && pool_) DeviceProc<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(device_, pool_, 1, &slot.command);
        slot = Slot{};
    }
}
void VitaVulkanBridge::Shutdown() {
    try {
        // After a stalled frame, waiting for the device could hang the app.
        if (device_ && !gpuStalled_) DeviceProc<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(device_);
        if (reset_ && callback_.context_destroy) callback_.context_destroy();
        reset_ = false;
        if (!gpuStalled_) ReleaseReadback();
        gpuStalled_ = false;
        if (device_ && pool_) DeviceProc<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device_, pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
        if (device_ && negotiation_ && negotiation_->destroy_device) negotiation_->destroy_device();
        if (device_) DeviceProc<PFN_vkDestroyDevice>("vkDestroyDevice")(device_, nullptr);
        device_ = VK_NULL_HANDLE;
        if (instance_) InstanceProc<PFN_vkDestroyInstance>("vkDestroyInstance")(instance_, nullptr);
    } catch (...) {}
    for (auto& slot : slots_) slot = Slot{};
    index_ = 0; serial_ = 0; shownSerial_ = 0;
    device_ = VK_NULL_HANDLE; instance_ = VK_NULL_HANDLE; queue_ = VK_NULL_HANDLE; haveImage_ = false;
    negotiation_ = nullptr; callback_ = {}; waits_.clear(); pending_.clear(); signal_ = VK_NULL_HANDLE;
    if (icd_) {
        // Unhook first: the driver must not call into the log while it unloads.
        if (auto setHook = reinterpret_cast<SetLogHook>(GetProcAddress(icd_, "onyx_set_log_hook"))) setHook(nullptr);
        FreeLibrary(icd_);
    }
    icd_ = nullptr; ipa_ = nullptr; dpa_ = nullptr;
}
