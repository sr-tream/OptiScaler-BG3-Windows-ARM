#include "pch.h"
#include "input_system_internal.h"

#include <hooks/Kernel_Hooks.h>

#include <hooks/HookBackend.h>

#include <tlhelp32.h>
#include <vector>
#include <string_view>

// #define USE_HID_HOOKS

static bool messageHooks = false;
static bool keyStateHooks = false;
static bool getPosHooks = false;
static bool clipCursorHooks = false;
static bool message2Hooks = false;
static bool hidHooks = false;
static bool rawHooks = false;
static bool windowsHooks = false;
static bool positionHooks = false;
static bool positionIATHooks = false;

namespace OptiInput
{
SHORT RealGetAsyncKeyStateSafe(int vk)
{
    ScopedHookBypass bypass;

    if (o_GetAsyncKeyState != nullptr)
        return o_GetAsyncKeyState(vk);

    return ::GetAsyncKeyState(vk);
}

SHORT RealGetKeyStateSafe(int vk)
{
    ScopedHookBypass bypass;

    if (o_GetKeyState != nullptr)
        return o_GetKeyState(vk);

    return ::GetKeyState(vk);
}

BOOL RealGetCursorPosSafe(LPPOINT point)
{
    ScopedHookBypass bypass;

    if (o_GetCursorPos != nullptr)
        return o_GetCursorPos(point);

    return ::GetCursorPos(point);
}

namespace
{
struct IatPatch
{
    void** Entry = nullptr;
    void* Original = nullptr;
    void* Detour = nullptr;
    std::string Name;
};

std::vector<IatPatch> gCursorIatPatches;

bool IsSelfModule(HMODULE module)
{
    HMODULE selfModule = nullptr;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&InstallHooks), &selfModule))
    {
        return false;
    }

    return module == selfModule;
}

bool PatchIatEntry(HMODULE module, const char* importName, void* original, void* detour)
{
    if (module == nullptr || importName == nullptr || original == nullptr || detour == nullptr)
        return false;

    if (IsSelfModule(module))
        return false;

    auto* base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);

    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);

    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    const auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    if (importDir.VirtualAddress == 0 || importDir.Size == 0)
        return false;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
    bool patched = false;

    for (; desc->Name != 0; ++desc)
    {
        auto* thunkOrig = desc->OriginalFirstThunk != 0
                              ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk)
                              : nullptr;

        auto* thunkIat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);

        if (thunkIat == nullptr)
            continue;

        for (size_t index = 0; thunkIat[index].u1.Function != 0; ++index)
        {
            const char* name = nullptr;

            if (thunkOrig != nullptr && !(thunkOrig[index].u1.Ordinal & IMAGE_ORDINAL_FLAG))
            {
                auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + thunkOrig[index].u1.AddressOfData);
                name = reinterpret_cast<const char*>(byName->Name);
            }

            if (name == nullptr || strcmp(name, importName) != 0)
                continue;

            auto** entry = reinterpret_cast<void**>(&thunkIat[index].u1.Function);

            // Only patch entries that still point to the real API.
            // This avoids overwriting another hook chain.
            if (*entry != original)
            {
                LOG_DEBUG("OptiInput IAT skip {} module:{} entry:{} current:{} expected:{}", importName,
                          static_cast<void*>(module), static_cast<void*>(entry), *entry, original);
                continue;
            }

            DWORD oldProtect = 0;
            if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &oldProtect))
            {
                LOG_WARN("OptiInput IAT protect failed {} module:{} entry:{} gle:{}", importName,
                         static_cast<void*>(module), static_cast<void*>(entry), GetLastError());
                continue;
            }

            *entry = detour;

            DWORD ignored = 0;
            VirtualProtect(entry, sizeof(void*), oldProtect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), entry, sizeof(void*));

            gCursorIatPatches.push_back({ entry, original, detour, importName });

            LOG_INFO("OptiInput IAT patched {} module:{} entry:{} original:{} detour:{}", importName,
                     static_cast<void*>(module), static_cast<void*>(entry), original, detour);

            patched = true;
        }
    }

    return patched;
}

