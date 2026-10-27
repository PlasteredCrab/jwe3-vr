# Builds the JWE3 VR mod with the installed Visual Studio (MSVC x64):
#   native\JWE3VR\build\JWE3VR.dll     the mod (hooks, OpenXR, rig, Lua bridge)
#   native\JWE3VR\build\version.dll    the loader proxy that goes next to JWE3.exe
# Needs third_party\minhook (source) and third_party\openxr (Khronos loader SDK).
#   powershell -ExecutionPolicy Bypass -File native/JWE3VR/build.ps1
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$root = Split-Path -Parent (Split-Path -Parent $here)
$mh = Join-Path $root 'third_party\minhook'
$oxr = Join-Path $root 'third_party\openxr'
if (-not (Test-Path (Join-Path $mh 'include\MinHook.h'))) { throw "missing MinHook in $mh" }
if (-not (Test-Path (Join-Path $oxr 'include\openxr\openxr.h'))) { throw "missing OpenXR SDK in $oxr" }

$out = Join-Path $here 'build'
$obj = Join-Path $out 'obj'
$objLoader = Join-Path $out 'obj_loader'
foreach ($d in @($out, $obj, $objLoader)) { New-Item -ItemType Directory -Force $d | Out-Null }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

function Invoke-VC([string]$command) {
    cmd /c "`"$vcvars`" >nul && $command"
    if ($LASTEXITCODE -ne 0) { throw "build step failed: $command" }
}

$mhSources = @('src\buffer.c', 'src\hook.c', 'src\trampoline.c', 'src\hde\hde64.c') | ForEach-Object { "`"$(Join-Path $mh $_)`"" }
$sources = Get-ChildItem (Join-Path $here 'src') -Filter '*.cpp' | Sort-Object Name | ForEach-Object { "`"$($_.FullName)`"" }
$oxrLib = Get-ChildItem (Join-Path $oxr 'x64') -Recurse -Filter 'openxr_loader.lib' | Select-Object -First 1
if (-not $oxrLib) { throw "openxr_loader.lib not found under $oxr\x64" }

# /EHa: SEH-guarded reads of game memory (view CB, Lua stack) become caught failures, not crashes.
$flags = "/nologo /std:c++20 /O2 /MT /W3 /EHa /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I`"$mh\include`" /I`"$oxr\include`" /I`"$here\src`""
Invoke-VC "cl $flags /Fo`"$obj\\`" /LD /Fe`"$out\JWE3VR.dll`" $($sources -join ' ') $($mhSources -join ' ') /link /NOLOGO /DELAYLOAD:openxr_loader.dll delayimp.lib `"$($oxrLib.FullName)`" user32.lib dxguid.lib dbghelp.lib"
Copy-Item (Join-Path (Split-Path $oxrLib.FullName -Parent) '..\bin\openxr_loader.dll') (Join-Path $out 'openxr_loader.dll') -Force -ErrorAction SilentlyContinue
if (-not (Test-Path (Join-Path $out 'openxr_loader.dll'))) {
    $oxrDll = Get-ChildItem (Join-Path $oxr 'x64') -Recurse -Filter 'openxr_loader.dll' | Select-Object -First 1
    Copy-Item $oxrDll.FullName (Join-Path $out 'openxr_loader.dll') -Force
}
Write-Host "built $out\JWE3VR.dll ($($sources.Count) sources)"

$loader = Join-Path (Split-Path -Parent $here) 'loader'
Invoke-VC "cl /nologo /O2 /MT /W3 /Fo`"$objLoader\\`" /LD /Fe`"$out\version.dll`" `"$loader\version.cpp`" /link /NOLOGO /DEF:`"$loader\version.def`""
Write-Host "built $out\version.dll"
