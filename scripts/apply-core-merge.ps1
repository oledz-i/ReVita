# SPDX-License-Identifier: GPL-2.0-or-later
# Brings the pinned libretro core (core-candidates.lock.json) up to the
# upstream Vita3K commit in upstream_merge: the full merge of upstream into
# the core, resolved once and kept as one patch (scripts/core-merge), plus the
# submodules upstream moved. Applied right after the core is fetched, before
# the Xbox patches, which are written against the merged source.
[CmdletBinding()]
param([string]$Source = 'upstream-core')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$lock = Get-Content (Join-Path $root 'core-candidates.lock.json') -Raw | ConvertFrom-Json
$merge = $lock.upstream_merge
if (-not $merge) { Write-Host 'No upstream merge pinned.'; return }
$patch = Join-Path $root $merge.patch
if (!(Test-Path $patch)) { throw "Missing merge patch $patch" }
& git -C $Source apply --check --binary --whitespace=nowarn $patch
if ($LASTEXITCODE -ne 0) { throw "The upstream merge patch does not apply to the pinned core" }
& git -C $Source apply --binary --whitespace=nowarn $patch
if ($LASTEXITCODE -ne 0) { throw "Applying the upstream merge patch failed" }
foreach ($sub in $merge.submodules.PSObject.Properties) {
    $dir = Join-Path $Source $sub.Name
    & git -C $dir fetch --depth 1 origin $sub.Value
    if ($LASTEXITCODE -ne 0) { throw "Could not fetch $($sub.Name) at $($sub.Value)" }
    & git -C $dir checkout --detach $sub.Value
    if ($LASTEXITCODE -ne 0) { throw "Could not check out $($sub.Name) at $($sub.Value)" }
}
"Core merged with upstream Vita3K $($merge.commit) ($($merge.patch))" | Write-Host
