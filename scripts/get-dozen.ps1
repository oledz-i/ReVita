# SPDX-License-Identifier: GPL-2.0-or-later
# Puts vulkan_dzn.dll and dxil.dll in -Destination: from the newest successful
# "Dozen from source" run (Onyx patches + scripts/dozen-patches), else from the
# pinned OnyxEMU build all earlier packages used.
param([Parameter(Mandatory = $true)][string]$Destination)
$ErrorActionPreference = 'Stop'
$repo = $env:GITHUB_REPOSITORY
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
# A Dozen build started by this same commit (a Dozen or OpenGL change) is
# waited for, so the package carries it rather than the previous one.
if ($env:GITHUB_SHA) {
    $same = gh run list --repo $repo --workflow dozen-source.yml --commit $env:GITHUB_SHA --limit 1 --json databaseId,status --jq '.[0] | select(.status != "completed") | .databaseId' 2>$null
    if ($same) {
        Write-Host "Waiting for Dozen build run $same of this commit"
        gh run watch $same --repo $repo --interval 60 > $null 2>&1
    }
}
$runId = gh run list --repo $repo --workflow dozen-source.yml --status success --limit 1 --json databaseId --jq '.[0].databaseId' 2>$null
$from = $null
if ($runId) {
    gh run download $runId --repo $repo --name vita3k-dozen-uwp --dir $Destination
    if ($LASTEXITCODE -eq 0) { $from = "source build run $runId" }
}
if (!$from) {
    gh run download 40146146144 --repo $repo --name onyx-mesa-dozen-uwp --dir $Destination
    if ($LASTEXITCODE -ne 0) { throw 'Could not get Dozen from a source build or the pinned OnyxEMU build' }
    $from = 'pinned OnyxEMU build (run 40146146144)'
}
foreach ($f in @('vulkan_dzn.dll', 'dxil.dll')) {
    if (!(Test-Path (Join-Path $Destination $f))) { throw "$f missing from Dozen ($from)" }
}
Write-Host "Dozen from $from"
if (Test-Path (Join-Path $Destination 'onyx_gl.dll')) { Write-Host 'OpenGL driver (onyx_gl.dll) included' } else { Write-Host 'No OpenGL driver in this Dozen build: OpenGL falls back to Vulkan' }
Get-FileHash (Join-Path $Destination 'vulkan_dzn.dll'), (Join-Path $Destination 'dxil.dll') | Format-Table | Out-String | Write-Host
