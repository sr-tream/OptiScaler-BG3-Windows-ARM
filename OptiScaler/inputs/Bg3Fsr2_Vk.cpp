#include "pch.h"
#include "Bg3Fsr2_Vk.h"
#include "Bg3Fsr2_Bridge.h"
#include "Bg3VulkanCommands.h"
#include <hooks/Vulkan_Hooks.h>
#include <Config.h>
#include <hooks/HookBackend.h>
#include <proxies/KernelBase_Proxy.h>
#include <map>

namespace Bg3Fsr2
{
namespace
{
constexpr size_t MaxTrackedSets = 65536;
std::once_flag initOnce;
std::once_flag vulkanOnce;
bool supportedExecutable = false;
std::atomic<bool> enabled = false;
std::atomic<unsigned> fallbackWarnings = 0;
std::atomic<uint64_t> bindOrder = 0;
std::mutex mutex;
thread_local bool injecting = false;

#define BG3_VK_FUNCTIONS(X) \
    X(EnumeratePhysicalDevices) X(DestroyDevice) X(BeginCommandBuffer) X(EndCommandBuffer) X(AllocateCommandBuffers) X(FreeCommandBuffers) X(ResetCommandBuffer) X(ResetCommandPool) X(DestroyCommandPool) X(QueueSubmit) X(QueueSubmit2) \
    X(CreateImage) X(DestroyImage) X(CreateImageView) X(DestroyImageView) \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreateComputePipelines) X(DestroyPipeline) \
    X(UpdateDescriptorSets) X(CreateDescriptorUpdateTemplate) X(DestroyDescriptorUpdateTemplate) X(UpdateDescriptorSetWithTemplate) \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(AllocateDescriptorSets) X(FreeDescriptorSets) X(ResetDescriptorPool) X(DestroyDescriptorPool) \
    X(CmdBindDescriptorSets) X(CmdBindPipeline) X(CmdPushConstants) X(CmdDispatch) X(CmdDispatchIndirect) X(CmdDispatchBase) X(CmdExecuteCommands) \
    X(BindBufferMemory) X(BindBufferMemory2) X(AllocateMemory) X(MapMemory) X(UnmapMemory) X(FreeMemory)
#define DECLARE_ORIGINAL(name) PFN_vk##name original##name = nullptr;
BG3_VK_FUNCTIONS(DECLARE_ORIGINAL)
#undef DECLARE_ORIGINAL

struct View { VkImageViewCreateInfo info; };
struct Shader { uint64_t hash = 0; bool fsrConstants = false; };
struct Pipeline { VkPipelineLayout layout; Shader shader; };
struct Descriptor
{
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    VkDescriptorImageInfo image {};
    VkDescriptorBufferInfo buffer {};
};
struct BufferMemory { VkDeviceMemory memory; VkDeviceSize offset; };
struct Mapping { VkDeviceSize offset, size; const uint8_t* pointer; };
struct DescriptorBind
{
    VkPipelineLayout layout;
    uint32_t firstSet;
    std::vector<VkDescriptorSet> descriptors;
    std::vector<uint32_t> dynamicOffsets;
    uint64_t order = 0;
};
std::unordered_map<VkImage, VkImageCreateInfo> images;
std::unordered_map<VkImageView, View> views;
std::unordered_map<VkShaderModule, Shader> shaders;
std::unordered_map<VkPipeline, Pipeline> pipelines;
std::unordered_map<VkDescriptorSet, std::map<uint64_t, Descriptor>> sets;
std::unordered_map<VkCommandBuffer, VkPipeline> boundPipelines;
std::unordered_map<VkCommandBuffer, std::map<uint32_t, DescriptorBind>> boundDescriptors;
std::unordered_map<VkBuffer, BufferMemory> bufferMemory;
std::unordered_map<VkDeviceMemory, Mapping> mappings;
std::unordered_map<VkDescriptorUpdateTemplate, std::vector<VkDescriptorUpdateTemplateEntry>> updateTemplates;
std::unordered_map<VkImage, VkDevice> imageDevices;
std::unordered_map<VkPhysicalDevice, VkInstance> physicalInstances;
struct DeviceInfo { VkPhysicalDevice physical; VkInstance instance; };
std::unordered_map<VkDevice, DeviceInfo> devices;
std::unordered_map<VkCommandBuffer, Bg3Fsr2Bridge::Frame> nativeFrames;
struct Push { VkPipelineLayout layout; VkShaderStageFlags stages; uint32_t offset; std::vector<uint8_t> bytes; };
std::unordered_map<VkCommandBuffer,std::vector<Push>> nativePushes;
std::unordered_set<VkCommandBuffer> pushOverflow;
std::unordered_map<VkDescriptorSetLayout,std::vector<uint64_t>> dynamicLayoutBindings;
struct SetMetadata { VkDescriptorPool pool; std::vector<uint64_t> dynamicBindings; };
std::unordered_map<VkDescriptorSet,SetMetadata> setMetadata;
std::unordered_map<VkCommandBuffer,VkCommandPool> commandPools;
struct Allocation {VkDevice device;VkMemoryPropertyFlags flags;};
std::unordered_map<VkDevice,VkPhysicalDeviceMemoryProperties> memoryProperties;
std::unordered_map<VkDeviceMemory,Allocation> allocations;
struct UniformRef {VkDevice device=VK_NULL_HANDLE;uint8_t* data=nullptr;bool coherent=false;};
std::unordered_map<VkCommandBuffer,UniformRef> nativeUniforms;
struct NativeSequence
{
    Bg3VulkanCommands::Tape tape;
    UniformRef uniform;
    unsigned skipped=0;
};
std::unordered_map<VkCommandBuffer,std::unique_ptr<NativeSequence>> nativeSequences;
std::unordered_set<VkCommandBuffer> nativeAborted;
std::unordered_set<VkDevice> dirtyNativeHistory;
std::atomic<unsigned> activeSequences=0;

void FlushNative(VkCommandBuffer cmd);
bool NativeRecording(VkCommandBuffer cmd);
bool RecordNative(VkCommandBuffer cmd,Bg3VulkanCommands::Kind kind,std::function<void()> call);

bool Collecting() { return !injecting && enabled.load(std::memory_order_relaxed) &&
    Config::Instance()->ReplaceBg3Fsr2.value_or_default(); }

bool KnownShader(uint64_t hash)
{
    switch (hash)
    {
    case 0x627D9FA372DDA76Bull: // Accumulate
    case 0x17C9AF34573E4CF2ull: // Luminance
    case 0xF06D72FAB56852B1ull: // DepthClip
    case 0xF9D14ABA0D02A13Eull: // Lock
    case 0x3D165D7D80378418ull: // RCAS
    case 0x118A1A24F8E0C4DEull: // Reconstruct
    case 0x4D0CAE3C0C1B3D84ull: // TcrAutogen
        return true;
    default: return false;
    }
}

bool CopyMemorySafe(void* destination, const void* source, size_t size)
{
    __try { memcpy(destination, source, size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

Shader Reflect(const VkShaderModuleCreateInfo& info)
{
    Shader result;
    if (info.codeSize < 20 || !info.pCode || info.pCode[0] != 0x07230203)
        return result;
    result.hash = 14695981039346656037ull;
    auto bytes = reinterpret_cast<const uint8_t*>(info.pCode);
    for (size_t i = 0; i < info.codeSize; ++i)
        result.hash = (result.hash ^ bytes[i]) * 1099511628211ull;
    // Only the seven verified BG3 shaders may enter the native replacement path.
    result.fsrConstants = KnownShader(result.hash);
    return result;
}

void Initialize()
{
    if (!Config::Instance()->ReplaceBg3Fsr2.value_or_default())
        return;
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    wchar_t path[MAX_PATH] {};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH) || _wcsicmp(std::filesystem::path(path).filename().c_str(), L"bg3.exe"))
        return;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt->FileHeader.TimeDateStamp != 0x6ab131a2 ||
        nt->OptionalHeader.SizeOfImage != 0x6833000)
    {
        LOG_ERROR("BG3FSR2 unsupported executable fingerprint; native hook disabled");
        return;
    }
    constexpr uint8_t prologue[] = { 0x48,0x8b,0xc4,0x48,0x89,0x58,0x10,0x48,0x89,0x70,0x18,0x48,0x89,0x78,0x20,
                                    0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xa8,0xb8,0xf4,0xff,0xff,0x48 };
    constexpr size_t renderRva = 0x3eebc60;
    if (memcmp(base + renderRva, prologue, sizeof(prologue)) ||
        *reinterpret_cast<uint8_t**>(base + 0x5b5e458) != base + renderRva ||
        strcmp(reinterpret_cast<const char*>(base + 0x5b5dc68), "FSR2_TcrAutogenPass.bshd"))
    {
        LOG_ERROR("BG3FSR2 code/vtable/shader signature mismatch; native hook disabled");
        return;
    }
    supportedExecutable = true;
    LOG_INFO("BG3FSR native Vulkan executable signatures verified");
}

VKAPI_ATTR VkResult VKAPI_CALL HookEnumeratePhysicalDevices(VkInstance instance, uint32_t* count, VkPhysicalDevice* output)
{
    auto result=originalEnumeratePhysicalDevices(instance,count,output);
    if (Collecting() && output && (result==VK_SUCCESS || result==VK_INCOMPLETE))
    {
        std::scoped_lock lock(mutex);
        for (uint32_t i=0;i<*count;++i) physicalInstances[output[i]]=instance;
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL HookDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator)
{

    const bool previous=injecting; injecting=true;
    Bg3Fsr2Bridge::DestroyDevice(device);
    injecting=previous;
    { std::scoped_lock lock(mutex); devices.erase(device); }
    originalDestroyDevice(device,allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL HookBeginCommandBuffer(VkCommandBuffer cmd, const VkCommandBufferBeginInfo* info)
{
    auto result=originalBeginCommandBuffer(cmd,info);
    if (result==VK_SUCCESS && Collecting())
    {

        const bool previous=injecting;injecting=true;Bg3Fsr2Bridge::ResetRecording(cmd);injecting=previous;
        std::scoped_lock lock(mutex);
        boundPipelines.erase(cmd); boundDescriptors.erase(cmd); nativeFrames.erase(cmd);
        if (nativeSequences.erase(cmd)) --activeSequences;
        nativeUniforms.erase(cmd);nativeAborted.erase(cmd);
        nativePushes.erase(cmd); pushOverflow.erase(cmd);
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL HookEndCommandBuffer(VkCommandBuffer cmd)
{
    FlushNative(cmd);
    return originalEndCommandBuffer(cmd);
}
void ForgetCommand(VkCommandBuffer cmd)
{

    const bool previous=injecting;injecting=true;Bg3Fsr2Bridge::ResetRecording(cmd);injecting=previous;
    std::scoped_lock lock(mutex);
    boundPipelines.erase(cmd);boundDescriptors.erase(cmd);nativeFrames.erase(cmd);nativePushes.erase(cmd);pushOverflow.erase(cmd);
    if (nativeSequences.erase(cmd)) --activeSequences;
    nativeUniforms.erase(cmd);nativeAborted.erase(cmd);
}
VKAPI_ATTR VkResult VKAPI_CALL HookAllocateCommandBuffers(VkDevice device,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* output)
{
    auto result=originalAllocateCommandBuffers(device,info,output);
    if (result==VK_SUCCESS && Collecting())
    {
        for (uint32_t i=0;i<info->commandBufferCount;++i) ForgetCommand(output[i]);
        std::scoped_lock lock(mutex);
        for (uint32_t i=0;i<info->commandBufferCount;++i) commandPools[output[i]]=info->commandPool;
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL HookFreeCommandBuffers(VkDevice device,VkCommandPool pool,uint32_t count,const VkCommandBuffer* commands)
{
    if (Collecting())
    {
        for (uint32_t i=0;i<count;++i) ForgetCommand(commands[i]);
        std::scoped_lock lock(mutex);for (uint32_t i=0;i<count;++i) commandPools.erase(commands[i]);
    }
    originalFreeCommandBuffers(device,pool,count,commands);
}
VKAPI_ATTR VkResult VKAPI_CALL HookResetCommandBuffer(VkCommandBuffer cmd,VkCommandBufferResetFlags flags)
{
    auto result=originalResetCommandBuffer(cmd,flags);
    if (result==VK_SUCCESS && Collecting()) ForgetCommand(cmd);
    return result;
}
void ForgetCommandPool(VkCommandPool pool,bool destroy)
{
    std::vector<VkCommandBuffer> commands;
    {std::scoped_lock lock(mutex);for (auto it=commandPools.begin();it!=commandPools.end();)
        if (it->second==pool) {commands.push_back(it->first);if (destroy) it=commandPools.erase(it);else ++it;} else ++it;}
    for (auto cmd:commands) ForgetCommand(cmd);
}
VKAPI_ATTR VkResult VKAPI_CALL HookResetCommandPool(VkDevice device,VkCommandPool pool,VkCommandPoolResetFlags flags)
{
    auto result=originalResetCommandPool(device,pool,flags);
    if (result==VK_SUCCESS && Collecting()) ForgetCommandPool(pool,false);
    return result;
}
VKAPI_ATTR void VKAPI_CALL HookDestroyCommandPool(VkDevice device,VkCommandPool pool,const VkAllocationCallbacks* allocator)
{
    if (Collecting()) ForgetCommandPool(pool,true);
    originalDestroyCommandPool(device,pool,allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL HookQueueSubmit(VkQueue queue,uint32_t count,const VkSubmitInfo* submits,VkFence fence)
{
    auto result=originalQueueSubmit(queue,count,submits,fence);
    if (result==VK_SUCCESS && enabled && !injecting)
    {
        std::vector<VkCommandBuffer> commands;
        for (uint32_t i=0;submits && i<count;++i)
            if (submits[i].commandBufferCount) commands.insert(commands.end(),submits[i].pCommandBuffers,submits[i].pCommandBuffers+submits[i].commandBufferCount);
        if (!commands.empty())
        {const bool previous=injecting;injecting=true;Bg3Fsr2Bridge::Submitted(queue,commands,originalQueueSubmit);injecting=previous;}
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL HookCmdExecuteCommands(VkCommandBuffer primary,uint32_t count,const VkCommandBuffer* secondary)
{
    if (Collecting()) Bg3Fsr2Bridge::ExecuteCommands(primary,count,secondary);
    originalCmdExecuteCommands(primary,count,secondary);
}
VKAPI_ATTR VkResult VKAPI_CALL HookQueueSubmit2(VkQueue queue,uint32_t count,const VkSubmitInfo2* submits,VkFence fence)
{
    auto result=originalQueueSubmit2(queue,count,submits,fence);
    if (result==VK_SUCCESS && enabled && !injecting)
    {
        std::vector<VkCommandBuffer> commands;
        for (uint32_t i=0;submits && i<count;++i)
            for (uint32_t j=0;j<submits[i].commandBufferInfoCount;++j) commands.push_back(submits[i].pCommandBufferInfos[j].commandBuffer);
        if (!commands.empty())
        {const bool previous=injecting;injecting=true;Bg3Fsr2Bridge::Submitted(queue,commands,originalQueueSubmit);injecting=previous;}
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL HookCreateImage(VkDevice device, const VkImageCreateInfo* info,
                                              const VkAllocationCallbacks* allocator, VkImage* image)
{
    auto result = originalCreateImage(device, info, allocator, image);
    if (result == VK_SUCCESS && Collecting())
    {
        std::scoped_lock lock(mutex);
        images[*image] = *info;
        images[*image].pNext = nullptr;
        images[*image].pQueueFamilyIndices = nullptr;
        imageDevices[*image] = device;
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL HookDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* allocator)
{
    if (Collecting()) { std::scoped_lock lock(mutex); images.erase(image); imageDevices.erase(image); }
    originalDestroyImage(device, image, allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL HookCreateImageView(VkDevice device, const VkImageViewCreateInfo* info,
                                                  const VkAllocationCallbacks* allocator, VkImageView* view)
{
    auto result = originalCreateImageView(device, info, allocator, view);
    if (result == VK_SUCCESS && Collecting())
    {
        std::scoped_lock lock(mutex);
        views[*view] = { *info };
        views[*view].info.pNext = nullptr;
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL HookDestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks* allocator)
{
    if (Collecting()) { std::scoped_lock lock(mutex); views.erase(view); }
    originalDestroyImageView(device, view, allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL HookCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* info,
                                                     const VkAllocationCallbacks* allocator, VkShaderModule* shader)
{
    auto result = originalCreateShaderModule(device, info, allocator, shader);
    if (result == VK_SUCCESS && Collecting())
    {
        auto reflection = Reflect(*info);
        std::scoped_lock lock(mutex);
        shaders[*shader] = std::move(reflection);
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL HookDestroyShaderModule(VkDevice device, VkShaderModule shader, const VkAllocationCallbacks* allocator)
{
    if (Collecting()) { std::scoped_lock lock(mutex); shaders.erase(shader); }
    originalDestroyShaderModule(device, shader, allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL HookCreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks* allocator, VkPipeline* output)
{
    auto result = originalCreateComputePipelines(device, cache, count, infos, allocator, output);
    if (Collecting())
    {
        std::scoped_lock lock(mutex);
        for (uint32_t i = 0; i < count; ++i)
            if (output[i] != VK_NULL_HANDLE)
            {
                pipelines[output[i]] = { infos[i].layout, shaders[infos[i].stage.module] };
            }
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL HookDestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* allocator)
{
    if (Collecting()) { std::scoped_lock lock(mutex); pipelines.erase(pipeline); }
    originalDestroyPipeline(device, pipeline, allocator);
}

bool IsImage(VkDescriptorType type)
{
    return type == VK_DESCRIPTOR_TYPE_SAMPLER || type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
           type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
           type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}
bool IsBuffer(VkDescriptorType type)
{
    return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
           type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}
VKAPI_ATTR VkResult VKAPI_CALL HookCreateDescriptorSetLayout(VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* info,const VkAllocationCallbacks* allocator,VkDescriptorSetLayout* output)
{
    auto result=originalCreateDescriptorSetLayout(device,info,allocator,output);
    if (result==VK_SUCCESS && Collecting())
    {
        std::vector<uint64_t> slots;
        for (uint32_t i=0;i<info->bindingCount;++i)
            if (info->pBindings[i].descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                info->pBindings[i].descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
                for (uint32_t j=0;j<info->pBindings[i].descriptorCount;++j)
                    slots.push_back((uint64_t(info->pBindings[i].binding)<<32)|j);
        std::sort(slots.begin(),slots.end());
        std::scoped_lock lock(mutex); dynamicLayoutBindings[*output]=std::move(slots);
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL HookDestroyDescriptorSetLayout(VkDevice device,VkDescriptorSetLayout layout,const VkAllocationCallbacks* allocator)
{
    if (Collecting()) {std::scoped_lock lock(mutex); dynamicLayoutBindings.erase(layout);}
    originalDestroyDescriptorSetLayout(device,layout,allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL HookAllocateDescriptorSets(VkDevice device,const VkDescriptorSetAllocateInfo* info,VkDescriptorSet* output)
{
    auto result=originalAllocateDescriptorSets(device,info,output);
    if (result==VK_SUCCESS && Collecting())
    {
        std::scoped_lock lock(mutex);
        for (uint32_t i=0;i<info->descriptorSetCount;++i)
        {
            // Pools recycle handles between layouts. Never retain entries from an earlier allocation.
            sets.erase(output[i]); setMetadata.erase(output[i]);
            auto layout=dynamicLayoutBindings.find(info->pSetLayouts[i]);
            if (layout!=dynamicLayoutBindings.end() && setMetadata.size()<MaxTrackedSets)
                setMetadata[output[i]]={info->descriptorPool,layout->second};
        }
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL HookFreeDescriptorSets(VkDevice device,VkDescriptorPool pool,uint32_t count,const VkDescriptorSet* descriptors)
{
    auto result=originalFreeDescriptorSets(device,pool,count,descriptors);
    if (result==VK_SUCCESS && Collecting())
    {
        std::scoped_lock lock(mutex);
        for (uint32_t i=0;i<count;++i) {sets.erase(descriptors[i]); setMetadata.erase(descriptors[i]);}
    }
    return result;
}
void ForgetDescriptorPool(VkDescriptorPool pool)
{
    std::scoped_lock lock(mutex);
    for (auto it=setMetadata.begin();it!=setMetadata.end();)
        if (it->second.pool==pool) {sets.erase(it->first);it=setMetadata.erase(it);} else ++it;
}
VKAPI_ATTR VkResult VKAPI_CALL HookResetDescriptorPool(VkDevice device,VkDescriptorPool pool,VkDescriptorPoolResetFlags flags)
{
    auto result=originalResetDescriptorPool(device,pool,flags);
    if (result==VK_SUCCESS && Collecting()) ForgetDescriptorPool(pool);
    return result;
}
VKAPI_ATTR void VKAPI_CALL HookDestroyDescriptorPool(VkDevice device,VkDescriptorPool pool,const VkAllocationCallbacks* allocator)
{
    if (Collecting()) ForgetDescriptorPool(pool);
    originalDestroyDescriptorPool(device,pool,allocator);
}
void TrackWritesLocked(uint32_t count, const VkWriteDescriptorSet* writes)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto& write = writes[i];
        if (!sets.contains(write.dstSet) && sets.size() >= MaxTrackedSets) continue;
        auto& set = sets[write.dstSet];
        for (uint32_t j = 0; j < write.descriptorCount; ++j)
        {
            Descriptor descriptor;
            descriptor.type = write.descriptorType;
            if (IsImage(write.descriptorType) && write.pImageInfo) descriptor.image = write.pImageInfo[j];
            if (IsBuffer(write.descriptorType) && write.pBufferInfo) descriptor.buffer = write.pBufferInfo[j];
            set[(uint64_t(write.dstBinding) << 32) | (write.dstArrayElement + j)] = descriptor;
        }
    }
}
VKAPI_ATTR void VKAPI_CALL HookUpdateDescriptorSets(VkDevice device, uint32_t count, const VkWriteDescriptorSet* writes,
                                                   uint32_t copyCount, const VkCopyDescriptorSet* copies)
{
    originalUpdateDescriptorSets(device, count, writes, copyCount, copies);
    if (!Collecting()) return;
    std::scoped_lock lock(mutex);
    TrackWritesLocked(count, writes);
    for (uint32_t i = 0; i < copyCount; ++i)
    {
        const auto& copy = copies[i];
        auto source = sets.find(copy.srcSet);
        if (source == sets.end() || (!sets.contains(copy.dstSet) && sets.size() >= MaxTrackedSets)) continue;
        // Do not guess descriptor rollover across bindings whose layouts are unknown.
        for (uint32_t j = 0; j < copy.descriptorCount; ++j)
            if (auto found = source->second.find((uint64_t(copy.srcBinding) << 32) | (copy.srcArrayElement + j));
                found != source->second.end())
                sets[copy.dstSet][(uint64_t(copy.dstBinding) << 32) | (copy.dstArrayElement + j)] = found->second;
    }
}
VKAPI_ATTR VkResult VKAPI_CALL HookCreateDescriptorUpdateTemplate(VkDevice device,
    const VkDescriptorUpdateTemplateCreateInfo* info, const VkAllocationCallbacks* allocator,
    VkDescriptorUpdateTemplate* output)
{
    auto result = originalCreateDescriptorUpdateTemplate(device, info, allocator, output);
    if (result == VK_SUCCESS && Collecting())
    {
        std::scoped_lock lock(mutex);
        updateTemplates[*output].assign(info->pDescriptorUpdateEntries,
                                       info->pDescriptorUpdateEntries + info->descriptorUpdateEntryCount);
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL HookDestroyDescriptorUpdateTemplate(VkDevice device,
    VkDescriptorUpdateTemplate updateTemplate, const VkAllocationCallbacks* allocator)
{
    if (Collecting()) { std::scoped_lock lock(mutex); updateTemplates.erase(updateTemplate); }
    originalDestroyDescriptorUpdateTemplate(device, updateTemplate, allocator);
}
VKAPI_ATTR void VKAPI_CALL HookUpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet set,
    VkDescriptorUpdateTemplate updateTemplate, const void* data)
{
    originalUpdateDescriptorSetWithTemplate(device, set, updateTemplate, data);
    if (!Collecting()) return;
    std::scoped_lock lock(mutex);
    auto found = updateTemplates.find(updateTemplate);
    if (found == updateTemplates.end()) return;
    for (const auto& entry : found->second)
        for (uint32_t i = 0; i < entry.descriptorCount; ++i)
        {
            auto source = static_cast<const uint8_t*>(data) + entry.offset + entry.stride * i;
            VkDescriptorImageInfo image {};
            VkDescriptorBufferInfo buffer {};
            VkWriteDescriptorSet write { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = set;
            write.dstBinding = entry.dstBinding;
            write.dstArrayElement = entry.dstArrayElement + i;
            write.descriptorCount = 1;
            write.descriptorType = entry.descriptorType;
            if (IsImage(entry.descriptorType) && CopyMemorySafe(&image, source, sizeof(image)))
                write.pImageInfo = &image;
            if (IsBuffer(entry.descriptorType) && CopyMemorySafe(&buffer, source, sizeof(buffer)))
                write.pBufferInfo = &buffer;
            TrackWritesLocked(1, &write);
        }
}

VKAPI_ATTR void VKAPI_CALL HookCmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline)
{
    if (Collecting() && point == VK_PIPELINE_BIND_POINT_COMPUTE)
    {
        std::scoped_lock lock(mutex);
        boundPipelines[cmd] = pipeline;
    }
    if (!Bg3VulkanCommands::BindPipeline(cmd,originalCmdBindPipeline,point,pipeline)) originalCmdBindPipeline(cmd,point,pipeline);
}
VKAPI_ATTR void VKAPI_CALL HookCmdBindDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipelineLayout layout,
    uint32_t firstSet, uint32_t count, const VkDescriptorSet* descriptors, uint32_t dynamicCount, const uint32_t* dynamicOffsets)
{
    if (Collecting() && point == VK_PIPELINE_BIND_POINT_COMPUTE)
    {
        DescriptorBind call;
        call.layout = layout;
        call.firstSet = firstSet;
        call.order = bindOrder.fetch_add(1);
        call.descriptors.assign(descriptors, descriptors + count);
        if (dynamicCount) call.dynamicOffsets.assign(dynamicOffsets, dynamicOffsets + dynamicCount);
        std::scoped_lock lock(mutex);
        boundDescriptors[cmd][firstSet] = std::move(call);
    }
    if (!Bg3VulkanCommands::BindDescriptors(cmd,originalCmdBindDescriptorSets,point,layout,firstSet,count,descriptors,dynamicCount,dynamicOffsets))
        originalCmdBindDescriptorSets(cmd, point, layout, firstSet, count, descriptors, dynamicCount, dynamicOffsets);
}
VKAPI_ATTR void VKAPI_CALL HookCmdPushConstants(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages,
                                               uint32_t offset, uint32_t size, const void* values)
{
    if (Collecting() && Config::Instance()->ReplaceBg3Fsr2.value_or_default())
    {
        std::scoped_lock lock(mutex);
        if (nativePushes[cmd].size()<256 && size<=256)
        {
            Push push {layout,stages,offset,{}};
            push.bytes.assign((const uint8_t*)values,(const uint8_t*)values+size);
            nativePushes[cmd].push_back(std::move(push));
        }
        else pushOverflow.insert(cmd);
    }
    if (!Bg3VulkanCommands::Push(cmd,originalCmdPushConstants,layout,stages,offset,size,values)) originalCmdPushConstants(cmd,layout,stages,offset,size,values);
}
const DescriptorBind* NativeBind(VkCommandBuffer cmd)
{
    auto calls=boundDescriptors.find(cmd);
    if (calls==boundDescriptors.end()) return nullptr;
    const DescriptorBind* latest=nullptr;
    for (const auto& [index,call] : calls->second)
    {
        if (call.firstSet>1 || call.descriptors.size()<=1-call.firstSet) continue;
        if (!latest || call.order>latest->order) latest=&call;
    }
    return latest;
}
const Descriptor* NativeDescriptor(VkCommandBuffer cmd, uint32_t binding)
{
    auto call=NativeBind(cmd);
    if (!call) return nullptr;
    auto set=sets.find(call->descriptors[1-call->firstSet]);
    if (set==sets.end()) return nullptr;
    auto descriptor=set->second.find(uint64_t(binding)<<32);
    return descriptor==set->second.end() ? nullptr : &descriptor->second;
}
Bg3Fsr2Bridge::Image NativeImage(VkCommandBuffer cmd, uint32_t binding)
{
    Bg3Fsr2Bridge::Image result;
    auto descriptor=NativeDescriptor(cmd,binding);
    if (!descriptor || !IsImage(descriptor->type)) return result;
    auto view=views.find(descriptor->image.imageView);
    if (view==views.end()) return result;
    auto image=images.find(view->second.info.image);
    auto device=imageDevices.find(view->second.info.image);
    if (image==images.end() || device==imageDevices.end()) return result;
    result.device=device->second; result.image=image->first; result.view=view->first;
    result.range=view->second.info.subresourceRange; result.format=view->second.info.format;
    if (result.range.levelCount==VK_REMAINING_MIP_LEVELS)
        result.range.levelCount=image->second.mipLevels-result.range.baseMipLevel;
    if (result.range.layerCount==VK_REMAINING_ARRAY_LAYERS)
        result.range.layerCount=image->second.arrayLayers-result.range.baseArrayLayer;
    result.width=image->second.extent.width; result.height=image->second.extent.height;
    result.layout=descriptor->image.imageLayout;
    return result;
}
bool NativeBufferAddress(VkCommandBuffer cmd,uint32_t binding,size_t size,UniformRef& reference)
{
    auto descriptor=NativeDescriptor(cmd,binding);
    if (!descriptor || (descriptor->type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
        descriptor->type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) || descriptor->buffer.range<size) return false;
    VkDeviceSize dynamicOffset=0;
    if (descriptor->type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
    {
        auto call=NativeBind(cmd);
        if (!call || call->firstSet+call->descriptors.size()!=2) return false;
        auto metadata=setMetadata.find(call->descriptors[1-call->firstSet]);
        if (metadata==setMetadata.end()) return false;
        const auto& dynamicBindings=metadata->second.dynamicBindings;
        const auto dynamicCount=dynamicBindings.size();
        if (!dynamicCount || (size_t)dynamicCount>call->dynamicOffsets.size()) return false;
        // Set 1 is the last set in this bind. Its dynamic offsets form the tail,
        // independently of any global set-0 descriptors that are not in this shader.
        size_t dynamicIndex=call->dynamicOffsets.size()-dynamicCount;
        auto slot=std::find(dynamicBindings.begin(),dynamicBindings.end(),uint64_t(binding)<<32);
        if (slot==dynamicBindings.end()) return false;
        dynamicOffset=call->dynamicOffsets[dynamicIndex+(slot-dynamicBindings.begin())];
    }
    auto allocation=bufferMemory.find(descriptor->buffer.buffer);
    if (allocation==bufferMemory.end()) return false;
    auto mapping=mappings.find(allocation->second.memory);
    if (mapping==mappings.end()) return false;
    const auto start=allocation->second.offset+descriptor->buffer.offset+dynamicOffset;
    if (start<mapping->second.offset) return false;
    const auto relative=start-mapping->second.offset;
    if (mapping->second.size!=VK_WHOLE_SIZE &&
        (relative>mapping->second.size || size>mapping->second.size-relative)) return false;
    reference.data=const_cast<uint8_t*>(mapping->second.pointer+relative);
    auto allocationInfo=allocations.find(allocation->second.memory);
    if (allocationInfo!=allocations.end())
    {
        reference.device=allocationInfo->second.device;
        reference.coherent=(allocationInfo->second.flags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
    }
    return true;
}
bool NativeBuffer(VkCommandBuffer cmd,uint32_t binding,void* destination,size_t size)
{
    UniformRef reference;
    return NativeBufferAddress(cmd,binding,size,reference) && CopyMemorySafe(destination,reference.data,size);
}
bool CaptureNativeFrame(VkCommandBuffer cmd, uint64_t shader, Bg3Fsr2Bridge::Frame& snapshot)
{
    using namespace Bg3Fsr2Bridge;
    // These stages contribute no external resource or camera data to the replacement.
    // Their unused dynamic UBO bindings may be omitted by the engine after a quality change.
    const unsigned marker=shader==0x17C9AF34573E4CF2ull?2:shader==0xF9D14ABA0D02A13Eull?16:shader==0x627D9FA372DDA76Bull?32:0;
    if (marker)
    {
        if (auto frame=nativeFrames.find(cmd);frame!=nativeFrames.end()) frame->second.stages|=marker;
        return false;
    }
    std::array<uint32_t,40> constants {};
    if (!NativeBuffer(cmd,0,constants.data(),sizeof(constants)))
    {
        if (fallbackWarnings.fetch_add(1)<8)
        {
            auto descriptor=NativeDescriptor(cmd,0);
            LOG_WARN("BG3FSR3 native constants unavailable; shader:{:016X} descriptor:{} type:{} range:{}",
                shader,descriptor!=nullptr,descriptor?(int)descriptor->type:-1,descriptor?descriptor->buffer.range:0);
        }
        return false;
    }
    if (shader==0x4D0CAE3C0C1B3D84ull)
    {
        auto& frame=nativeFrames[cmd]; frame=Frame{};
        frame.commandBuffer=cmd; frame.constants=constants; frame.stages=1;
        UniformRef reference;NativeBufferAddress(cmd,0,sizeof(constants),reference);
        nativeUniforms[cmd]=reference;nativeAborted.erase(cmd);
        return false;
    }
    auto found=nativeFrames.find(cmd);
    if (found==nativeFrames.end() || found->second.constants!=constants)
    {
        if (fallbackWarnings.fetch_add(1)<8)
            LOG_WARN("BG3FSR3 frame sequence mismatch; shader:{:016X} frame:{} started:{}",shader,constants[15],found!=nativeFrames.end());
        return false;
    }
    auto& frame=found->second;
    switch(shader)
    {
    case 0x17C9AF34573E4CF2ull: frame.stages|=2; break;
    case 0x118A1A24F8E0C4DEull:
        frame.color=NativeImage(cmd,5); frame.motion=NativeImage(cmd,6); frame.depth=NativeImage(cmd,7);
        frame.stages|=4; break;
    case 0xF06D72FAB56852B1ull:
        frame.reactive=NativeImage(cmd,6); frame.composition=NativeImage(cmd,7); frame.stages|=8; break;
    case 0xF9D14ABA0D02A13Eull: frame.stages|=16; break;
    case 0x627D9FA372DDA76Bull: frame.stages|=32; break;
    case 0x3D165D7D80378418ull:
    {
        frame.output=NativeImage(cmd,2); frame.exposure=NativeImage(cmd,3); frame.stages|=64;
        std::array<float,4> rcas {};
        if (NativeBuffer(cmd,1,rcas.data(),sizeof(rcas)) && rcas[0]>0 && rcas[0]<=1)
            frame.sharpness=std::clamp(1.0f+std::log2(rcas[0])*0.5f,0.0f,1.0f);
        auto device=devices.find(frame.output.device);
        if (device==devices.end())
        {
            if (fallbackWarnings.fetch_add(1)<8)
                LOG_WARN("BG3FSR3 output device unknown; output:{:X} device:{:X} trackedDevices:{} trackedPhysical:{}",
                    (size_t)frame.output.image,(size_t)frame.output.device,devices.size(),physicalInstances.size());
            return false;
        }
        frame.physicalDevice=device->second.physical; frame.instance=device->second.instance;
        snapshot=frame; return true;
    }
    }
    return false;
}
bool NativeRecording(VkCommandBuffer cmd)
{
    if (injecting || !activeSequences.load(std::memory_order_relaxed)) return false;
    std::scoped_lock lock(mutex);return nativeSequences.contains(cmd);
}
void ResetNativeHistory(const UniformRef& uniform)
{
    if (!uniform.data || !uniform.coherent || !uniform.device) return;
    {std::scoped_lock lock(mutex);if (!dirtyNativeHistory.contains(uniform.device)) return;}
    // All seven BG3 passes share this coherent, per-frame constant allocation.
    // FrameIndex=0 is FSR2's reset convention; it discards the skipped native history.
    const uint32_t reset=0;
    if (CopyMemorySafe(uniform.data+15*sizeof(uint32_t),&reset,sizeof(reset)))
    {std::scoped_lock lock(mutex);dirtyNativeHistory.erase(uniform.device);}
}
std::unique_ptr<NativeSequence> TakeNativeLocked(VkCommandBuffer cmd)
{
    auto it=nativeSequences.find(cmd);
    if (it==nativeSequences.end()) return {};
    auto sequence=std::move(it->second);nativeSequences.erase(it);--activeSequences;return sequence;
}
void FlushNative(VkCommandBuffer cmd)
{
    std::unique_ptr<NativeSequence> sequence;
    {std::scoped_lock lock(mutex);sequence=TakeNativeLocked(cmd);if (sequence) nativeAborted.insert(cmd);}
    if (!sequence) return;
    ResetNativeHistory(sequence->uniform);
    const bool previous=injecting;injecting=true;sequence->tape.Replay();injecting=previous;
}
bool RecordNative(VkCommandBuffer cmd,Bg3VulkanCommands::Kind kind,std::function<void()> call)
{
    using Kind=Bg3VulkanCommands::Kind;
    if (kind==Kind::Unsupported || kind==Kind::Transfer || kind==Kind::Dispatch)
    {FlushNative(cmd);return false;}
    {
        std::scoped_lock lock(mutex);
        auto sequence=nativeSequences.find(cmd);
        if (sequence==nativeSequences.end()) return false;
        if (sequence->second->tape.calls.size()<256)
        {sequence->second->tape.Add(kind,std::move(call));return true;}
    }
    FlushNative(cmd);return false;
}
std::function<void()> RestoreComputeLocked(VkCommandBuffer cmd)
{
    const auto pipeline=boundPipelines[cmd];auto descriptors=boundDescriptors[cmd];auto pushes=nativePushes[cmd];
    return [=] {
        if (pipeline) originalCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        std::vector<const DescriptorBind*> order;
        for (const auto& [index,call]:descriptors) order.push_back(&call);
        std::sort(order.begin(),order.end(),[](auto a,auto b){return a->order<b->order;});
        for (auto call:order) originalCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,call->layout,call->firstSet,
            (uint32_t)call->descriptors.size(),call->descriptors.data(),(uint32_t)call->dynamicOffsets.size(),call->dynamicOffsets.data());
        for (const auto& push:pushes) originalCmdPushConstants(cmd,push.layout,push.stages,push.offset,(uint32_t)push.bytes.size(),push.bytes.data());
    };
}
unsigned RedundantStage(uint64_t shader)
{
    return shader==0x17C9AF34573E4CF2ull?2:shader==0x118A1A24F8E0C4DEull?4:shader==0xF06D72FAB56852B1ull?8:
        shader==0xF9D14ABA0D02A13Eull?16:shader==0x627D9FA372DDA76Bull?32:0;
}
bool TransactionBackend(Upscaler backend)
{
    return backend==Upscaler::FFX || backend==Upscaler::FSR21 || backend==Upscaler::FSR22;
}
bool ObserveDispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z,std::function<void()> dispatchCommand)
{
    Bg3Fsr2Bridge::Frame frame;
    std::map<uint32_t,DescriptorBind> restoreDescriptors;
    std::vector<Push> restorePushes;
    VkPipeline restorePipeline=VK_NULL_HANDLE;
    bool replace=false;
    bool suppressed=false,abort=false,rcas=false;
    const bool optimize= Config::Instance()->OptimizeBg3Fsr2.value_or_default() &&
        TransactionBackend(Config::Instance()->VulkanUpscaler.value_or_default()) &&
        (State::Instance().newBackend==Upscaler::Reset || TransactionBackend(State::Instance().newBackend)) &&
        !Config::Instance()->IsFGEnabled() && !VulkanHooks::GameWindowInactive();
    const bool skipLuminance=optimize && Config::Instance()->AutoExposure.value_or(false) &&
        State::Instance().autoExposure.value_or(true);
    std::unique_ptr<NativeSequence> sequence;
    UniformRef uniform;
    if (Collecting())
    {
        std::scoped_lock lock(mutex);
        auto pipeline = pipelines.find(boundPipelines[cmd]);
        if (pipeline != pipelines.end() && pipeline->second.shader.fsrConstants)
        {
            const auto shader=pipeline->second.shader.hash;
            rcas=shader==0x3D165D7D80378418ull;
            if (nativeSequences.contains(cmd) && !RedundantStage(shader) && !rcas) abort=true;
            if (Config::Instance()->ReplaceBg3Fsr2.value_or_default())
            {
                replace=CaptureNativeFrame(cmd,pipeline->second.shader.hash,frame);
                if (nativeAborted.contains(cmd)) replace=false;
                const unsigned stage=RedundantStage(shader);
                if (stage && optimize && (stage!=2 || skipLuminance) && !nativeAborted.contains(cmd))
                {
                    auto ref=nativeUniforms.find(cmd);
                    if ((stage==4 || stage==2) && !nativeSequences.contains(cmd) && ref!=nativeUniforms.end() &&
                        ref->second.coherent && nativeFrames[cmd].stages==(stage==2?3u:7u) && !pushOverflow.contains(cmd) && x && y && z==1)
                    {
                        auto started=std::make_unique<NativeSequence>();started->uniform=ref->second;
                        started->tape.Add(Bg3VulkanCommands::Kind::Binding,RestoreComputeLocked(cmd));
                        nativeSequences[cmd]=std::move(started);++activeSequences;
                    }
                    auto pending=nativeSequences.find(cmd);
                    if (pending!=nativeSequences.end())
                    {
                        UniformRef currentUniform;
                        // Lock's unused UBO can be absent, but the shared FSR2 constants have already been captured.
                        const bool same=stage==16 || (NativeBufferAddress(cmd,0,160,currentUniform) && currentUniform.data==pending->second->uniform.data);
                        if (same && !(pending->second->skipped&stage))
                        {
                            pending->second->skipped|=stage;
                            pending->second->tape.Add(Bg3VulkanCommands::Kind::Dispatch,std::move(dispatchCommand));
                            suppressed=true;
                        }
                        else abort=true;
                    }
                }
                if (replace)
                {
                    if (pushOverflow.contains(cmd)) replace=false;
                    else { restoreDescriptors=boundDescriptors[cmd]; restorePipeline=pipeline->first; restorePushes=nativePushes[cmd]; }
                }
            }
            if (rcas)
            {
                sequence=TakeNativeLocked(cmd);
                if (auto ref=nativeUniforms.find(cmd);ref!=nativeUniforms.end()) uniform=ref->second;
                if (sequence && sequence->skipped!=60 && sequence->skipped!=62) replace=false;
            }
        }
        else if (nativeSequences.contains(cmd)) abort=true;
    }
    if (abort) {FlushNative(cmd);suppressed=false;replace=false;}
    if (suppressed) return true;
    if (!replace)
    {
        if (sequence)
        {
            ResetNativeHistory(sequence->uniform);
            const bool previous=injecting;injecting=true;sequence->tape.Replay();injecting=previous;
        }
        else if (rcas) ResetNativeHistory(uniform);
        return false;
    }
    const bool previous=injecting; injecting=true;
    bool result=false;
    Bg3VulkanCommands::Tape replacement;
    if (Config::Instance()->OptimizeBg3Fsr2.value_or_default() &&
        TransactionBackend(Config::Instance()->VulkanUpscaler.value_or_default()) &&
        (State::Instance().newBackend==Upscaler::Reset || TransactionBackend(State::Instance().newBackend)))
    {
        {Bg3VulkanCommands::Capture capture(cmd,replacement);result=Bg3Fsr2Bridge::Dispatch(frame);}
        if (result && sequence && (sequence->skipped&2) && !Bg3Fsr2Bridge::UsesAutoExposure())
        {result=false;Bg3Fsr2Bridge::DiscardEvaluation();LOG_WARN("BG3FSR native exposure required; original passes restored");}
        if (!replacement.valid) {result=false;Bg3Fsr2Bridge::DiscardEvaluation();LOG_WARN("BG3FSR command transaction rejected; original passes restored");}
        if (result)
        {
            if (sequence) sequence->tape.ReplaySynchronization();
            replacement.Replay();
            if (sequence)
            {
                {std::scoped_lock lock(mutex);dirtyNativeHistory.insert(sequence->uniform.device);}
            }
        }
        else
        {
            ResetNativeHistory(sequence?sequence->uniform:uniform);
            if (sequence) sequence->tape.Replay();
        }
    }
    else
    {
        ResetNativeHistory(sequence?sequence->uniform:uniform);
        if (sequence) sequence->tape.Replay();
        result=Bg3Fsr2Bridge::Dispatch(frame);
    }
    // FFX changes compute bindings and global push constants, but never graphics/dynamic state.
    originalCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,restorePipeline);
    std::vector<const DescriptorBind*> ordered;
    for (const auto& [index,call] : restoreDescriptors) ordered.push_back(&call);
    std::sort(ordered.begin(),ordered.end(),[](const auto* a,const auto* b){return a->order<b->order;});
    for (const auto* call : ordered)
        originalCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,call->layout,call->firstSet,
            (uint32_t)call->descriptors.size(),call->descriptors.data(),(uint32_t)call->dynamicOffsets.size(),
            call->dynamicOffsets.empty()?nullptr:call->dynamicOffsets.data());
    for (const auto& push : restorePushes)
        if (push.layout && !push.bytes.empty())
            originalCmdPushConstants(cmd,push.layout,push.stages,push.offset,(uint32_t)push.bytes.size(),push.bytes.data());
    injecting=previous;
    return result;
}
VKAPI_ATTR void VKAPI_CALL HookCmdDispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z)
{
    auto call=[=]{originalCmdDispatch(cmd,x,y,z);};
    if (Bg3VulkanCommands::sdkTape && Bg3VulkanCommands::sdkCommand==cmd)
    {Bg3VulkanCommands::Record(cmd,Bg3VulkanCommands::Kind::Dispatch,call);return;}
    if (!ObserveDispatch(cmd,x,y,z,call)) call();
}
VKAPI_ATTR void VKAPI_CALL HookCmdDispatchIndirect(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset)
{
    uint32_t groups[3] {};
    if (Collecting())
    {
        std::scoped_lock lock(mutex);
        auto allocation = bufferMemory.find(buffer);
        if (allocation != bufferMemory.end())
        {
            auto mapped = mappings.find(allocation->second.memory);
            const auto start = allocation->second.offset + offset;
            if (mapped != mappings.end() && start >= mapped->second.offset)
            {
                auto relative = start - mapped->second.offset;
                if (mapped->second.size == VK_WHOLE_SIZE ||
                    (relative <= mapped->second.size && sizeof(groups) <= mapped->second.size - relative))
                    CopyMemorySafe(groups, mapped->second.pointer + relative, sizeof(groups));
            }
        }
    }
    auto call=[=]{originalCmdDispatchIndirect(cmd,buffer,offset);};
    if (Bg3VulkanCommands::sdkTape && Bg3VulkanCommands::sdkCommand==cmd)
    {Bg3VulkanCommands::Record(cmd,Bg3VulkanCommands::Kind::Dispatch,call);return;}
    if (!ObserveDispatch(cmd,groups[0],groups[1],groups[2],call)) call();
}
VKAPI_ATTR void VKAPI_CALL HookCmdDispatchBase(VkCommandBuffer cmd, uint32_t bx, uint32_t by, uint32_t bz,
                                             uint32_t x, uint32_t y, uint32_t z)
{
    auto call=[=]{originalCmdDispatchBase(cmd,bx,by,bz,x,y,z);};
    if (Bg3VulkanCommands::sdkTape && Bg3VulkanCommands::sdkCommand==cmd)
    {Bg3VulkanCommands::Record(cmd,Bg3VulkanCommands::Kind::Dispatch,call);return;}
    if (!bx && !by && !bz && ObserveDispatch(cmd,x,y,z,call)) return;
    FlushNative(cmd);
    originalCmdDispatchBase(cmd, bx, by, bz, x, y, z);
}

VKAPI_ATTR VkResult VKAPI_CALL HookBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset)
{
    auto result = originalBindBufferMemory(device, buffer, memory, offset);
    if (result == VK_SUCCESS && Collecting()) { std::scoped_lock lock(mutex); bufferMemory[buffer] = {memory,offset}; }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL HookBindBufferMemory2(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo* infos)
{
    auto result = originalBindBufferMemory2(device, count, infos);
    if (result == VK_SUCCESS && Collecting())
    {
        std::scoped_lock lock(mutex);
        for (uint32_t i = 0; i < count; ++i)
            bufferMemory[infos[i].buffer] = { infos[i].memory, infos[i].memoryOffset };
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL HookAllocateMemory(VkDevice device,const VkMemoryAllocateInfo* info,const VkAllocationCallbacks* allocator,VkDeviceMemory* memory)
{
    auto result=originalAllocateMemory(device,info,allocator,memory);
    if (result==VK_SUCCESS && Collecting())
    {
        std::scoped_lock lock(mutex);
        auto properties=memoryProperties.find(device);
        if (properties!=memoryProperties.end() && info->memoryTypeIndex<properties->second.memoryTypeCount)
            allocations[*memory]={device,properties->second.memoryTypes[info->memoryTypeIndex].propertyFlags};
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL HookMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
                                            VkDeviceSize size, VkMemoryMapFlags flags, void** data)
{
    auto result = originalMapMemory(device, memory, offset, size, flags, data);
    if (result == VK_SUCCESS && Collecting())
    {
        std::scoped_lock lock(mutex);
        mappings[memory] = {offset,size,static_cast<const uint8_t*>(*data)};
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL HookUnmapMemory(VkDevice device, VkDeviceMemory memory)
{
    if (Collecting()) { std::scoped_lock lock(mutex); mappings.erase(memory); }
    originalUnmapMemory(device, memory);
}
VKAPI_ATTR void VKAPI_CALL HookFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* allocator)
{
    if (Collecting()) { std::scoped_lock lock(mutex); mappings.erase(memory);allocations.erase(memory); }
    originalFreeMemory(device, memory, allocator);
}
} // namespace

PFN_vkVoidFunction GetAddress(const char* name, PFN_vkVoidFunction original)
{
    if (!enabled || !original) return nullptr;
    if (strcmp(name, "vkBindBufferMemory2KHR") == 0)
    {
        if (!originalBindBufferMemory2)
            originalBindBufferMemory2 = reinterpret_cast<PFN_vkBindBufferMemory2>(original);
        return reinterpret_cast<PFN_vkVoidFunction>(HookBindBufferMemory2);
    }
    if (strcmp(name,"vkQueueSubmit2KHR")==0)
    {
        if (!originalQueueSubmit2) originalQueueSubmit2=reinterpret_cast<PFN_vkQueueSubmit2>(original);
        return reinterpret_cast<PFN_vkVoidFunction>(HookQueueSubmit2);
    }
#define RETURN_WRAPPER(function) \
    if (strcmp(name, "vk" #function) == 0) \
    { \
        if (!original##function) original##function = reinterpret_cast<PFN_vk##function>(original); \
        return reinterpret_cast<PFN_vkVoidFunction>(Hook##function); \
    }
    BG3_VK_FUNCTIONS(RETURN_WRAPPER)
#undef RETURN_WRAPPER
    return Bg3VulkanCommands::Resolve(name,original);
}

PFN_vkVoidFunction RecordingDeviceProcAddr(VkDevice device,const char* name)
{
    const auto address=VulkanHooks::GetDeviceProcAddr(device,name);
    if (auto captured=Bg3VulkanCommands::Resolve(name,address)) return captured;
    if (!strncmp(name,"vkCmd",5)) return nullptr;
    return address;
}

void DeviceCreated(VkPhysicalDevice physical, VkDevice device)
{
    if (!Collecting()) return;
    VkPhysicalDeviceMemoryProperties properties {};vkGetPhysicalDeviceMemoryProperties(physical,&properties);
    std::scoped_lock lock(mutex);
    memoryProperties[device]=properties;
    auto instance=physicalInstances.find(physical);
    if (instance!=physicalInstances.end()) devices[device]={physical,instance->second};
}

bool ReplacementEnabled()
{
    return enabled.load(std::memory_order_relaxed) && Config::Instance()->ReplaceBg3Fsr2.value_or_default();
}

void HookVulkan(HMODULE module)
{
    std::call_once(initOnce, Initialize);
    if (!supportedExecutable) return;
    std::call_once(vulkanOnce, [module]() {
    Bg3VulkanCommands::nativeActive=NativeRecording;Bg3VulkanCommands::nativeSink=RecordNative;
    HookBackend::Begin();
#define ATTACH_WRAPPER(function) \
    if (!original##function) \
    { \
        original##function = reinterpret_cast<PFN_vk##function>(KernelBaseProxy::GetProcAddress_()(module,"vk" #function)); \
        if (original##function) HookBackend::Attach(reinterpret_cast<PVOID*>(&original##function), Hook##function); \
    }
    BG3_VK_FUNCTIONS(ATTACH_WRAPPER)
#undef ATTACH_WRAPPER
#define ATTACH_COMMAND(function) \
    if (strcmp("vk" #function,"vkCmdBindPipeline") && strcmp("vk" #function,"vkCmdBindDescriptorSets") && \
        strcmp("vk" #function,"vkCmdPushConstants") && strcmp("vk" #function,"vkCmdDispatch") && \
        strcmp("vk" #function,"vkCmdDispatchIndirect") && strcmp("vk" #function,"vkCmdDispatchBase")) \
    { \
        auto& address=Bg3VulkanCommands::original##function; \
        if (!address) {address=(PFN_vk##function)KernelBaseProxy::GetProcAddress_()(module,"vk" #function); \
            if (address) HookBackend::Attach(reinterpret_cast<PVOID*>(&address),Bg3VulkanCommands::function);} \
    }
    BG3_CAPTURE_FUNCTIONS(ATTACH_COMMAND)
#undef ATTACH_COMMAND
    auto result = HookBackend::Commit();
    if (FAILED(result))
    {
        enabled = false;
        LOG_ERROR("BG3FSR2 Vulkan observer hooks failed: {:X}; replacement disabled", (UINT) result);
    }
    else
    {
        enabled = true;
        LOG_INFO("BG3FSR native Vulkan replacement hooks installed");
    }
    });
}
} // namespace Bg3Fsr2
