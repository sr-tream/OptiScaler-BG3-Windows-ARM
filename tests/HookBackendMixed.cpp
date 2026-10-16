#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objidl.h>
#include <cstdio>
#include "hooks/HookBackend.h"

using TestFunction = int(WINAPI*)(int);
static TestFunction originalTest;
static int WINAPI TestHook(int value) { return originalTest(value) + 100; }
static void NativeProbeHook(void*) {}

extern "C" __declspec(dllexport) int WINAPI ProbeNativeTarget(PVOID target)
{
    unsigned char before[32];
    memcpy(before, target, sizeof(before));
    PVOID trampoline = target;
    if (FAILED(HookBackend::Begin()) || FAILED(HookBackend::Attach(&trampoline, NativeProbeHook)) ||
        FAILED(HookBackend::Commit()))
        return 1;
    const bool attached = trampoline != target && memcmp(before, target, sizeof(before)) != 0;
    HookBackend::Begin();
    HookBackend::Detach(&trampoline, NativeProbeHook);
    if (FAILED(HookBackend::Commit()))
        return 2;
    return attached && memcmp(before, target, sizeof(before)) == 0 ? 0 : 3;
}
__declspec(noinline) static int WINAPI NativeTest(int value) { return value * 2; }

using StreamRead = HRESULT(STDMETHODCALLTYPE*)(IStream*, void*, ULONG, ULONG*);
static StreamRead originalRead;
static int readCalls;
static HRESULT STDMETHODCALLTYPE ReadHook(IStream* stream, void* buffer, ULONG count, ULONG* read)
{
    ++readCalls;
    return originalRead(stream, buffer, count, read);
}

static bool TestTarget(TestFunction target)
{
    printf("  target %p: attach\n", reinterpret_cast<void*>(target));
    fflush(stdout);
    originalTest = target;
    if (FAILED(HookBackend::Begin()) || FAILED(HookBackend::Attach(reinterpret_cast<PVOID*>(&originalTest), TestHook)) ||
        FAILED(HookBackend::Commit()))
        return false;
    printf("  call hooked target\n");
    fflush(stdout);
    volatile TestFunction call = target;
    const bool hooked = call(7) == 114 && originalTest(7) == 14;
    printf("  detach\n");
    fflush(stdout);
    HookBackend::Begin();
    HookBackend::Detach(reinterpret_cast<PVOID*>(&originalTest), TestHook);
    const bool detached = SUCCEEDED(HookBackend::Commit()) && call(7) == 14;

    // A failed attach must abort the entire batch, including successful earlier attaches.
    PVOID invalid = nullptr;
    printf("  rollback\n");
    fflush(stdout);
    HookBackend::Begin();
    HookBackend::Attach(reinterpret_cast<PVOID*>(&originalTest), TestHook);
    HookBackend::Attach(&invalid, TestHook);
    const bool rolledBack = FAILED(HookBackend::Commit()) && call(7) == 14;
    printf("  abort\n");
    fflush(stdout);
    HookBackend::Begin();
    HookBackend::Attach(reinterpret_cast<PVOID*>(&originalTest), TestHook);
    const bool aborted = SUCCEEDED(HookBackend::Abort()) && call(7) == 14;
    return hooked && detached && rolledBack && aborted;
}

extern "C" __declspec(dllexport) int WINAPI RunHookTests(TestFunction x64Target)
{
    printf("Testing x64 function\n");
    fflush(stdout);
    if (!TestTarget(x64Target))
        return 1;
    printf("Testing ARM64EC function\n");
    fflush(stdout);
    if (!TestTarget(NativeTest))
        return 2;

    printf("Testing system ARM64EC COM method\n");
    fflush(stdout);
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
        return 3;
    const char content[] = "mixed ARM64EC hook";
    stream->Write(content, sizeof(content), nullptr);
    LARGE_INTEGER position {};
    stream->Seek(position, STREAM_SEEK_SET, nullptr);
    auto targetRead = reinterpret_cast<StreamRead>((*reinterpret_cast<PVOID**>(stream))[3]);
    originalRead = targetRead;
    auto isEcCode = reinterpret_cast<BOOL(WINAPI*)(PVOID)>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlIsEcCode"));
    if (!isEcCode || !isEcCode(reinterpret_cast<PVOID>(targetRead)))
    {
        stream->Release();
        return 4;
    }
    HookBackend::Begin();
    HookBackend::Attach(reinterpret_cast<PVOID*>(&originalRead), ReadHook);
    if (FAILED(HookBackend::Commit()))
    {
        stream->Release();
        return 5;
    }
    char buffer[64] {};
    ULONG read = 0;
    const auto result = stream->Read(buffer, sizeof(content), &read);
    const bool ok = SUCCEEDED(result) && read == sizeof(content) && readCalls == 1 &&
                    memcmp(buffer, content, sizeof(content)) == 0;
    HookBackend::Begin();
    HookBackend::Detach(reinterpret_cast<PVOID*>(&originalRead), ReadHook);
    const bool detached = SUCCEEDED(HookBackend::Commit());
    stream->Release();
    return ok && detached ? 0 : 6;
}
