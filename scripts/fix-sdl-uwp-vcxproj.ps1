# SPDX-License-Identifier: GPL-2.0-or-later
# The pinned UWP SDL3 port compiles its WinRT platform files as C++/CX (/ZW).
# MSVC rejects that combination with /std:c++latest (error D8016), and the
# upstream project sets a newer C++ standard globally. After CMake generates the
# Visual Studio projects, pin every SDL project to /std:c++17.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$BuildDir)
$ErrorActionPreference = 'Stop'
$sdlDir = Join-Path $BuildDir 'external/sdl'
if (!(Test-Path $sdlDir)) { throw "SDL build directory not found: $sdlDir" }
$changed = 0
foreach ($p in Get-ChildItem $sdlDir -Recurse -Filter '*.vcxproj') {
  $text = [IO.File]::ReadAllText($p.FullName)
  $new = $text
  $new = [regex]::Replace($new, '<LanguageStandard>[^<]*</LanguageStandard>', '<LanguageStandard>stdcpp17</LanguageStandard>')
  $new = [regex]::Replace($new, '/std:c\+\+(latest|20|23)', '/std:c++17')
  if ($new -ne $text) {
    [IO.File]::WriteAllText($p.FullName, $new)
    $changed++
    Write-Host "Pinned C++17 for $($p.Name)"
  }
}
if ($changed -eq 0) { Write-Host 'SDL projects already use the source-level C++17 pin.' }
