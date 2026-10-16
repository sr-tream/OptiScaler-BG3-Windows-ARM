#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <vulkan/vulkan.h>
#include <ffx_api.h>
#include <ffx_upscale.h>
#include <vk/ffx_api_vk.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <inputs/Bg3VulkanCommands.h>
#include "VulkanNativeFrame.h"

static PFN_vkVoidFunction RecordingResolver(VkDevice device,const char* name)
{
    const auto original=vkGetDeviceProcAddr(device,name);
    if (auto wrapper=Bg3VulkanCommands::Resolve(name,original)) return wrapper;
    if (!strncmp(name,"vkCmd",5)) return nullptr;
    return original;
}
#ifdef TEST_STATIC_FSR
#include "VulkanStaticFsr.h"
#endif

static int TestVulkan(const wchar_t* sdkPath)
{
    VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "OptiScaler Vulkan check";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instanceInfo { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instanceInfo.pApplicationInfo = &app;
    VkInstance instance = nullptr;
    auto result = vkCreateInstance(&instanceInfo, nullptr, &instance);
    if (result != VK_SUCCESS)
    {
        printf("vkCreateInstance: %d\n", result);
        return 1;
    }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    VkPhysicalDevice physical = nullptr;
    for (auto candidate : devices)
    {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(candidate, &properties);
        // Use the native hardware driver, excluding the D3D12 mapping driver.
        if (!strstr(properties.deviceName, "Microsoft") && properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU)
        {
            physical = candidate;
            printf("Vulkan GPU: %s (vendor %04x)\n", properties.deviceName, properties.vendorID);
            break;
        }
    }
    if (!physical)
        return 2;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    uint32_t family = count;
    for (uint32_t i = 0; i < count; ++i)
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
    if (family == count)
        return 3;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> supportedExtensions(count);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, supportedExtensions.data());
    std::vector<const char*> extensions;
    for (const auto& extension : supportedExtensions)
        if (!strcmp(extension.extensionName, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME) ||
            !strcmp(extension.extensionName, VK_KHR_16BIT_STORAGE_EXTENSION_NAME) ||
            !strcmp(extension.extensionName, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME) ||
            !strcmp(extension.extensionName, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME) ||
            !strcmp(extension.extensionName, VK_KHR_BIND_MEMORY_2_EXTENSION_NAME))
            extensions.push_back(extension.extensionName);
    VkPhysicalDeviceVulkan12Features features12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan11Features features11 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    features11.pNext = &features12;
    VkPhysicalDeviceFeatures2 features { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    features.pNext = &features11;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    printf("shaderFloat16: %u, shaderInt16: %u\n", features12.shaderFloat16, features.features.shaderInt16);
    float priority = 1;
    VkDeviceQueueCreateInfo queue { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queue.queueFamilyIndex = family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceInfo.pNext = &features11;
    deviceInfo.pEnabledFeatures = &features.features;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queue;
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    deviceInfo.ppEnabledExtensionNames = extensions.data();
    VkDevice device = nullptr;
    result = vkCreateDevice(physical, &deviceInfo, nullptr, &device);
    printf("vkCreateDevice: %d\n", result);
    fflush(stdout);
    if (result != VK_SUCCESS)
        return 4;
    int status = 0;
#ifdef TEST_STATIC_FSR
    status=TestStaticFsr(device,physical,family);
    vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);return status;
#endif
    if (!sdkPath)
    {
        // Games commonly retrieve the device resolver through the instance resolver.
        // That route must keep the same command interception as the loader export.
        auto instanceGDPA = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            vkGetInstanceProcAddr(instance, "vkGetDeviceProcAddr"));
        const auto viaInstance = instanceGDPA ? instanceGDPA(device, "vkCmdBindPipeline") : nullptr;
        const auto viaExport = vkGetDeviceProcAddr(device, "vkCmdBindPipeline");
        printf("Instance-provided device resolver: %s (instance %p, export %p)\n",
               viaInstance && viaInstance == viaExport ? "PASS" : "FAIL",
               reinterpret_cast<void*>(viaInstance), reinterpret_cast<void*>(viaExport));
        if (!viaInstance || viaInstance != viaExport)
            status = 9;
    }
    if (sdkPath)
    {
        auto sdk = LoadLibraryW(sdkPath);
        auto query = sdk ? reinterpret_cast<PfnFfxQuery>(GetProcAddress(sdk, "ffxQuery")) : nullptr;
        auto create = sdk ? reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(sdk, "ffxCreateContext")) : nullptr;
        auto destroy = sdk ? reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(sdk, "ffxDestroyContext")) : nullptr;
        auto dispatch = sdk ? reinterpret_cast<PfnFfxDispatch>(GetProcAddress(sdk,"ffxDispatch")) : nullptr;
        if (!query || !create || !destroy || !dispatch)
            return 5;
        uint64_t versionCount = 0;
        ffxQueryDescGetVersions versions {};
        versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        versions.outputCount = &versionCount;
        auto queried = query(nullptr, &versions.header);
        std::vector<uint64_t> ids(versionCount);
        std::vector<const char*> names(versionCount);
        versions.versionIds = ids.data();
        versions.versionNames = names.data();
        if (queried != FFX_API_RETURN_OK || !versionCount || query(nullptr, &versions.header) != FFX_API_RETURN_OK)
            return 6;
        size_t selected = names.size();
        for (size_t i = 0; i < names.size(); ++i)
        {
            printf("FFX upscaler %zu: %s\n", i, names[i]);
            if (strstr(names[i], "3.1"))
                selected = i;
        }
        if (selected == names.size())
            return 7;
        std::vector<size_t> sequence {selected};
        for (size_t i=0;i<names.size();++i)
            if (strstr(names[i], "2.3")) sequence.push_back(i);
        sequence.push_back(selected);
        for (auto index:sequence)
        {
        ffxOverrideVersion overrideVersion {};
        overrideVersion.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        overrideVersion.versionId = ids[index];
        ffxCreateBackendVKDesc backend {};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        backend.header.pNext = &overrideVersion.header;
        backend.vkDevice = device;
        backend.vkPhysicalDevice = physical;
        backend.vkDeviceProcAddr = RecordingResolver;
        ffxCreateContextDescUpscale desc {};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        desc.header.pNext = &backend.header;
        desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
#ifdef TEST_AUTO_EXPOSURE
        desc.flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
#endif
        desc.maxRenderSize = { 64, 64 };
        desc.maxUpscaleSize = { 128, 128 };
        ffxContext context = nullptr;
        printf("Creating %s Vulkan context\n",names[index]);
        fflush(stdout);
        auto created = create(&context, &desc.header, nullptr);
        printf("%s Vulkan context result: %u\n",names[index],created);
        fflush(stdout);
        status = created == FFX_API_RETURN_OK ? 0 : 8;
        if (created == FFX_API_RETURN_OK)
        {
            status=TestNativeFrame(device,physical,family,&context,dispatch);
            destroy(&context, nullptr);
        }
        if (status) break;
        }
        FreeLibrary(sdk);
    }
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return status;
}

