#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#include <Windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <vector>

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2)
        return 1;
    WNDCLASSW wc {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"OptiScalerHiddenVulkanCheck";
    if (!RegisterClassW(&wc))
        return 2;
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"OptiScaler Vulkan input check", WS_OVERLAPPEDWINDOW,
                              0, 0, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd || !LoadLibraryW(argv[1]))
        return 3;

    const char* instanceExtensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "OptiScaler hidden Vulkan overlay check";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo info { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = instanceExtensions;
    VkInstance instance = nullptr;
    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS)
        return 4;

    // BG3 uses this instance dispatch path instead of the loader's exported function.
    auto createSurface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
    VkWin32SurfaceCreateInfoKHR surfaceInfo { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    surfaceInfo.hinstance = wc.hInstance;
    surfaceInfo.hwnd = hwnd;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!createSurface || createSurface(instance, &surfaceInfo, nullptr, &surface) != VK_SUCCESS)
        return 5;

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    VkPhysicalDevice physical = nullptr;
    uint32_t family = 0;
    for (auto candidate : devices)
    {
        VkPhysicalDeviceProperties props {};
        vkGetPhysicalDeviceProperties(candidate, &props);
        if (strstr(props.deviceName, "Microsoft") || props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
            continue;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
        for (uint32_t i = 0; i < count; ++i)
        {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &present);
            if (present && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            {
                physical = candidate;
                family = i;
                break;
            }
        }
        if (physical)
        {
            printf("GPU: %s, HWND: %p\n", props.deviceName, hwnd);
            break;
        }
    }
    if (!physical)
        return 6;
    float priority = 1;
    VkDeviceQueueCreateInfo queueInfo { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const char* deviceExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo deviceInfo { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = &deviceExtension;
    VkDevice device = nullptr;
    if (vkCreateDevice(physical, &deviceInfo, nullptr, &device) != VK_SUCCESS)
        return 7;
    VkSurfaceCapabilitiesKHR capabilities {};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &capabilities);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data());
    VkSwapchainCreateInfoKHR sc { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sc.surface = surface;
    sc.minImageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount && sc.minImageCount > capabilities.maxImageCount)
        sc.minImageCount = capabilities.maxImageCount;
    sc.imageFormat = formats[0].format;
    sc.imageColorSpace = formats[0].colorSpace;
    sc.imageExtent = capabilities.currentExtent;
    if (sc.imageExtent.width == UINT32_MAX)
        sc.imageExtent = { 640, 480 };
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc.preTransform = capabilities.currentTransform;
    sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sc.clipped = VK_TRUE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    auto createSwapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(vkGetDeviceProcAddr(device, "vkCreateSwapchainKHR"));
    const auto result = createSwapchain(device, &sc, nullptr, &swapchain);
    printf("Vulkan overlay swapchain: %d\n", result);
    if (result != VK_SUCCESS)
        return 8;
    // Exercise the real Win32 cursor trampolines from an x64 process after input hooks attach.
    // Reapplying the current rectangle preserves the existing desktop confinement.
    RECT clipBefore {}, clipAfter {};
    if (!GetClipCursor(&clipBefore) || !ClipCursor(&clipBefore) || !GetClipCursor(&clipAfter) ||
        memcmp(&clipBefore,&clipAfter,sizeof(RECT)) != 0)
        return 9;
    printf("PASS: mixed x64/ARM64EC GetClipCursor and ClipCursor hooks preserve the current rectangle.\n");
    fflush(stdout);
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    DestroyWindow(hwnd);
    printf("PASS: hidden Vulkan surface/swapchain initialized; input registration must also pass log checks.\n");
    return 0;
}
