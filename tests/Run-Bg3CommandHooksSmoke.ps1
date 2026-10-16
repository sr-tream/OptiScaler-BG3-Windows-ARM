param([Parameter(Mandatory=$true)][string]$PackageDirectory,[switch]$StaticFsr2,[switch]$AutoExposure)
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$package=(Resolve-Path -LiteralPath $PackageDirectory).Path
$vc='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231'
$sdk='C:\Program Files (x86)\Windows Kits\10'
$env:INCLUDE="$vc\include;$sdk\Include\10.0.26100.0\ucrt;$sdk\Include\10.0.26100.0\shared;$sdk\Include\10.0.26100.0\um"
$out=Join-Path $repo 'ARM64EC\CommandHookTests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
Push-Location $out
try {
    $testFlags=@()
    $testLibs=@()
    if ($StaticFsr2) {
        $testFlags=@('/DTEST_STATIC_FSR')
        $testLibs=@("$repo\OptiScaler\library\fsr2\ffx_fsr2_api_x64.lib","$repo\OptiScaler\library\fsr2\ffx_fsr2_api_vk_x64.lib","$repo\OptiScaler\library\fsr2_212\ffx_fsr2_212_api_x64.lib","$repo\OptiScaler\library\fsr2_212\ffx_fsr2_212_api_vk_x64.lib")
    }
    if ($AutoExposure) {$testFlags+=@('/DTEST_AUTO_EXPOSURE')}
    $env:LIB="$vc\lib\arm64ec;$vc\lib\arm64;$sdk\Lib\10.0.26100.0\ucrt\arm64;$sdk\Lib\10.0.26100.0\um\arm64;$repo\external\SlimDetours\Lib\ARM64EC\Release"
    & "$vc\bin\Hostarm64\arm64\cl.exe" /nologo /arm64EC /D_ARM64EC_ /D_AMD64_ /std:c++20 /O2 /MD /EHsc /LD @testFlags "/I$repo\OptiScaler" "/I$repo\OptiScaler\include" "/I$repo\external\SlimDetours\Include" "/I$repo\external\vulkan\include" "/I$repo\external\FidelityFX-SDK\ffx-api\include\ffx_api" "$PSScriptRoot\Bg3CommandHooksSmoke.cpp" /link /MACHINE:ARM64EC /OUT:Bg3CommandHooksSmoke.dll "$repo\OptiScaler\library\vulkan\vulkan-1.lib" @testLibs
    if ($LASTEXITCODE) {throw 'ARM64EC command hook DLL compilation failed'}
    $env:LIB="$vc\lib\x64;$sdk\Lib\10.0.26100.0\ucrt\x64;$sdk\Lib\10.0.26100.0\um\x64"
    & "$vc\bin\Hostarm64\x64\cl.exe" /nologo /std:c++20 /O2 /MD /EHsc "$PSScriptRoot\Bg3CommandHooksLauncher.cpp" /link /OUT:Bg3CommandHooksLauncher.exe
    if ($LASTEXITCODE) {throw 'Command hook launcher compilation failed'}
    & '.\Bg3CommandHooksLauncher.exe' "$out\Bg3CommandHooksSmoke.dll" "$package\OptiScaler\amd_fidelityfx_vk.dll" *> "$out\result.log"
    $result=$LASTEXITCODE
    Get-Content -LiteralPath "$out\result.log"
    if ($result) {throw "Command hook GPU test failed: $result"}
} finally {Pop-Location}
