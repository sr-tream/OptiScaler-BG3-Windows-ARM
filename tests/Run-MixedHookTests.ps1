param([string]$VisualStudio = 'C:\Program Files\Microsoft Visual Studio\18\Community')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$out = Join-Path $repo 'ARM64EC\HookTests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$toolchain = Get-ChildItem -LiteralPath "$VisualStudio\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1
$sdk = Get-ChildItem -LiteralPath 'C:\Program Files (x86)\Windows Kits\10\Lib' -Directory | Sort-Object Name -Descending | Select-Object -First 1
$env:INCLUDE = "$($toolchain.FullName)\include;C:\Program Files (x86)\Windows Kits\10\Include\$($sdk.Name)\ucrt;C:\Program Files (x86)\Windows Kits\10\Include\$($sdk.Name)\shared;C:\Program Files (x86)\Windows Kits\10\Include\$($sdk.Name)\um"
Push-Location $out
try {
    $env:LIB = "$($toolchain.FullName)\lib\arm64ec;$($toolchain.FullName)\lib\arm64;C:\Program Files (x86)\Windows Kits\10\Lib\$($sdk.Name)\ucrt\arm64;C:\Program Files (x86)\Windows Kits\10\Lib\$($sdk.Name)\um\arm64;$repo\external\SlimDetours\Lib\ARM64EC\Release"
    & "$($toolchain.FullName)\bin\Hostarm64\arm64\cl.exe" /nologo /arm64EC /D_ARM64EC_ /D_AMD64_ /std:c++20 /O2 /MD /EHsc /LD "/I$repo\OptiScaler" "/I$repo\external\SlimDetours\Include" "$PSScriptRoot\HookBackendMixed.cpp" /link /MACHINE:ARM64EC /OUT:HookBackendMixed.dll ole32.lib
    if ($LASTEXITCODE) { throw "ARM64EC test DLL compilation failed: $LASTEXITCODE" }
    $env:LIB = "$($toolchain.FullName)\lib\x64;C:\Program Files (x86)\Windows Kits\10\Lib\$($sdk.Name)\ucrt\x64;C:\Program Files (x86)\Windows Kits\10\Lib\$($sdk.Name)\um\x64"
    & "$($toolchain.FullName)\bin\Hostarm64\x64\cl.exe" /nologo /std:c++20 /O2 /MD /EHsc "$PSScriptRoot\HookBackendLauncher.cpp" /link /OUT:HookBackendLauncher.exe
    if ($LASTEXITCODE) { throw "x64 test launcher compilation failed: $LASTEXITCODE" }
    & '.\HookBackendLauncher.exe'
    if ($LASTEXITCODE) { throw "Mixed hook runtime test failed: $LASTEXITCODE" }
} finally { Pop-Location }
