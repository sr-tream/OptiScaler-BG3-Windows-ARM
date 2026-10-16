#include "pch.h"

#include "HookBackend.h"
#include <cstring>
#include <vector>

BOOL HookBackend::EnumerateExports(HMODULE module, PVOID context, ExportCallback callback)
{
    if (!callback)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!module)
        module = GetModuleHandleW(nullptr);

    const auto base = reinterpret_cast<const BYTE*>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
    {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return FALSE;
    }
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC)
    {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return FALSE;
    }
    const auto imageSize = nt->OptionalHeader.SizeOfImage;
    const auto valid = [imageSize](DWORD rva, size_t bytes) {
        return rva != 0 && rva < imageSize && bytes <= imageSize - rva;
    };
    const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress)
    {
        SetLastError(NO_ERROR);
        return TRUE;
    }
    const auto invalid = [] {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return FALSE;
    };
    if (!valid(directory.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY)))
        return invalid();
    const auto exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
    if (!valid(exports->AddressOfFunctions, size_t(exports->NumberOfFunctions) * sizeof(DWORD)) ||
        (exports->NumberOfNames &&
         (!valid(exports->AddressOfNames, size_t(exports->NumberOfNames) * sizeof(DWORD)) ||
          !valid(exports->AddressOfNameOrdinals, size_t(exports->NumberOfNames) * sizeof(WORD)))))
        return invalid();

    const auto functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
    const auto names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    const auto ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
    std::vector<LPCSTR> functionNames(exports->NumberOfFunctions, nullptr);
    for (DWORD i = 0; i < exports->NumberOfNames; ++i)
    {
        if (ordinals[i] >= exports->NumberOfFunctions || !valid(names[i], 1) ||
            !std::memchr(base + names[i], '\0', imageSize - names[i]))
            return invalid();
        functionNames[ordinals[i]] = reinterpret_cast<LPCSTR>(base + names[i]);
    }
    for (DWORD i = 0; i < exports->NumberOfFunctions; ++i)
    {
        if (!functions[i])
            continue;
        if (!valid(functions[i], 1))
            return invalid();
        auto name = functionNames[i];
        const auto ordinal = exports->Base + i;
        // Resolve forwarded exports too, rather than treating their strings as code.
        PVOID address = nullptr;
        if (functions[i] >= directory.VirtualAddress &&
            functions[i] - directory.VirtualAddress < directory.Size)
        {
            if (name)
                address = reinterpret_cast<PVOID>(GetProcAddress(module, name));
            else if (ordinal <= 0xffff)
                address = reinterpret_cast<PVOID>(GetProcAddress(module, MAKEINTRESOURCEA(ordinal)));
        }
        else
            address = const_cast<BYTE*>(base + functions[i]);
        if (address && !callback(context, ordinal, name, address))
            break;
    }
    SetLastError(NO_ERROR);
    return TRUE;
}
