# Installs the JWE3 VR mod into the game:
#   <game>\version.dll                                  loader proxy (loads JWE3VR.dll at start)
#   <game>\Win64\ovldata\JWE3VR\JWE3VR.dll              the mod
#   <game>\Win64\ovldata\JWE3VR\openxr_loader.dll
#   <game>\Win64\ovldata\JWE3VR\Manifest.xml, Main.ovl  content pack (the Lua side; no ACSE needed)
# JWE3VR.ini and JWE3VR.log are created next to JWE3VR.dll on first run (the ini is kept).
# -NoProxy installs only the content pack: the game's Lua then loads JWE3VR.dll itself (package.loadlib)
# when content-pack databases run at startup. Use it where endpoint protection flags a version.dll placed
# next to the exe (a DLL-hijacking pattern). Without the proxy the mod's hooks go in a little later, so
# the present queue comes from the "last direct queue" fallback.
# Close the game first (the DLLs are locked while it runs).
#   powershell -ExecutionPolicy Bypass -File deploy.ps1 [-Build] [-NoProxy] [-Uninstall] [-Package]
param(
    [switch]$Build,
    [switch]$NoProxy,
    [switch]$Uninstall,
    [switch]$Package,
    [string]$GameDir = 'E:\SteamLibrary\steamapps\common\Jurassic World Evolution 3',
    [string]$CobraTools = 'E:\Modding\tools\cobra-tools'
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$packDir = Join-Path $GameDir 'Win64\ovldata\JWE3VR'
$proxy = Join-Path $GameDir 'version.dll'

if (Get-Process -Name 'JWE3' -ErrorAction SilentlyContinue) { throw 'Jurassic World Evolution 3 is running; close it first.' }

if ($Uninstall) {
    if (Test-Path $proxy) { Remove-Item $proxy; Write-Host "removed $proxy" }
    foreach ($f in 'JWE3VR.dll', 'openxr_loader.dll', 'Manifest.xml', 'Main.ovl') {
        $p = Join-Path $packDir $f
        if (Test-Path $p) { Remove-Item $p; Write-Host "removed $p" }
    }
    Write-Host "kept $packDir\JWE3VR.ini and the log (delete the folder to remove them)"
    return
}

$native = Join-Path $here 'native\JWE3VR'
if ($Build) {
    & (Join-Path $native 'build.ps1')
    & (Join-Path $native 'test.ps1')
}
$bin = Join-Path $native 'build'
foreach ($f in 'JWE3VR.dll', 'openxr_loader.dll', 'version.dll') {
    if (-not (Test-Path (Join-Path $bin $f))) { throw "build first: $bin\$f is missing (use -Build)" }
}

# Pack the Lua side into Main.ovl with cobra-tools.
$dist = Join-Path $here 'dist\JWE3VR'
New-Item -ItemType Directory -Force $dist | Out-Null
$py = Join-Path $CobraTools '.venv\Scripts\python.exe'
if (-not (Test-Path $py)) { throw "cobra-tools venv not found at $CobraTools (see re\lua\README.md)" }
foreach ($lua in Get-ChildItem (Join-Path $here 'pack\JWE3VR\Main') -Filter '*.lua') {
    & (Join-Path $CobraTools 'modules\formats\utils\luadec\luac.exe') -p $lua.FullName
    if ($LASTEXITCODE -ne 0) { throw "Lua syntax error in $($lua.Name)" }
}
Push-Location $CobraTools
try {
    & $py ovl_tool_cmd.py new -i (Join-Path $here 'pack\JWE3VR\Main') -g 'Jurassic World Evolution 3' -o (Join-Path $dist 'Main.ovl') --force *> $null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $dist 'Main.ovl'))) { throw 'packing Main.ovl failed' }
} finally { Pop-Location }
Copy-Item (Join-Path $here 'pack\JWE3VR\Manifest.xml') $dist -Force
foreach ($f in 'JWE3VR.dll', 'openxr_loader.dll') { Copy-Item (Join-Path $bin $f) $dist -Force }
Write-Host "packed $dist"

if ($Package) {
    $stage = Join-Path $here 'dist\release'
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    $ovl = Join-Path $stage 'Win64\ovldata\JWE3VR'
    New-Item -ItemType Directory -Force $ovl | Out-Null
    Copy-Item (Join-Path $dist '*') $ovl -Force
    Copy-Item (Join-Path $bin 'version.dll') $stage -Force
    Copy-Item (Join-Path $here 'README.md') $stage -Force
    $zip = Join-Path $here 'dist\JWE3VR.zip'
    if (Test-Path $zip) { Remove-Item $zip }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
    Write-Host "release zip: $zip (unzip into the game folder)"
    return
}

if (-not (Test-Path (Join-Path $GameDir 'JWE3.exe'))) { throw "JWE3.exe not found in $GameDir (use -GameDir)" }
New-Item -ItemType Directory -Force $packDir | Out-Null
Copy-Item (Join-Path $dist '*') $packDir -Force
if ($NoProxy) {
    if (Test-Path $proxy) { Remove-Item $proxy; Write-Host "removed $proxy (-NoProxy)" }
    Write-Host 'no version.dll proxy: the game Lua loads JWE3VR.dll at startup'
} else {
    Copy-Item (Join-Path $bin 'version.dll') $proxy -Force
    Write-Host "installed $proxy"
}
Write-Host "installed $packDir (JWE3VR.dll, openxr_loader.dll, Manifest.xml, Main.ovl)"
Write-Host "settings: $packDir\JWE3VR.ini   log: $packDir\JWE3VR.log"
