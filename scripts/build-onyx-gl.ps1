# SPDX-License-Identifier: GPL-3.0-or-later
# OpenGL on D3D12 (onyx_gl.dll): Mesa's d3d12 gallium driver with OnyxEMU's
# window-less front end (Onyx patches 0029-0034), built by Onyx's own
# tools/build-mesa-gl.ps1 from the Mesa tree build-onyx-dozen.ps1 fetched and
# patched. Runs in a fresh PowerShell (its own workflow step), as Onyx's CI
# does: the Dozen build leaves compiler settings behind that break this one.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'artifacts/onyx-dozen'
$work = Join-Path $root 'build/onyx-dozen-source'
$log = Join-Path $out 'build-gl.log'
if (!(Test-Path (Join-Path $work 'tools/build-mesa-gl.ps1'))) { throw 'Onyx checkout without build-mesa-gl.ps1' }
Push-Location $work
try {
    & (Join-Path $work 'tools/build-mesa-gl.ps1') *>&1 | Tee-Object -FilePath $log
    $code = $LASTEXITCODE
} catch {
    "$_" | Add-Content $log
    $code = 1
} finally {
    Pop-Location
}
$gl = Join-Path $work 'deps/bin/onyx_gl.dll'
if (Test-Path $gl) {
    Copy-Item -LiteralPath $gl -Destination (Join-Path $out 'onyx_gl.dll') -Force
    $map = Join-Path $work 'deps/bin/onyx_gl.map'
    if (Test-Path $map) { Copy-Item -LiteralPath $map -Destination (Join-Path $out 'onyx_gl.map') -Force }
    Write-Host 'onyx_gl.dll built'
    exit 0
}
# Not built: what went wrong, on the run page (the logs are not always reachable).
$lines = @()
if (Test-Path $log) { $lines += Get-Content $log | Select-String -Pattern 'ERROR|error|FAILED|not found|Exception|NOT AVAILABLE|Program .* found: NO|Dependency .* found: NO' | Select-Object -Last 25 | ForEach-Object { $_.Line } }
$mesonLog = Join-Path $work 'deps/build/mesa-gl/meson-logs/meson-log.txt'
if (Test-Path $mesonLog) { $lines += Get-Content $mesonLog -Tail 30 }
if (!$lines.Count -and (Test-Path $log)) { $lines += Get-Content $log -Tail 30 }
foreach ($l in $lines) {
    $clean = ($l -replace '[\r\n]', ' ').Trim()
    if ($clean) { Write-Host "::warning title=onyx_gl build::$($clean.Substring(0, [Math]::Min(400, $clean.Length)))" }
}
if (Test-Path $mesonLog) { Copy-Item $mesonLog (Join-Path $out 'mesa-gl-meson-log.txt') -Force }
Write-Host "::warning title=OpenGL driver::onyx_gl.dll was not built (exit $code); the package ships without OpenGL"
exit 0
