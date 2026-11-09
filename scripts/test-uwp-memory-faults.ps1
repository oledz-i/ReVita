# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'build/uwp-memory-fault-test'
$logs = Join-Path $root 'artifacts/core-port'
New-Item -ItemType Directory -Force $out, $logs | Out-Null
Start-Transcript -Path (Join-Path $logs 'memory-fault-test.log')
try {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'MSVC installation not found' }
    Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
    # API visibility matches UWP. Execute the test as a native Windows process;
    # AppContainer/Xbox delivery still requires hardware verification.
    & cl /nologo /EHsc /std:c++17 /W4 /WX /DWINAPI_FAMILY=WINAPI_FAMILY_APP /D_WIN32_WINNT=0x0A00 `
        "$root/tests/uwp-memory-fault-test.cpp" "/Fo:$out/test.obj" "/Fe:$out/test.exe" /link WindowsApp.lib
    if ($LASTEXITCODE -ne 0) { throw 'UWP API-family memory fault test failed to compile' }
    & "$out/test.exe"
    if ($LASTEXITCODE -ne 0) { throw "Memory fault recovery test failed ($LASTEXITCODE)" }
} finally { Stop-Transcript }
