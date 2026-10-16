$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$vc='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231'
$sdk='C:\Program Files (x86)\Windows Kits\10'
$env:INCLUDE="$vc\include;$sdk\Include\10.0.26100.0\ucrt;$sdk\Include\10.0.26100.0\shared;$sdk\Include\10.0.26100.0\um"
$env:LIB="$vc\lib\x64;$sdk\Lib\10.0.26100.0\ucrt\x64;$sdk\Lib\10.0.26100.0\um\x64"
$out=Join-Path $repo 'ARM64EC\VulkanCommandsTests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
& "$vc\bin\Hostarm64\x64\cl.exe" /nologo /std:c++20 /EHsc /O2 /MD "/I$repo\external\vulkan\include" "/I$repo\OptiScaler" "/Fo$out\Commands.obj" "$PSScriptRoot\Bg3VulkanCommandsTest.cpp" /link "/OUT:$out\Commands.exe"
if ($LASTEXITCODE) {throw 'Command transaction test compilation failed'}
& "$out\Commands.exe" *> "$out\result.log"
$result=$LASTEXITCODE
Get-Content -LiteralPath "$out\result.log"
if ($result) {throw "Command transaction test failed: $result"}
