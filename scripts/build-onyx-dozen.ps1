# SPDX-License-Identifier: GPL-3.0-or-later
# Reuses the ORIGINAL OnyxEMU source build and exact Mesa patch series, not a
# differently configured stock Mesa driver. OnyxEMU copyright and GPLv3 notices
# are preserved in the source checkout and source attribution report.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$lock = (Get-Content (Join-Path $root 'core-candidates.lock.json') -Raw | ConvertFrom-Json).onyx_renderer_source
if (!$lock.repository -or !$lock.commit -or !$lock.mesa_patch_base) {
    throw 'Onyx source pin missing'
}
$out = Join-Path $root 'artifacts/onyx-dozen'
$work = Join-Path $root 'build/onyx-dozen-source'
New-Item -ItemType Directory -Path $out -Force | Out-Null
if (!(Test-Path (Join-Path $work '.git'))) {
    git clone --depth 25 $lock.repository $work
    if ($LASTEXITCODE -ne 0) { throw 'Cannot fetch original OnyxEMU source' }
}
git -C $work fetch --depth 1 origin $lock.commit
if ($LASTEXITCODE -ne 0) { throw 'Cannot fetch pinned Onyx commit' }
git -C $work checkout --detach --force $lock.commit
if ($LASTEXITCODE -ne 0) { throw 'Cannot check out pinned Onyx commit' }
$actual = (git -C $work rev-parse HEAD).Trim()
if ($actual -ne $lock.commit) { throw 'Onyx pinned revision mismatch' }
$mesaBase = (Get-Content (Join-Path $work 'patches/mesa/BASE_COMMIT') -Raw).Trim()
if ($mesaBase -ne $lock.mesa_patch_base) { throw 'Mesa patch base mismatch (do not apply unreviewed patch set)' }

# This port's own Dozen fixes, applied after OnyxEMU's series (Onyx's
# Apply-Patches takes every patches/mesa/*.patch in name order; ours are 01xx).
$ours = @(Get-ChildItem (Join-Path $root 'scripts/dozen-patches') -Filter '*.patch' -ErrorAction SilentlyContinue | Sort-Object Name)
foreach ($patch in $ours) {
    Copy-Item -LiteralPath $patch.FullName -Destination (Join-Path $work 'patches/mesa') -Force
}

@(
    "Vita3K-UWP renderer build from OnyxEMU (GPL-3.0-or-later)",
    "Reused project: $($lock.repository)",
    "Exact source commit: $actual",
    "Mesa patch base: $mesaBase",
    "Primary Mesa license: MIT; Onyx patches/host: GPL-3.0-or-later",
    "Builder: original Onyx tools/fetch-deps.ps1 and tools/build-dozen.ps1",
    "Vita3K-UWP Dozen patches: $((@(Get-ChildItem (Join-Path $root 'scripts/dozen-patches') -Filter '*.patch' -ErrorAction SilentlyContinue | ForEach-Object Name)) -join ', ')",
    "This compiles a Vulkan-over-D3D12 ICD only. Vita GXM execution/guest CPU remain unavailable.",
    "No Sony firmware, commercial games, keys or dumped content included."
) | Set-Content (Join-Path $out 'SOURCE-AND-STATUS.txt')

# Exact Onyx build scripts were tested against its pinned Mesa tree on Series S.
# They solve UWP's Store CRT selection, lack of LoadLibrary in the ICD, and
# Meson desktop sanity tests that cannot run with an AppContainer binary.
Push-Location $work
try {
    & (Join-Path $work 'tools/fetch-deps.ps1') -Only @('mesa','dxil') *>&1 |
        Tee-Object -FilePath (Join-Path $out 'fetch.log')
    if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) { throw 'Onyx fetch-deps failed' }
    & (Join-Path $work 'tools/build-dozen.ps1') *>&1 |
        Tee-Object -FilePath (Join-Path $out 'build.log')
    if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) { throw 'Onyx build-dozen failed' }
} finally {
    Pop-Location
}

$dzn = Join-Path $work 'deps/bin/vulkan_dzn.dll'
$dxil = Join-Path $work 'deps/bin/dxil.dll'
if (!(Test-Path $dzn)) { throw 'Onyx Vulkan Dozen DLL missing after build' }
if (!(Test-Path $dxil)) { throw 'Onyx DXIL dependency missing after build' }

# Fail if the DLL still depends on the desktop CRT unavailable to Xbox.
$imports = (& dumpbin.exe /nologo /dependents $dzn 2>&1) -join "`n"
$imports | Set-Content (Join-Path $out 'dozen-dependencies.txt')
if ($imports -match '(?i)(^|\W)VCRUNTIME140(_1)?\.dll(\W|$)' -and
    $imports -notmatch '(?i)VCRUNTIME140_APP\.dll') {
    throw 'Dozen links desktop VCRUNTIME instead of UWP Store runtime'
}
Copy-Item -LiteralPath $dzn -Destination (Join-Path $out 'vulkan_dzn.dll') -Force
Copy-Item -LiteralPath $dxil -Destination (Join-Path $out 'dxil.dll') -Force
Get-FileHash $dzn,$dxil -Algorithm SHA256 |
    Select-Object Path,Hash | Format-Table | Out-String |
    Add-Content (Join-Path $out 'SOURCE-AND-STATUS.txt')
'Native patched Mesa Dozen UWP driver compiled. Device creation and Vita rendering remain untested.' |
    Add-Content (Join-Path $out 'SOURCE-AND-STATUS.txt')