bool PatchCursorIatForModule(HMODULE module)
{
    bool patched = false;

    patched |= PatchIatEntry(module, "GetCursorPos", reinterpret_cast<void*>(o_GetCursorPos),
                             reinterpret_cast<void*>(hkGetCursorPos));

    patched |= PatchIatEntry(module, "SetCursorPos", reinterpret_cast<void*>(o_SetCursorPos),
                             reinterpret_cast<void*>(hkSetCursorPos));

    return patched;
}

bool InstallCursorIatHooks()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        LOG_WARN("OptiInput cursor IAT snapshot failed gle:{}", GetLastError());
        return false;
    }

    MODULEENTRY32W entry {};
    entry.dwSize = sizeof(entry);

    bool patchedAny = false;

    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            patchedAny |= PatchCursorIatForModule(entry.hModule);
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);

    LOG_INFO("OptiInput cursor IAT hooks installed patched:{} entries:{}", patchedAny ? 1 : 0,
             gCursorIatPatches.size());

    return patchedAny;
}

void RemoveCursorIatHooks()
{
    for (auto it = gCursorIatPatches.rbegin(); it != gCursorIatPatches.rend(); ++it)
    {
        IatPatch& patch = *it;

        if (patch.Entry == nullptr || patch.Original == nullptr)
            continue;

        DWORD oldProtect = 0;
        if (!VirtualProtect(patch.Entry, sizeof(void*), PAGE_READWRITE, &oldProtect))
            continue;

        if (*patch.Entry == patch.Detour)
            *patch.Entry = patch.Original;

        DWORD ignored = 0;
        VirtualProtect(patch.Entry, sizeof(void*), oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), patch.Entry, sizeof(void*));
    }

    LOG_INFO("OptiInput cursor IAT hooks removed entries:{}", gCursorIatPatches.size());

    gCursorIatPatches.clear();
}

void ResolveOptionalUser32Exports()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");

    if (user32 == nullptr)
        user32 = LoadLibraryW(L"user32.dll");

    if (user32 == nullptr)
        return;

    if (o_GetPhysicalCursorPos == nullptr)
    {
        o_GetPhysicalCursorPos = reinterpret_cast<GetPhysicalCursorPos_t>(
            KernelBaseProxy::GetProcAddress_()(user32, "GetPhysicalCursorPos"));
    }

    if (o_SetPhysicalCursorPos == nullptr)
    {
        o_SetPhysicalCursorPos = reinterpret_cast<SetPhysicalCursorPos_t>(
            KernelBaseProxy::GetProcAddress_()(user32, "SetPhysicalCursorPos"));
    }
}
} // namespace

bool InstallHooks()
{
    if (_state.HooksInstalled)
        return true;

    LOG_INFO("installing Win32 input hooks currentPid:{} targetPid:{} inputPid:{}", _state.CurrentProcessId,
             _state.TargetProcessId, _state.InputProcessId);

    ResolveOptionalUser32Exports();

    if (!messageHooks)
    {
        HookBackend::Begin();

        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_PeekMessageA), hkPeekMessageA);
        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_PeekMessageW), hkPeekMessageW);
        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_GetMessageA), hkGetMessageA);
        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_GetMessageW), hkGetMessageW);

        const HRESULT result = HookBackend::Commit();

        messageHooks = SUCCEEDED(result);
        if (messageHooks)
            LOG_INFO("Win32 message hooks installed");
        else
            LOG_ERROR("Win32 message hook installation failed result:{}", result);
    }

    if (!keyStateHooks)
    {
        HookBackend::Begin();

        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_GetAsyncKeyState), hkGetAsyncKeyState);
        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_GetKeyState), hkGetKeyState);
        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_GetKeyboardState), hkGetKeyboardState);

        const HRESULT result = HookBackend::Commit();

        keyStateHooks = SUCCEEDED(result);
        if (keyStateHooks)
            LOG_INFO("Win32 key state hooks installed");
        else
            LOG_ERROR("Win32 key state hook installation failed result:{}", result);
    }

    if (!getPosHooks)
    {
        HookBackend::Begin();

        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_GetMessagePos), hkGetMessagePos);
        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_GetMouseMovePointsEx), hkGetMouseMovePointsEx);

        const HRESULT result = HookBackend::Commit();

        getPosHooks = SUCCEEDED(result);
        if (getPosHooks)
            LOG_INFO("Win32 GetMessagePos hooks installed");
        else
            LOG_ERROR("Win32 GetMessagePos hook installation failed result:{}", result);
    }

    if (!clipCursorHooks)
    {
        HookBackend::Begin();

        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_ClipCursor), hkClipCursor);
        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_GetClipCursor), hkGetClipCursor);

        const HRESULT result = HookBackend::Commit();

        clipCursorHooks = SUCCEEDED(result);
        if (clipCursorHooks)
            LOG_INFO("Win32 clip cursor hooks installed");
        else
            LOG_ERROR("Win32 clip cursor hook installation failed result:{}", result);
    }

    if (!message2Hooks)
    {
        HookBackend::Begin();

        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_SendInput), hkSendInput);
        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_mouse_event), hkmouse_event);
        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_PostMessageA), hkPostMessageA);
        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_PostMessageW), hkPostMessageW);
        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_SendMessageA), hkSendMessageA);
        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_SendMessageW), hkSendMessageW);

        const HRESULT result = HookBackend::Commit();

        message2Hooks = SUCCEEDED(result);
        if (message2Hooks)
            LOG_INFO("Win32 message hooks installed");
        else
            LOG_ERROR("Win32 message hook installation failed result:{}", result);
    }

