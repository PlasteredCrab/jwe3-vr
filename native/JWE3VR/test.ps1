# Builds and runs the offline checks (rig math; the Lua bridge against a fake lua_State; Lua 5.3
# syntax of the content pack).
# No game or headset needed. Build the DLL first (build.ps1).
#   powershell -ExecutionPolicy Bypass -File native/JWE3VR/test.ps1
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$out = Join-Path $here 'build\test'
New-Item -ItemType Directory -Force $out | Out-Null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
# Compiler and linker temp files next to the build, not in %TEMP%.
$env:TMP = $env:TEMP = Join-Path $out 'tmp'
New-Item -ItemType Directory -Force $env:TMP | Out-Null

cmd /c "`"$vcvars`" >nul 2>nul && cl /nologo /std:c++20 /O2 /EHsc /Fo`"$out\\`" /Fe`"$out\math_test.exe`" `"$here\test\math_test.cpp`""
if ($LASTEXITCODE -ne 0) { throw 'math test build failed' }
& "$out\math_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'math checks failed' }

$dll = Join-Path $here 'build\JWE3VR.dll'
if (-not (Test-Path $dll)) { throw "build the DLL first: $dll" }
# Run against a copy so the test's JWE3VR.ini/.log stay out of the build folder.
$tdll = Join-Path $out 'JWE3VR.dll'
Copy-Item $dll $tdll -Force
Copy-Item (Join-Path $here 'build\openxr_loader.dll') $out -Force
cmd /c "`"$vcvars`" >nul 2>nul && cl /nologo /std:c++20 /O2 /EHsc /Fo`"$out\\`" /Fe`"$out\bridge_test.exe`" `"$here\test\bridge_test.cpp`""
if ($LASTEXITCODE -ne 0) { throw 'bridge test build failed' }
& "$out\bridge_test.exe" $tdll
if ($LASTEXITCODE -ne 0) { throw 'bridge checks failed' }

# Lua 5.3 syntax check of the content pack (luac built once from third_party\lua).
$luaSrc = Join-Path (Split-Path -Parent (Split-Path -Parent $here)) 'third_party\lua\src'
$luac = Join-Path $out 'luac.exe'
if (-not (Test-Path $luac)) {
    if (-not (Test-Path (Join-Path $luaSrc 'luac.c'))) { throw "missing Lua source in $luaSrc (run third_party/fetch.ps1)" }
    $luaObj = Join-Path $out 'obj_lua'
    New-Item -ItemType Directory -Force $luaObj | Out-Null
    $luaFiles = Get-ChildItem $luaSrc -Filter '*.c' | Where-Object { $_.Name -ne 'lua.c' } | ForEach-Object { "`"$($_.FullName)`"" }
    cmd /c "`"$vcvars`" >nul 2>nul && cl /nologo /O2 /W0 /Fo`"$luaObj\\`" /Fe`"$luac`" $($luaFiles -join ' ')"
    if ($LASTEXITCODE -ne 0) { throw 'luac build failed' }
}
$pack = Join-Path (Split-Path -Parent (Split-Path -Parent $here)) 'pack\JWE3VR\Main'
$luaFails = 0
foreach ($lua in Get-ChildItem $pack -Filter '*.lua') {
    & $luac -p $lua.FullName
    if ($LASTEXITCODE -ne 0) { $luaFails++; continue }
    # Globals: the pack reaches everything through _G ("local global = _G") and module(); any other global is
    # a local used before its declaration or a typo, which only fails at run time in the game.
    $globals = & $luac -p -l -l $lua.FullName | Select-String -Pattern '[GS]ETTABUP.*_ENV "([A-Za-z_][A-Za-z0-9_]*)"' |
        ForEach-Object { $_.Matches[0].Groups[1].Value } | Where-Object { $_ -notin @('_G', 'module') } | Sort-Object -Unique
    if ($globals) { Write-Host "$($lua.Name): unexpected globals: $($globals -join ', ')"; $luaFails++ }
}
if ($luaFails) { throw "$luaFails pack Lua file(s) failed the syntax / globals check" }
Write-Host "pack Lua syntax ok ($((Get-ChildItem $pack -Filter '*.lua').Count) files)"
