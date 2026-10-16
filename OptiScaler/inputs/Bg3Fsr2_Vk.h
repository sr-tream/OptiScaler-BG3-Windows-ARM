#pragma once
#include <Windows.h>
#include <vulkan/vulkan.h>

// Version-checked replacement of BG3's engine-integrated Vulkan FSR2 renderer.
namespace Bg3Fsr2
{
void HookVulkan(HMODULE module);
PFN_vkVoidFunction GetAddress(const char* name, PFN_vkVoidFunction original);
void DeviceCreated(VkPhysicalDevice physicalDevice, VkDevice device);
bool ReplacementEnabled();
PFN_vkVoidFunction RecordingDeviceProcAddr(VkDevice device,const char* name);
}