#ifdef USE_HID_HOOKS
    if (!State::Instance().isRunningOnLinux && !hidHooks)
    {
        HookBackend::Begin();

        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_CreateFileA), hkCreateFileA);
        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_CreateFileW), hkCreateFileW);
        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_ReadFile), hkReadFile);
        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_DeviceIoControl), hkDeviceIoControl);
        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_CloseHandle), hkCloseHandle);

        const HRESULT result = HookBackend::Commit();

        hidHooks = SUCCEEDED(result);
        if (hidHooks)
            LOG_INFO("Win32 HID hooks installed");
        else
            LOG_ERROR("Win32 HID hook installation failed result:{}", result);
    }
#endif //  USE_HID_HOOKS

    if (!rawHooks)
    {
        HookBackend::Begin();

        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_GetRawInputData), hkGetRawInputData);
        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_GetRawInputBuffer), hkGetRawInputBuffer);
        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_RegisterRawInputDevices), hkRegisterRawInputDevices);

        const HRESULT result = HookBackend::Commit();

        rawHooks = SUCCEEDED(result);
        if (rawHooks)
            LOG_INFO("Win32 raw input hooks installed");
        else
            LOG_ERROR("Win32 raw input installation failed result:{}", result);
    }

    if (!windowsHooks)
    {
        HookBackend::Begin();

        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_SetWindowsHookExA), hkSetWindowsHookExA);
        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_SetWindowsHookExW), hkSetWindowsHookExW);
        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_UnhookWindowsHookEx), hkUnhookWindowsHookEx);

        const HRESULT result = HookBackend::Commit();

        windowsHooks = SUCCEEDED(result);
        if (windowsHooks)
            LOG_INFO("Win32 windows input hooks installed");
        else
            LOG_ERROR("Win32 windows input installation failed result:{}", result);
    }

    if (!positionHooks)
    {
        HookBackend::Begin();

        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_GetCursorPos), hkGetCursorPos);
        HookBackend::Attach(reinterpret_cast<PVOID*>(&o_SetCursorPos), hkSetCursorPos);

        if (o_GetPhysicalCursorPos != nullptr)
            HookBackend::Attach(reinterpret_cast<PVOID*>(&o_GetPhysicalCursorPos), hkGetPhysicalCursorPos);

        if (o_SetPhysicalCursorPos != nullptr)
            HookBackend::Attach(reinterpret_cast<PVOID*>(&o_SetPhysicalCursorPos), hkSetPhysicalCursorPos);

        const HRESULT result = HookBackend::Commit();

        positionHooks = SUCCEEDED(result);
        if (positionHooks)
            LOG_INFO("Win32 position input hooks installed");
        else
            LOG_ERROR("Win32 position input installation failed result:{}", result);
    }

    // If CursorPos hooks are not installed, try to install
    // IAT (Import Address Table) hooks for CursorPos
    if (!positionHooks && !positionIATHooks)
        positionIATHooks = InstallCursorIatHooks();

#ifdef USE_HID_HOOKS
    const bool hidReady = State::Instance().isRunningOnLinux || hidHooks;
    _state.HooksInstalled = messageHooks && keyStateHooks && getPosHooks && clipCursorHooks && message2Hooks &&
                            hidReady && rawHooks && windowsHooks && (positionHooks || positionIATHooks);
