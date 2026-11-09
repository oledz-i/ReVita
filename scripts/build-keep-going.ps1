# SPDX-License-Identifier: GPL-2.0-or-later
# MSBuild normally skips consumers of a failed project. This diagnostic pass
# runs ClCompile for each core project without references, collecting later
# source errors without attempting to link incomplete dependencies. It cannot
# stage a DLL or produce an APPX; the ordinary core build must still succeed.
[CmdletBinding()]
param(
  [Parameter(Mandatory)][string]$BuildDir,
  [Parameter(Mandatory)][string]$OutDir,
  [int]$Parallel = 2
)
$ErrorActionPreference = 'Continue'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (!$msbuild) { Write-Host '::warning::MSBuild not found; keep-going pass skipped'; return }

$logs = Join-Path $OutDir 'keepgoing'
New-Item -ItemType Directory -Force $logs | Out-Null
$skip = '^(ALL_BUILD|ZERO_CHECK|INSTALL|PACKAGE|RUN_TESTS|uninstall)$'
$projects = @(Get-ChildItem (Join-Path $BuildDir 'vita3k') -Recurse -Filter '*.vcxproj' -File |
  Where-Object { $_.BaseName -notmatch $skip -and $_.BaseName -notmatch '(-tests|_tests)$' })
# Compile only: missing dependency .lib files must not hide source diagnostics.
Write-Host "Keep-going compile pass over $($projects.Count) projects"

$projects | ForEach-Object -ThrottleLimit $Parallel -Parallel {
  $log = Join-Path $using:logs ($_.BaseName + '.log')
  & $using:msbuild $_.FullName /nologo /m:2 /v:minimal /t:ClCompile '/p:Configuration=Release' '/p:Platform=x64' '/p:BuildProjectReferences=false' '/clp:NoSummary' *> $log
}

$errorPattern = 'error\s+(?:C|D|LNK|MSB)\d+|fatal error'
$rows = foreach ($log in Get-ChildItem $logs -Filter '*.log') {
  foreach ($m in (Select-String -Path $log.FullName -Pattern $errorPattern)) {
    $line = ($m.Line.Trim() -replace '\s*\[[^\]]*\]\s*$', '')
    [pscustomobject]@{ Project = $log.BaseName; Line = $line }
  }
}
$rows = @($rows | Sort-Object Project, Line -Unique)

$digest = Join-Path $OutDir 'keepgoing-errors.txt'
"Distinct source error lines across core projects: $($rows.Count)" | Set-Content $digest
''  | Add-Content $digest
'== By project (ClCompile only; individual logs contain full context) ==' | Add-Content $digest
foreach ($g in ($rows | Group-Object Project | Sort-Object Name)) {
  "-- $($g.Name): $($g.Count) distinct" | Add-Content $digest
  $g.Group | Select-Object -First 60 | ForEach-Object { '   ' + $_.Line } | Add-Content $digest
}

# Root-cause view: same message text regardless of file/line, most frequent first.
$causes = $rows | Where-Object { $_.Line -notmatch 'error LNK' } | ForEach-Object {
  if ($_.Line -match '(error [A-Z]+\d+:.*)$') { $Matches[1] } else { $_.Line }
} | Group-Object | Sort-Object Count -Descending | Select-Object -First 80
$top = Join-Path $OutDir 'keepgoing-top-causes.txt'
$causes | ForEach-Object { '{0,4}x  {1}' -f $_.Count, $_.Name } | Set-Content $top
Write-Host "Wrote $digest and $top"
Get-Content $top -TotalCount 40 | Write-Host
