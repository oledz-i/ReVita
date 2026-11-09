# SPDX-License-Identifier: GPL-2.0-or-later
# Republishes the distinct MSBuild/compiler errors of the shell build as one
# workflow annotation, so a failed build can be read from the run page.
[CmdletBinding()]
param([string]$Log = 'artifacts/logs/msbuild.log', [int]$MaxChars = 30000)
$ErrorActionPreference = 'Continue'
if (!(Test-Path $Log)) { Write-Host "::warning::No MSBuild log at $Log"; return }
$pattern = 'error\s+(?:C|D|LNK|MSB|XAML|RC|APPX)\d+|fatal error|MSB\d+: '
$lines = @(Select-String -Path $Log -Pattern $pattern | ForEach-Object {
    ($_.Line.Trim() -replace '(?i)[A-Z]:[\\/](?:a|actions-runner)[\\/][^\\/]+[\\/][^\\/]+[\\/]', '' -replace '\s*\[[^\]]*\.vcxproj\]\s*$', '')
  } | Select-Object -Unique)
if (!$lines.Count) { Write-Host '::warning::The build failed but no compiler errors were found in the log'; return }
$text = New-Object System.Text.StringBuilder
foreach ($line in $lines) {
  if ($text.Length + $line.Length + 1 -gt $MaxChars) { break }
  [void]$text.Append($line).Append("`n")
}
$escaped = $text.ToString().Replace('%', '%25').Replace("`r", '').Replace("`n", '%0A')
Write-Host "::error title=Shell build errors ($($lines.Count) distinct)::$escaped"