#else
    _state.HooksInstalled = messageHooks && keyStateHooks && getPosHooks && clipCursorHooks && message2Hooks &&
                            rawHooks && windowsHooks && (positionHooks || positionIATHooks);
#endif // USE_HID_HOOKS

    return _state.HooksInstalled;
}

bool RemoveHooks()
{
    const bool hasDetourHooks = messageHooks || keyStateHooks || getPosHooks || clipCursorHooks || message2Hooks ||
                                hidHooks || rawHooks || windowsHooks || positionHooks;

    if (!hasDetourHooks && !positionIATHooks)
    {
        _state.HooksInstalled = false;
        return true;
    }

    LOG_INFO("removing Win32 input hooks");

    HRESULT result = S_OK;

    if (hasDetourHooks)
    {
        HookBackend::Begin();

        if (messageHooks)
        {
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_PeekMessageA), hkPeekMessageA);
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_PeekMessageW), hkPeekMessageW);
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_GetMessageA), hkGetMessageA);
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_GetMessageW), hkGetMessageW);
        }

        if (keyStateHooks)
        {
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_GetAsyncKeyState), hkGetAsyncKeyState);
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_GetKeyState), hkGetKeyState);
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_GetKeyboardState), hkGetKeyboardState);
        }

        if (getPosHooks)
        {
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_GetMessagePos), hkGetMessagePos);
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_GetMouseMovePointsEx), hkGetMouseMovePointsEx);
        }

        if (clipCursorHooks)
        {
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_ClipCursor), hkClipCursor);
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_GetClipCursor), hkGetClipCursor);
        }

        if (message2Hooks)
        {
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_SendInput), hkSendInput);
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_mouse_event), hkmouse_event);
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_PostMessageA), hkPostMessageA);
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_PostMessageW), hkPostMessageW);
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_SendMessageA), hkSendMessageA);
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_SendMessageW), hkSendMessageW);
        }

        if (hidHooks)
        {
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_CreateFileA), hkCreateFileA);
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_CreateFileW), hkCreateFileW);
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_ReadFile), hkReadFile);
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_DeviceIoControl), hkDeviceIoControl);
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_CloseHandle), hkCloseHandle);
        }

        if (rawHooks)
        {
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_GetRawInputData), hkGetRawInputData);
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_GetRawInputBuffer), hkGetRawInputBuffer);
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_RegisterRawInputDevices), hkRegisterRawInputDevices);
        }

        if (windowsHooks)
        {
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_SetWindowsHookExA), hkSetWindowsHookExA);
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_SetWindowsHookExW), hkSetWindowsHookExW);
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_UnhookWindowsHookEx), hkUnhookWindowsHookEx);
        }

        if (positionHooks)
        {
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_GetCursorPos), hkGetCursorPos);
            HookBackend::Detach(reinterpret_cast<PVOID*>(&o_SetCursorPos), hkSetCursorPos);

            if (o_GetPhysicalCursorPos != nullptr)
                HookBackend::Detach(reinterpret_cast<PVOID*>(&o_GetPhysicalCursorPos), hkGetPhysicalCursorPos);

            if (o_SetPhysicalCursorPos != nullptr)
                HookBackend::Detach(reinterpret_cast<PVOID*>(&o_SetPhysicalCursorPos), hkSetPhysicalCursorPos);
        }

        result = HookBackend::Commit();

        if (SUCCEEDED(result))
        {
            messageHooks = false;
            keyStateHooks = false;
            getPosHooks = false;
            clipCursorHooks = false;
            message2Hooks = false;
            hidHooks = false;
            rawHooks = false;
            windowsHooks = false;
            positionHooks = false;
        }
        else
        {
            LOG_WARN("Win32 input hook removal failed result:{}; retaining hook state for a safe retry", result);
        }
    }

    if (positionIATHooks)
    {
        RemoveCursorIatHooks();
        positionIATHooks = false;
    }

    const bool hidReady = State::Instance().isRunningOnLinux || hidHooks;
    _state.HooksInstalled = messageHooks && keyStateHooks && getPosHooks && clipCursorHooks && message2Hooks &&
                            hidReady && rawHooks && windowsHooks && (positionHooks || positionIATHooks);

    return SUCCEEDED(result);
}

} // namespace OptiInput
