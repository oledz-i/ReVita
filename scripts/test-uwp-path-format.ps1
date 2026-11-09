# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'build/uwp-path-format-test'
$logs = Join-Path $root 'artifacts/core-port'
New-Item -ItemType Directory -Force $out, $logs | Out-Null
Start-Transcript -Path (Join-Path $logs 'path-format-test.log')
try {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'MSVC installation not found' }
    Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
    # Compile the actual patched filesystem header with the pinned fmt source.
    # fmt 12 deliberately branches on template constants (C4127). Keep all
    # other /W4 diagnostics fatal; do not modify the dependency or skip assertions.
    & cl /nologo /EHsc /utf-8 /std:c++latest /W4 /WX /wd4127 /DVITA3K_UWP=1 /DFMT_HEADER_ONLY `
        /DWINAPI_FAMILY=WINAPI_FAMILY_APP /D_WIN32_WINNT=0x0A00 `
        "/I$root/upstream-core/vita3k/util/include" "/I$root/upstream-core/external/fmt/include" `
        "$root/tests/uwp-path-format-test.cpp" "/Fo:$out/test.obj" "/Fe:$out/test.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Patched filesystem formatter failed to compile' }
    & "$out/test.exe"
    if ($LASTEXITCODE -ne 0) { throw "Filesystem formatting test failed ($LASTEXITCODE)" }
} finally { Stop-Transcript }
