#pragma once

#include <Windows.h>

enum class ExportProxy
{
    Version,
    WinMM,
    WinINet,
    DbgHelp,
    WinHTTP,
    DXGI,
    D3D12
};

void InitializeExportProxy(ExportProxy proxy, HMODULE module);
