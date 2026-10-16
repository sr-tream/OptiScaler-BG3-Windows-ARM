#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdio>
int wmain(int argc,wchar_t** argv)
{
    if (argc!=3) return 1;
    auto module=LoadLibraryW(argv[1]);
    auto test=module?(int(*)(const wchar_t*))GetProcAddress(module,"RunCommandHooks"):nullptr;
    if (!test) return 2;
    const auto result=test(argv[2]);
    printf("Mixed x64/ARM64EC Vulkan command transaction: %s\n",result?"FAIL":"PASS");
    return result;
}
