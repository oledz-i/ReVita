# SPDX-License-Identifier: GPL-2.0-or-later
# Condenses the (huge) configure/build logs and vcpkg port logs of the UWP core
# attempt into one short file listing every distinct compiler/linker/CMake error,
# so the next fix round starts from the real first failures.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot -Parent
$dir = Join-Path $root 'artifacts/core-port'
New-Item -ItemType Directory -Force $dir | Out-Null
$summary = Join-Path $dir 'errors-summary.txt'
$pattern = 'error\s+(?:C|D|LNK|MSB)\d+|fatal error|CMake Error|Could NOT find|error: |FAILED|vcpkg.*(failed|error)|unresolved external'
'' | Set-Content $summary
foreach ($name in @('configure.log', 'build.log', 'steps.log', 'path-format-test.log', 'memory-fault-test.log')) {
  $p = Join-Path $dir $name
  if (!(Test-Path $p)) { "== ${name}: not produced" | Add-Content $summary; continue }
  $filePattern = $pattern
  if (Select-String -Path $p -Pattern 'error C2220' -Quiet) { $filePattern += '|warning C\d+' }
  $lines = @(Select-String -Path $p -Pattern $filePattern | ForEach-Object { $_.Line.Trim() } | Sort-Object -Unique)
  "== ${name}: $($lines.Count) distinct error lines" | Add-Content $summary
  $lines | Add-Content $summary
}
# vcpkg port build logs hold the real reason a dependency failed.
$vlogs = Join-Path $dir 'vcpkg-logs'
New-Item -ItemType Directory -Force $vlogs | Out-Null
$roots = @((Join-Path $root 'build'), 'C:/vcpkg/buildtrees') | Where-Object { Test-Path $_ }
$found = foreach ($r in $roots) { Get-ChildItem $r -Recurse -Include '*-err.log', '*-out.log' -File -ErrorAction SilentlyContinue }
$i = 0
foreach ($f in ($found | Sort-Object LastWriteTime -Descending | Select-Object -First 40)) {
  $i++
  Get-Content $f.FullName -Tail 120 -ErrorAction SilentlyContinue |
    Set-Content (Join-Path $vlogs ("{0:D2}-{1}" -f $i, $f.Name))
}
"== vcpkg logs captured: $i" | Add-Content $summary
@("Source: $env:GITHUB_SHA", "Run: https://github.com/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID") | Add-Content $summary
Get-Content $summary -TotalCount 120
