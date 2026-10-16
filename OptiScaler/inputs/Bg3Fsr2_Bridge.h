#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <memory>
#include <vector>
class IFeature_Vk;

namespace Bg3Fsr2Bridge
{
inline constexpr unsigned HandleId = 0xB3000001;
struct Image
{
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageSubresourceRange range {};
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
struct Frame
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    std::array<uint32_t, 40> constants {};
    Image color, motion, depth, reactive, composition, exposure, output;
    float sharpness = 0;
    unsigned stages = 0;
};
bool Dispatch(const Frame& frame);
void DestroyDevice(VkDevice device);
void ResetRecording(VkCommandBuffer cmd);
void ExecuteCommands(VkCommandBuffer primary,uint32_t count,const VkCommandBuffer* secondary);
void Submitted(VkQueue queue,const std::vector<VkCommandBuffer>& commands,PFN_vkQueueSubmit submit);
std::shared_ptr<IFeature_Vk> CurrentFeatureLifetime();
void WindowSuspended(bool suspended);
void SwapchainChanged();
void DiscardEvaluation();
bool UsesAutoExposure();
}
