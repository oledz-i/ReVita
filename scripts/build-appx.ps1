# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param(
    [ValidateRange(0,65535)][int]$BuildNumber = 0,
    # Public release: hide the developer Tools page. Also set by VITA3K_RELEASE=1.
    [switch]$Release
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$releaseBuild = $Release.IsPresent -or ($env:VITA3K_RELEASE -eq '1')
$releaseArgs = @()
if ($releaseBuild) { $releaseArgs += '/p:Vita3KRelease=true' }
Write-Host "Build flavor: $(if ($releaseBuild) { 'release (Tools page hidden)' } else { 'development (Tools page included)' })"
$root = Split-Path $PSScriptRoot -Parent
$artifacts = Join-Path $root 'artifacts'
$packages = Join-Path $artifacts 'package'
$logs = Join-Path $artifacts 'logs'
New-Item -ItemType Directory -Force $packages, $logs | Out-Null
if (Get-ChildItem $packages) { throw 'artifacts/package must be empty. Move previous artifacts before rebuilding.' }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (!$msbuild) { throw 'Install Visual Studio 2022 with C++ UWP tools and the Windows 10/11 SDK.' }
$sdk = "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.22621.0\x64"
$signTool = Join-Path $sdk 'signtool.exe'
if (!(Test-Path $signTool)) { throw 'Windows SDK 10.0.22621.0 is required.' }
$manifestPath = Join-Path $root 'src/Vita3K.UWP/Package.appxmanifest'
$originalManifest = [IO.File]::ReadAllText($manifestPath)
& python "$root/scripts/prepare-render-abi.py"
if ($LASTEXITCODE -ne 0) { throw 'Render ABI header preparation failed' }
$cert = $null
try {
    [xml]$manifest = $originalManifest
    $manifest.Package.Identity.Version = "0.1.$BuildNumber.0"
    $manifest.Save($manifestPath)
    & $msbuild "$root/src/Vita3K.UWP/Vita3K.UWP.vcxproj" /restore /m /p:Configuration=Release /p:Platform=x64 /p:UapAppxPackageBuildMode=SideloadOnly /p:AppxPackageExtension=.appx "/p:AppxPackageDir=$packages\" /p:AppxPackageSigningEnabled=false @releaseArgs "/bl:$logs/build.binlog" /fl "/flp:logfile=$logs/msbuild.log;verbosity=normal"
    if ($LASTEXITCODE -ne 0) { throw "MSBuild failed ($LASTEXITCODE)." }
    $appx = @(Get-ChildItem $packages -Recurse -Filter '*.appx' | Where-Object { $_.FullName -notmatch '[\\/]Dependencies[\\/]' })
    if ($appx.Count -ne 1) { throw "Expected exactly one application APPX; found $($appx.Count)." }
    # A disposable test identity; no repository secrets or private-key artifact.
    $cert = New-SelfSignedCertificate -Type Custom -Subject 'CN=Vita3K-UWP' -KeyUsage DigitalSignature -FriendlyName 'Vita3K UWP disposable CI signing' -CertStoreLocation 'Cert:\CurrentUser\My' -NotAfter (Get-Date).AddMonths(3) -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3','2.5.29.19={text}')
    & $signTool sign /fd SHA256 /s My /sha1 $cert.Thumbprint $appx[0].FullName
    if ($LASTEXITCODE -ne 0) { throw 'APPX signing failed.' }
    Export-Certificate -Cert $cert -FilePath "$packages/Development.cer" | Out-Null
    # Trust only in the build machine's TrustedPeople store so signtool can validate the test signature.
    Import-Certificate -FilePath "$packages/Development.cer" -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople' | Out-Null
    & $signTool verify /pa /v $appx[0].FullName
    if ($LASTEXITCODE -ne 0) { throw 'APPX signature verification failed.' }
    Copy-Item "$root/docs/INSTALL.md" "$packages/INSTALL.md"
    Copy-Item "$root/COPYING.txt" "$packages/COPYING.txt"
    if (Test-Path "$root/COPYING.GPLv3.txt") { Copy-Item "$root/COPYING.GPLv3.txt" "$packages/COPYING.GPLv3.txt" }
    Copy-Item "$root/NOTICE.md" "$packages/NOTICE.md"
    $hash = Get-FileHash $appx[0].FullName -Algorithm SHA256
    "$($hash.Hash)  $($appx[0].Name)" | Set-Content "$packages/SHA256SUMS.txt"
    $commit = git -C $root rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Cannot determine source commit.' }
    $hasCore = Test-Path "$root/src/Vita3K.UWP/vita3k_libretro.dll"
    $packageKind = if ($hasCore) { 'full-core-xbox-test' } else { 'frontend-only' }
    @{release_build=$releaseBuild; tools_page_included=(-not $releaseBuild); source_commit=$commit; version="0.1.$BuildNumber.0"; architecture='x64'; kind=$packageKind; emulator_core_integrated=$hasCore; gameplay_verified=$false; vita3k_allocator_integrated=$true; d3d12_host_linked=$true; ui_sound_effects=$true; formats_recognized=@('vpk','zip','pkg','vci','eboot.bin','extracted-folders'); game_boot_supported=$false; game_package_installer_integrated=$hasCore; libretro_core_dll_packaged=(Test-Path "$root/src/Vita3K.UWP/vita3k_libretro.dll"); onyx_dozen_icd_packaged=(Test-Path "$root/src/Vita3K.UWP/vulkan_dzn.dll"); onyx_dxil_packaged=(Test-Path "$root/src/Vita3K.UWP/dxil.dll"); onyx_source_revision='78f2561a16fe7a65e59893f896611263f10575c5'; vulkan_dozen_verified_on_xbox=$false; certificate_thumbprint=$cert.Thumbprint} | ConvertTo-Json | Set-Content "$packages/build-info.json"
    if ($env:GITHUB_STEP_SUMMARY) {
        "## Vita3K UWP core/renderer integration`nBuilt signed x64 APPX **0.1.$BuildNumber.0** from $commit.`n`n**Streaming library, UI audio, Vita file recognition, genuine Vita3K allocator and D3D12 presentation are included. This branch supports packaging Onyx-patched Mesa Dozen when its own source build succeeds. Package type: $packageKind. Xbox core loading and game boot remain unverified.** Download the package artifact for APPX, dependencies, public test certificate and installation instructions. Xbox launch must be tested on hardware." | Out-File $env:GITHUB_STEP_SUMMARY -Append
    }
} finally {
    [IO.File]::WriteAllText($manifestPath, $originalManifest)
    if ($cert) {
        Remove-Item "Cert:\LocalMachine\TrustedPeople\$($cert.Thumbprint)" -ErrorAction SilentlyContinue
        Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey -ErrorAction SilentlyContinue
    }
}
