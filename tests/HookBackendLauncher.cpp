#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdio>

__declspec(noinline) static int WINAPI EmulatedTest(int value) { return value * 2; }
int wmain(int argc, wchar_t** argv)
{
    auto library = LoadLibraryW(L"HookBackendMixed.dll");
    if (!library)
    {
        printf("LoadLibrary failed: %lu\n", GetLastError());
        return 10;
    }
    using RunTests = int(WINAPI*)(int(WINAPI*)(int));
    auto run = reinterpret_cast<RunTests>(GetProcAddress(library, "RunHookTests"));
    if (!run)
        return 11;
    const int result = run(EmulatedTest);
    printf("x64 process + ARM64EC hook DLL: %s (result %d)\n", result == 0 ? "PASS" : "FAIL", result);
    int probeResult = 0;
    if (result == 0 && argc == 2)
    {
        auto image = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
        auto probe = reinterpret_cast<int(WINAPI*)(PVOID)>(GetProcAddress(library, "ProbeNativeTarget"));
        if (!image || !probe)
            return 12;
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<unsigned char*>(image) +
                   reinterpret_cast<IMAGE_DOS_HEADER*>(image)->e_lfanew);
        if (nt->FileHeader.TimeDateStamp != 0x6ab131a2 || nt->OptionalHeader.SizeOfImage != 0x6833000)
            return 13;
        probeResult = probe(reinterpret_cast<unsigned char*>(image) + 0x3eebc60);
        printf("Actual BG3 FSR2 prologue hook/detach, without executing game code: %s (%d)\n",
               probeResult == 0 ? "PASS" : "FAIL", probeResult);
        FreeLibrary(image);
    }
    FreeLibrary(library);
    return result ? result : probeResult;
}
