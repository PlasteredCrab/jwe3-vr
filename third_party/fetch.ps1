# Fetches the third-party code the build needs into third_party\ (not committed):
#   minhook   - https://github.com/TsudaKageyu/minhook (BSD-2)
#   openxr    - Khronos OpenXR loader SDK for Windows (Apache-2.0)
#   lua       - Lua 5.3.6 source (MIT); test.ps1 builds luac from it to syntax-check the pack
#   powershell -ExecutionPolicy Bypass -File third_party/fetch.ps1
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$openxrVersion = '1.1.65'

if (-not (Test-Path (Join-Path $here 'minhook'))) {
    git clone --depth 1 https://github.com/TsudaKageyu/minhook.git (Join-Path $here 'minhook')
}
$oxr = Join-Path $here 'openxr'
if (-not (Test-Path (Join-Path $oxr 'include\openxr\openxr.h'))) {
    $zip = Join-Path $env:TEMP "openxr_loader_windows-$openxrVersion.zip"
    Invoke-WebRequest "https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-$openxrVersion/openxr_loader_windows-$openxrVersion.zip" -OutFile $zip
    Expand-Archive $zip $oxr -Force
    Remove-Item $zip
}
$lua = Join-Path $here 'lua'
if (-not (Test-Path (Join-Path $lua 'src\luac.c'))) {
    $tgz = Join-Path $env:TEMP 'lua-5.3.6.tar.gz'
    Invoke-WebRequest 'https://www.lua.org/ftp/lua-5.3.6.tar.gz' -OutFile $tgz
    # Windows' own tar: a GNU tar earlier on PATH (Git Bash) reads "C:" as a remote host.
    & (Join-Path $env:SystemRoot 'System32\tar.exe') -xzf $tgz -C $here
    if ($LASTEXITCODE -ne 0) { throw 'extracting Lua failed' }
    Rename-Item (Join-Path $here 'lua-5.3.6') 'lua'
    Remove-Item $tgz
}
Write-Host 'third_party ready'
