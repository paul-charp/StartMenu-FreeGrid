# Builds Release x64 into build\. -Version x.y.z overrides src\common\version.h.
# -Installer also builds the Inno Setup installer (fails if ISCC is missing).
param(
    [string]$Version,
    [switch]$Installer
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$out = Join-Path $root 'build'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = $null
if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
}
if (-not $vs) { throw 'Visual Studio with C++ tools not found' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

foreach ($d in '', 'dll', 'exe') { New-Item -ItemType Directory -Force (Join-Path $out $d) | Out-Null }

$override = Join-Path $out 'version_override.h'
Remove-Item $override -ErrorAction SilentlyContinue
$defs = ''
if ($Version) {
    if ($Version -notmatch '^(\d+)\.(\d+)\.(\d+)$') { throw "bad version '$Version'" }
    @(
        "#define STARTGRID_VER_MAJOR $($Matches[1])"
        "#define STARTGRID_VER_MINOR $($Matches[2])"
        "#define STARTGRID_VER_PATCH $($Matches[3])"
        "#define STARTGRID_VERSION_TEXT `"$Version`""
        "#define STARTGRID_VERSION_STR L`"$Version`""
    ) | Set-Content $override -Encoding ascii
    $defs = '/DSTARTGRID_VERSION_OVERRIDE /Ibuild'
}

$flags = "/nologo /std:c++20 /utf-8 /EHsc /MT /O2 /W3 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN $defs"
$rc = "rc /nologo $defs"
$steps = @(
    "$rc /Fobuild\dll\StartGrid.res src\dll\StartGrid.rc",
    "$rc /Fobuild\exe\StartGrid.res src\exe\StartGrid.rc",
    "cl $flags /LD src\dll\StartGrid.cpp src\dll\host.cpp build\dll\StartGrid.res /Fobuild\dll\ /Fe:build\StartGrid.dll /link /DEF:src\dll\StartGrid.def runtimeobject.lib user32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib",
    "cl $flags src\exe\main.cpp src\exe\ui.cpp build\exe\StartGrid.res /Fobuild\exe\ /Fe:build\StartGrid.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:NO runtimeobject.lib advapi32.lib shell32.lib user32.lib ole32.lib oleaut32.lib dwmapi.lib gdi32.lib"
)
cmd /c ('"{0}" >nul && cd /d "{1}" && {2}' -f $vcvars, $root, ($steps -join ' && '))
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

if ($Installer) {
    if (-not $Version) {
        $Version = [regex]::Match((Get-Content (Join-Path $root 'src\common\version.h') -Raw), 'STARTGRID_VERSION_TEXT "([^"]+)"').Groups[1].Value
    }
    $iscc = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
        (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) {
        $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
        if ($cmd) { $iscc = $cmd.Source }
    }
    if ($iscc) {
        & $iscc /Qp "/DAppVersion=$Version" (Join-Path $root 'installer\StartMenu-FreeGrid.iss')
        if ($LASTEXITCODE -ne 0) { throw 'installer failed' }
    } else {
        throw 'Inno Setup not found'
    }
}
