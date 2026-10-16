param(
    [Parameter(Mandatory=$true)][string]$PackageDirectory,
    [string]$VisualStudio = 'C:\Program Files\Microsoft Visual Studio\18\Community'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
$toolchain = Get-ChildItem -LiteralPath "$VisualStudio\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdk = Get-ChildItem -LiteralPath "$sdkRoot\Lib" -Directory | Sort-Object Name -Descending | Select-Object -First 1
$env:INCLUDE = "$($toolchain.FullName)\include;$sdkRoot\Include\$($sdk.Name)\ucrt;$sdkRoot\Include\$($sdk.Name)\shared;$sdkRoot\Include\$($sdk.Name)\um"
$env:LIB = "$($toolchain.FullName)\lib\x64;$sdkRoot\Lib\$($sdk.Name)\ucrt\x64;$sdkRoot\Lib\$($sdk.Name)\um\x64"
$out = Join-Path $repo 'ARM64EC\VulkanOverlayTests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
& "$($toolchain.FullName)\bin\Hostarm64\x64\cl.exe" /nologo /std:c++20 /O2 /MD /EHsc "/I$repo\external\vulkan\include" "/Fo$out\VulkanOverlaySmoke.obj" "$PSScriptRoot\VulkanOverlaySmoke.cpp" /link "/OUT:$out\VulkanOverlaySmoke.exe" "$repo\OptiScaler\library\vulkan\vulkan-1.lib" user32.lib
if ($LASTEXITCODE) { throw 'Vulkan overlay smoke test compilation failed' }
Push-Location $package
try {
    & "$out\VulkanOverlaySmoke.exe" "$package\winmm.dll" *> "$out\result.log"
    $testExit = $LASTEXITCODE
    Get-Content -LiteralPath "$out\result.log"
    if ($testExit) { throw "Vulkan overlay smoke test failed: $testExit" }
    $log = Get-Content -LiteralPath "$package\OptiScaler.log" -Raw
    if ($log -notmatch 'Initialize completed hooksInstalled:1 target:0x[1-9a-fA-F][0-9a-fA-F]*' -or
        $log -notmatch 'subclass installed hwnd:' -or
        $log -match 'hook installation failed') {
        throw 'Vulkan overlay input did not bind to its window or failed hook installation'
    }
    Write-Output 'PASS: instance-dispatched Vulkan surface, correct HWND, window subclass, Win32 input hooks.'
} finally { Pop-Location }