static int ReportException(EXCEPTION_POINTERS* exception)
{
    const auto address = exception->ExceptionRecord->ExceptionAddress;
    MEMORY_BASIC_INFORMATION memory {};
    VirtualQuery(address, &memory, sizeof(memory));
    wchar_t module[MAX_PATH] {};
    GetModuleFileNameW(static_cast<HMODULE>(memory.AllocationBase), module, MAX_PATH);
    printf("Exception %08lx at %p, module %ls + %llx\n", exception->ExceptionRecord->ExceptionCode,
           address, module, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                                          reinterpret_cast<uintptr_t>(memory.AllocationBase)));
    if (exception->ExceptionRecord->NumberParameters >= 2)
        printf("Access type %llu, address %p\n", static_cast<unsigned long long>(exception->ExceptionRecord->ExceptionInformation[0]),
               reinterpret_cast<void*>(exception->ExceptionRecord->ExceptionInformation[1]));
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

static int SafeTestVulkan(const wchar_t* path)
{
    __try { return TestVulkan(path); }
    __except (ReportException(GetExceptionInformation())) { return 20; }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3)
        return 10;
    const auto sdkResult = SafeTestVulkan(argv[1]);
    if (sdkResult)
        return sdkResult;
    printf("Loading OptiScaler ARM64EC Vulkan profile\n");
    fflush(stdout);
    if (!LoadLibraryW(argv[2]))
        return 11;
    const auto hookResult = SafeTestVulkan(nullptr);
    printf("OptiScaler Vulkan device hooks: %s (%d). Synthetic native-format GPU frames tested; no game benchmark.\n",
           hookResult ? "FAIL" : "PASS", hookResult);
    return hookResult;
}
