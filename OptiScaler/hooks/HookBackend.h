#pragma once

#include <KNSoft/SlimDetours/SlimDetours.h>

#pragma comment(lib, "KNSoft.SlimDetours.lib")
#pragma comment(lib, "ntdll.lib")

namespace HookBackend
{
// Keep the first error: existing hook batches often only check Commit().
inline thread_local HRESULT transactionResult = E_UNEXPECTED;
inline thread_local bool transactionActive = false;

inline HRESULT Begin()
{
    if (transactionActive)
        return transactionResult = E_UNEXPECTED;
    transactionResult = SlimDetoursTransactionBegin();
    transactionActive = SUCCEEDED(transactionResult);
    return transactionResult;
}

inline HRESULT Record(HRESULT result)
{
    if (FAILED(result) && SUCCEEDED(transactionResult))
        transactionResult = result;
    return result;
}

template <typename T> inline HRESULT Attach(PVOID* original, T replacement)
{
    if (!transactionActive)
        return FAILED(transactionResult) ? transactionResult : E_UNEXPECTED;
    if (FAILED(transactionResult))
        return transactionResult;
    if (!original || !*original || !replacement)
        return Record(E_INVALIDARG);
    return Record(SlimDetoursAttach(original, reinterpret_cast<PVOID>(replacement)));
}

template <typename T> inline HRESULT Detach(PVOID* original, T replacement)
{
    if (!transactionActive)
        return FAILED(transactionResult) ? transactionResult : E_UNEXPECTED;
    if (FAILED(transactionResult))
        return transactionResult;
    if (!original || !*original || !replacement)
        return Record(E_INVALIDARG);
    return Record(SlimDetoursDetach(original, reinterpret_cast<PVOID>(replacement)));
}

inline HRESULT Abort()
{
    if (!transactionActive)
        return transactionResult;
    transactionActive = false;
    return SlimDetoursTransactionAbort();
}

inline HRESULT Commit()
{
    if (!transactionActive)
        return transactionResult;
    transactionActive = false;
    if (FAILED(transactionResult))
    {
        SlimDetoursTransactionAbort();
        return transactionResult;
    }
    return transactionResult = SlimDetoursTransactionCommit();
}

// These callers only look up exported functions; no debug-symbol lookup is needed.
inline PVOID FindFunction(LPCSTR moduleName, LPCSTR functionName)
{
    auto module = GetModuleHandleA(moduleName);
    if (!module)
        module = LoadLibraryA(moduleName);
    return module ? reinterpret_cast<PVOID>(GetProcAddress(module, functionName)) : nullptr;
}

using ExportCallback = BOOL(WINAPI*)(PVOID, ULONG, LPCSTR, PVOID);
BOOL EnumerateExports(HMODULE module, PVOID context, ExportCallback callback);
} // namespace HookBackend
