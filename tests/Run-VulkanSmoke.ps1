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
$out = Join-Path $repo 'ARM64EC\VulkanTests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
& "$($toolchain.FullName)\bin\Hostarm64\x64\cl.exe" /nologo /std:c++20 /O2 /MD /EHsc "/I$repo\OptiScaler" "/I$repo\external\vulkan\include" "/I$repo\external\FidelityFX-SDK\ffx-api\include\ffx_api" "/Fo$out\VulkanSmoke.obj" "$PSScriptRoot\VulkanSmoke.cpp" /link "/OUT:$out\VulkanSmoke.exe" "$repo\OptiScaler\library\vulkan\vulkan-1.lib"
if ($LASTEXITCODE) { throw 'Vulkan smoke test compilation failed' }
& "$out\VulkanSmoke.exe" "$package\OptiScaler\amd_fidelityfx_vk.dll" "$package\winmm.dll" *> "$out\result.log"
$testExit = $LASTEXITCODE
Get-Content -LiteralPath "$out\result.log"
if ($testExit) { throw "Vulkan smoke test failed: $testExit" }
