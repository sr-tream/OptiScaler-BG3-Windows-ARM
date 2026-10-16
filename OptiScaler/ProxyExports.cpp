#include "pch.h"

#include "State.h"
#include "exports/ProxyExports.h"
#include "exports/exports.h"

// Untyped forwarding exports must keep the x64 ABI, including return registers.
// ARM64EC builds compile this file as x64 and link it into the hybrid DLL.
void InitializeExportProxy(ExportProxy proxy, HMODULE module)
{
    if (proxy != ExportProxy::DXGI && proxy != ExportProxy::D3D12)
        shared.LoadOriginalLibrary(module);
    switch (proxy)
    {
    case ExportProxy::Version: version.LoadOriginalLibrary(module); break;
    case ExportProxy::WinMM: winmm.LoadOriginalLibrary(module); break;
    case ExportProxy::WinINet: wininet.LoadOriginalLibrary(module); break;
    case ExportProxy::DbgHelp: dbghelp.LoadOriginalLibrary(module); break;
    case ExportProxy::WinHTTP: winhttp.LoadOriginalLibrary(module); break;
    case ExportProxy::DXGI: dxgi.LoadOriginalLibrary(module); break;
    case ExportProxy::D3D12: d3d12.LoadOriginalLibrary(module); break;
    }
}
