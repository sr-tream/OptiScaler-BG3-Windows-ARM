#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <mmsystem.h>
#include <d3d11.h>
#include <cstdio>
#include <cstring>

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2)
        return 1;
    // Resolve the reference before OptiScaler installs its DLL-loading hooks.
    auto system = LoadLibraryExW(L"winmm.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto systemCaps = reinterpret_cast<decltype(&timeGetDevCaps)>(GetProcAddress(system, "timeGetDevCaps"));
    auto module = LoadLibraryW(argv[1]);
    if (!module)
    {
        printf("OptiScaler LoadLibrary failed: %lu\n", GetLastError());
        return 2;
    }
    auto caps = reinterpret_cast<decltype(&timeGetDevCaps)>(GetProcAddress(module, "timeGetDevCaps"));
    auto ticks = reinterpret_cast<decltype(&timeGetTime)>(GetProcAddress(module, "timeGetTime"));
    auto begin = reinterpret_cast<decltype(&timeBeginPeriod)>(GetProcAddress(module, "timeBeginPeriod"));
    auto end = reinterpret_cast<decltype(&timeEndPeriod)>(GetProcAddress(module, "timeEndPeriod"));
    if (!caps || !systemCaps || !ticks || !begin || !end)
        return 3;
    TIMECAPS a {}, b {};
    const auto result = caps(&a, sizeof(a));
    const auto expected = systemCaps(&b, sizeof(b));
    printf("timeGetDevCaps: %u vs %u; periods %u/%u vs %u/%u\n", result, expected,
           a.wPeriodMin, a.wPeriodMax, b.wPeriodMin, b.wPeriodMax);
    if (result != expected || memcmp(&a, &b, sizeof(a)) != 0)
        return 4;
    const auto tick = ticks();
    if (!tick)
        return 5;
    const auto period = a.wPeriodMin;
    if (begin(period) != TIMERR_NOERROR || end(period) != TIMERR_NOERROR)
        return 6;
    auto d3d11 = LoadLibraryW(L"d3d11.dll");
    auto create = reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(d3d11, "D3D11CreateDevice"));
    if (!create)
        return 7;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL level;
    const auto created = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                D3D11_SDK_VERSION, &device, &level, &context);
    printf("D3D11 hardware device creation: %08lx (feature level %x)\n", created, level);
    if (FAILED(created))
        return 8;
    context->Release();
    device->Release();
    printf("OptiScaler ARM64EC DLL load + winmm exports from x64 process: PASS (%lu ms)\n", tick);
    // OptiScaler owns worker threads; let process termination perform its normal cleanup.
    return 0;
}
