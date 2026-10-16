param(
    [string]$VisualStudio,
    [string]$Toolset = 'v145'
)
$ErrorActionPreference = 'Stop'
if (!$VisualStudio) {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    $VisualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64EC -property installationPath
    if (!$VisualStudio) { throw 'Visual Studio with ARM64EC C++ tools was not found.' }
}
$hostArchitecture = if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64') { 'arm64' } else { 'amd64' }
$preferred = if ($hostArchitecture -eq 'arm64') { 'arm64' } else { 'x64' }
$msbuild = Join-Path $VisualStudio "MSBuild\Current\Bin\$hostArchitecture\MSBuild.exe"
& $msbuild "$PSScriptRoot\OptiScaler.sln" /m:1 /p:Configuration=Release /p:Platform=ARM64EC "/p:PlatformToolset=$Toolset" "/p:PreferredToolArchitecture=$preferred" /p:CL_MPCount=2 /p:PostBuildEventUseInBuild=false /v:minimal /nologo
if ($LASTEXITCODE) { throw "OptiScaler build failed: $LASTEXITCODE" }
Write-Output "Built: $PSScriptRoot\ARM64EC\Release\OptiScaler.dll"
