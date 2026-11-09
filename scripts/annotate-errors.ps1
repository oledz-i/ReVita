# SPDX-License-Identifier: GPL-2.0-or-later
# Republishes the condensed error digests as workflow annotations, so the
# distinct failures can be read from the run page (and the checks API) without
# downloading the log artifacts. GitHub keeps at most 10 error annotations per
# step, so the digests are packed into a few large messages.
[CmdletBinding()]
param([int]$MaxChars = 30000)
$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot -Parent
$dir = Join-Path $root 'artifacts/core-port'

function Get-Escaped([string]$text) {
  return $text.Replace('%', '%25').Replace("`r", '').Replace("`n", '%0A')
}

function Compress-Line([string]$line) {
  # Long absolute runner paths add nothing; keep the source-relative part.
  $line = $line -replace '(?i)[A-Z]:[\\/](?:a|actions-runner)[\\/][^\\/]+[\\/][^\\/]+[\\/]', ''
  $line = $line -replace '\s*\[[^\]]*\.vcxproj\]\s*$', ''
  return $line.Trim()
}

$sections = @()
$buildLog = Join-Path $dir 'build.log'
if (Test-Path $buildLog) {
  $primary = @(Select-String -Path $buildLog -Pattern 'error\s+(?:C|D|LNK|MSB)\d+|fatal error|CMake Error' |
    ForEach-Object { Compress-Line $_.Line } | Select-Object -Unique)
  if ($primary.Count) { $sections += [pscustomobject]@{ Name = 'build.log errors'; Lines = $primary } }
}
foreach ($name in @('keepgoing-top-causes.txt', 'keepgoing-errors.txt', 'errors-summary.txt', 'build-tail.txt', 'configure-tail.txt')) {
  $p = Join-Path $dir $name
  if (!(Test-Path $p)) { continue }
  $lines = @(Get-Content $p | ForEach-Object { Compress-Line $_ } | Where-Object { $_ })
  if ($name -eq 'errors-summary.txt') {
    # Only the compiler/linker diagnostics; skip the long command lines.
    $lines = @($lines | Where-Object { $_ -match '^==|error|warning C|unresolved|Could NOT|FAILED' } | Select-Object -Unique)
  }
  if ($lines.Count) { $sections += [pscustomobject]@{ Name = $name; Lines = $lines } }
}

$budget = 9
foreach ($section in $sections) {
  if ($budget -le 0) { break }
  $chunk = New-Object System.Text.StringBuilder
  $part = 1
  foreach ($line in $section.Lines) {
    if ($chunk.Length + $line.Length + 1 -gt $MaxChars) {
      Write-Host ("::error title={0} (part {1})::{2}" -f $section.Name, $part, (Get-Escaped $chunk.ToString()))
      $budget--; $part++
      [void]$chunk.Clear()
      if ($budget -le 0) { break }
    }
    [void]$chunk.Append($line).Append("`n")
  }
  if ($budget -gt 0 -and $chunk.Length) {
    Write-Host ("::error title={0} (part {1})::{2}" -f $section.Name, $part, (Get-Escaped $chunk.ToString()))
    $budget--
  }
}
if (!$sections.Count) { Write-Host '::warning::No error digests were produced' }
