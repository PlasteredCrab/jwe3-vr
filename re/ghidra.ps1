# Headless Ghidra for the JWE3 exe (the project and decompiles stay local; see .gitignore):
#   powershell -ExecutionPolicy Bypass -File re/ghidra.ps1 -Import   new project re\ghidra\JWE3.gpr: Denuvo VM
#                                                                    blob removed, then auto-analysis (hours)
#   powershell -ExecutionPolicy Bypass -File re/ghidra.ps1 -Dump     decompile re\targets.txt into re\decomp\
# The exe defaults to the first JWE3*.exe in the Steam folder; Ghidra runs on Corretto 21 with a big heap.
param(
    [switch]$Import,
    [switch]$Dump,
    [string]$Exe = '',
    [string]$GameDir = 'E:\SteamLibrary\steamapps\common\Jurassic World Evolution 3',
    [string]$Ghidra = 'E:\Ghidra\ghidra_12.0.3_PUBLIC',
    [string]$Jdk = (Join-Path $env:USERPROFILE '.jdks\corretto-21.0.10'),
    [string]$MaxMem = '20G',
    [string]$ProjectDir = (Join-Path $PSScriptRoot 'ghidra'),
    [string]$ProjectName = 'JWE3',
    # Ghidra's settings, cache and temp files (default: under %APPDATA%, %LOCALAPPDATA% and %TEMP% on C:).
    [string]$GhidraHome = 'E:\Modding\tools\ghidra-home'
)
$ErrorActionPreference = 'Stop'
if (-not ($Import -or $Dump)) { throw 'pass -Import and/or -Dump' }
if (-not $Exe) {
    $hit = Get-ChildItem $GameDir -Filter 'JWE3*.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $hit) { throw "no JWE3*.exe in $GameDir (use -Exe)" }
    $Exe = $hit.FullName
}
if (-not (Test-Path (Join-Path $Jdk 'bin\java.exe'))) { throw "JDK not found: $Jdk" }
$launch = Join-Path $Ghidra 'support\launch.bat'
if (-not (Test-Path $launch)) { throw "Ghidra not found: $Ghidra" }
New-Item -ItemType Directory -Force $ProjectDir | Out-Null
$scripts = Join-Path $PSScriptRoot 'ghidra_scripts'
$env:JAVA_HOME = $Jdk
$env:PATH = "$Jdk\bin;$env:PATH"

foreach ($d in 'settings', 'cache', 'tmp') { New-Item -ItemType Directory -Force (Join-Path $GhidraHome $d) | Out-Null }
if ($GhidraHome -match '\s') { throw "GhidraHome must not contain spaces: $GhidraHome" }
$vmArgs = "-Dapplication.settingsdir=$GhidraHome\settings -Dapplication.cachedir=$GhidraHome\cache -Djava.io.tmpdir=$GhidraHome\tmp"

function Invoke-Headless([string[]]$headlessArgs) {
    # Same call analyzeHeadless.bat makes, with our heap size and Ghidra's own files kept under $GhidraHome.
    & cmd /c "`"$launch`" fg jdk Ghidra-Headless $MaxMem `"$vmArgs`" ghidra.app.util.headless.AnalyzeHeadless $($headlessArgs -join ' ')"
    if ($LASTEXITCODE -ne 0) { throw "Ghidra headless failed ($LASTEXITCODE)" }
}

if ($Import) {
    Invoke-Headless @("`"$ProjectDir`"", $ProjectName, '-import', "`"$Exe`"", '-overwrite',
        '-scriptPath', "`"$scripts`"", '-preScript', 'JWE3PrepareImport.java')
}
if ($Dump) {
    Invoke-Headless @("`"$ProjectDir`"", $ProjectName, '-process', "`"$(Split-Path $Exe -Leaf)`"", '-noanalysis',
        '-scriptPath', "`"$scripts`"", '-postScript', 'JWE3DumpTargets.java')
}
