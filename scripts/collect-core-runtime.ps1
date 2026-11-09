# SPDX-License-Identifier: GPL-2.0-or-later
# Gathers the UWP-built Vita3K libretro core and the runtime DLLs it needs so
# build-appx.ps1 can package them next to the app. Missing or desktop-only
# imports are reported (never hidden) in the staged DEPENDENCIES files.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build/core-uwp-probe'
$out = Join-Path $root 'artifacts/core-runtime'
New-Item -ItemType Directory -Force $out | Out-Null

$core = Get-ChildItem $build -Recurse -Filter 'vita3k_libretro.dll' -File | Select-Object -First 1
if (!$core) { throw 'vita3k_libretro.dll was not produced by the Windows Store build' }
Copy-Item $core.FullName $out -Force

# CMake/vcpkg copy linked runtime DLLs beside the core. Also include the
# target UWP bin directory for explicitly loaded modules (e.g. OpenSSL).
# Never walk the whole build tree: it contains x64 desktop build tools.
$runtimeDirs = @($core.Directory.FullName, (Join-Path $build 'vcpkg_installed/x64-uwp/bin'))
$staged = @{ 'vita3k_libretro.dll' = $core.FullName }
foreach ($dir in $runtimeDirs) {
    if (!(Test-Path $dir)) { continue }
    Get-ChildItem $dir -Filter '*.dll' -File | ForEach-Object {
        $key = $_.Name.ToLowerInvariant()
        if (-not $staged.ContainsKey($key)) {
            $staged[$key] = $_.FullName
            Copy-Item $_.FullName $out -Force
        }
    }
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$dumpbin = & $vswhere -latest -products '*' -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' | Select-Object -First 1
$missing = Join-Path $out 'DEPENDENCIES-MISSING.txt'
$desktop = Join-Path $out 'DEPENDENCIES-DESKTOP-ONLY.txt'
'' | Set-Content $missing
'' | Set-Content $desktop
if ($dumpbin) {
  $have = @{}
  Get-ChildItem $out -Filter '*.dll' | ForEach-Object { $have[$_.Name.ToLowerInvariant()] = $true }
  $system = '^(api-ms-|ext-ms-|kernel32|kernelbase|ntdll|ucrtbase|vccorlib140_app|vcruntime140|msvcp140|concrt140|vcomp140|d3d1[12]|dxgi|dxcore|xaudio2|windows\.|combase|bcrypt|crypt32|ws2_32|mswsock|iphlpapi|cfgmgr32|rpcrt4|secur32|shlwapi|oleaut32|ole32|propsys|mf|mfplat|mfreadwrite|mmdevapi|xinput)'
  $desktopOnly = '^(user32|gdi32|shell32|psapi|winmm|comdlg32|imm32|setupapi|version|dbghelp|advapi32)\.dll$'
  foreach ($dll in Get-ChildItem $out -Filter '*.dll') {
    $deps = & $dumpbin /nologo /dependents $dll.FullName |
      ForEach-Object { $_.Trim() } | Where-Object { $_ -match '^\S+\.dll$' }
    foreach ($d in $deps) {
      $k = $d.ToLowerInvariant()
      if ($k -match $desktopOnly) { "$($dll.Name) -> $d" | Add-Content $desktop; continue }
      if ($have[$k] -or $k -match $system) { continue }
      "$($dll.Name) -> $d" | Add-Content $missing
    }
  }
} else {
  'dumpbin not found; dependency report skipped' | Set-Content $missing
}
$count = (Get-ChildItem $out -Filter '*.dll').Count
"Staged $count DLLs for packaging in $out"
Get-ChildItem $out -Filter '*.dll' | Select-Object Name, Length | Format-Table | Out-String | Write-Host
if ((Get-Content $missing | Where-Object { $_.Trim() }).Count) { Write-Host '::warning::Some core imports are not satisfied by the package; see DEPENDENCIES-MISSING.txt' }
if ((Get-Content $desktop | Where-Object { $_.Trim() }).Count) { Write-Host '::warning::Some imports are desktop-only and may fail in AppContainer; see DEPENDENCIES-DESKTOP-ONLY.txt' }
