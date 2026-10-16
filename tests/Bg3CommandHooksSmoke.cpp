#include <hooks/HookBackend.h>
#include "VulkanSmoke.cpp"

extern "C" __declspec(dllexport) int RunCommandHooks(const wchar_t* sdkPath)
{
    auto loader=LoadLibraryW(L"vulkan-1.dll");
    if (!loader) return 30;
    HookBackend::Begin();
    unsigned count=0;
#define ATTACH_COMMAND(name) \
    Bg3VulkanCommands::original##name=(PFN_vk##name)GetProcAddress(loader,"vk" #name); \
    if (Bg3VulkanCommands::original##name) { \
        HookBackend::Attach((PVOID*)&Bg3VulkanCommands::original##name,Bg3VulkanCommands::name);++count; }
    BG3_CAPTURE_FUNCTIONS(ATTACH_COMMAND)
#undef ATTACH_COMMAND
    const auto installed=HookBackend::Commit();
    printf("ARM64EC Vulkan command hook batch: %u hooks, HRESULT %08lx\n",count,installed);
    if (FAILED(installed)) return 31;
    const auto result=SafeTestVulkan(sdkPath);
    HookBackend::Begin();
#define DETACH_COMMAND(name) \
    if (Bg3VulkanCommands::original##name) HookBackend::Detach((PVOID*)&Bg3VulkanCommands::original##name,Bg3VulkanCommands::name);
    BG3_CAPTURE_FUNCTIONS(DETACH_COMMAND)
#undef DETACH_COMMAND
    const auto detached=HookBackend::Commit();
    printf("ARM64EC Vulkan command hook detach: %08lx; GPU result: %d\n",detached,result);
    return result?result:FAILED(detached)?32:0;
}
