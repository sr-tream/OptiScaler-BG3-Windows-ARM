param([string]$Python='python')
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$out=Join-Path $repo 'ARM64EC\CursorPolicyTests'
& $Python "$PSScriptRoot\Generate-CursorPolicyTests.py"
if ($LASTEXITCODE) {throw 'Cursor fixture generation failed'}
$vc='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231'
$sdk='C:\Program Files (x86)\Windows Kits\10'
$env:INCLUDE="$vc\include;$sdk\Include\10.0.26100.0\ucrt;$sdk\Include\10.0.26100.0\shared;$sdk\Include\10.0.26100.0\um"
$env:LIB="$vc\lib\x64;$sdk\Lib\10.0.26100.0\ucrt\x64;$sdk\Lib\10.0.26100.0\um\x64"
foreach ($name in @('CursorFocusBaseline','CursorFocusProbe')) {
 & "$vc\bin\Hostarm64\x64\cl.exe" /nologo /std:c++20 /EHsc /O2 /MD "/Fo$out\$name.obj" "$out\$name.cpp" /link "/OUT:$out\$name.exe" user32.lib
 if ($LASTEXITCODE) {throw "Cursor test compilation failed: $name"}
 & "$out\$name.exe" *> "$out\$name.log"
 $result=$LASTEXITCODE
 Get-Content -LiteralPath "$out\$name.log"
 if ($result) {throw "Cursor test failed: $name, $result"}
}
