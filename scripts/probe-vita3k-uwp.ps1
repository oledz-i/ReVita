# SPDX-License-Identifier: GPL-2.0-or-later
# Experimental *real* Vita3K core integration. Intentionally exercises the
# Windows Store toolchain, not a fake shim or the normal desktop DLL target.
# Change vs c7e088f: pins SDL3 projects to /std:c++17 between configure and build
# (fixes cl D8016 '/ZW' vs '/std:c++latest') and builds with 4 workers.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$upstream = Join-Path $root 'upstream-core'
$out = Join-Path $root 'artifacts/core-port'
New-Item -ItemType Directory -Path $out -Force | Out-Null
if (!(Test-Path "$upstream/CMakeLists.txt")) { throw "Pinned source checkout is missing" }

$lock = Get-Content (Join-Path $root 'core-candidates.lock.json') -Raw | ConvertFrom-Json
$actual = (git -C $upstream rev-parse HEAD).Trim()
if ($actual -ne $lock.libretro_candidate.commit) { throw "Unexpected libretro source revision: $actual" }

$notes = @(
  "Vita3K libretro UWP source compile attempt",
  "Real source: $($lock.libretro_candidate.repository)",
  "Source revision: $actual",
  "Compiler target: Windows Store / x64 (NOT normal desktop Win32)",
  "Attempting full emulator build; no gameplay claim or UWP compatibility assumed.",
  "vcpkg allows unsupported UWP ports experimentally; failures must still be examined and never treated as compatibility.",
  "Dependencies: Dynarmic CPU/JIT, PS Vita kernel/HLE, SDL3, shader tools, Vulkan/OpenGL, package/firmware loaders.",
  "Renderer: Mesa Dozen (dzn) is a potential Vulkan-over-D3D12 layer but is not bundled or verified for Xbox UWP.",
  "Prerequisites before gameplay: UWP core DLL links, Vulkan >=1.1 HW context using a compatible driver, UWP JIT/VM, Sony firmware installation, legal homebrew boot."
)
$notes | Set-Content -Path (Join-Path $out 'assessment.txt')
$sourceList = @(
  "CMakeLists.txt",
  "vita3k/cpu/src/cpu.cpp",
  "vita3k/renderer/src/vulkan/renderer.cpp",
  "vita3k/kernel/src/thread.cpp"
)
foreach ($path in $sourceList) {
  $present = Test-Path (Join-Path $upstream $path)
  "$path => $present" | Add-Content -Path (Join-Path $out 'assessment.txt')
}

# Upstream's Windows libretro bootstrap explicitly builds VC-WIN64A OpenSSL.
# A WindowsStore build must use the x64-uwp package selected by the toolchain.
$externalCmake = Join-Path $upstream 'external/CMakeLists.txt'
$external = Get-Content $externalCmake -Raw
$desktopBootstrap = 'if(MSVC AND BUILD_LIBRETRO)'
if (([regex]::Matches($external, [regex]::Escape($desktopBootstrap)).Count) -ne 1) {
    throw 'OpenSSL bootstrap source changed; review the pinned source before patching.'
}
$external = $external.Replace($desktopBootstrap, 'if(MSVC AND BUILD_LIBRETRO AND NOT CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")')
$desktopFallback = 'if(NOT OPENSSL_FOUND OR FORCE_BUILD_OPENSSL_MAC)'
if (!$external.Contains($desktopFallback)) { throw 'OpenSSL fallback source changed' }
$external = $external.Replace($desktopFallback, @'
if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore" AND NOT OPENSSL_FOUND)
    message(FATAL_ERROR "UWP OpenSSL must come from x64-uwp; desktop fallback is disabled")
endif()
if(NOT OPENSSL_FOUND OR FORCE_BUILD_OPENSSL_MAC)
'@)
[IO.File]::WriteAllText($externalCmake, $external)

& python "$root/scripts/patch-vita3k-uwp.py" "$upstream" 2>&1 | Tee-Object -FilePath (Join-Path $out 'filesystem-port.log')
if ($LASTEXITCODE -ne 0) { throw 'Pinned filesystem port failed' }
& python "$root/scripts/patch-vita3k-uwp-round2.py" "$upstream" 2>&1 | Tee-Object -FilePath (Join-Path $out 'config-codec-io-port.log')
if ($LASTEXITCODE -ne 0) { throw 'Checked config/codec/io port adapter failed' }
& python "$root/scripts/patch-vita3k-uwp-round3.py" "$upstream" 2>&1 | Tee-Object -FilePath (Join-Path $out 'firmware-install-port.log')
if ($LASTEXITCODE -ne 0) { throw 'Firmware installation hardening failed' }
& python "$root/scripts/apply-core-patches.py" "$upstream" 2>&1 | Tee-Object -FilePath (Join-Path $out 'core-patches.log')
if ($LASTEXITCODE -ne 0) { throw 'Core game-fix patches did not apply' }
& "$root/scripts/test-uwp-path-format.ps1"
$vcpkg = "C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
if (!(Test-Path $vcpkg)) { throw "GitHub Windows runner does not include $vcpkg" }
"Starting Windows Store CMake configure..." | Add-Content -Path (Join-Path $out 'assessment.txt')
$env:VCPKG_MAX_CONCURRENCY = "4"
$env:VCPKG_KEEP_ENV_VARS = "VCPKG_MAX_CONCURRENCY"
$cmdArgs = @(
  "-S", "$upstream",
  "-B", "$root/build/core-uwp-probe",
  "-G", "Visual Studio 17 2022",
  "-A", "x64",
  "-DCMAKE_SYSTEM_NAME=WindowsStore",
  "-DCMAKE_SYSTEM_VERSION=10.0",
  "-DCMAKE_SYSTEM_PROCESSOR=AMD64",
  "-DBUILD_LIBRETRO=ON",
  "-DUSE_DISCORD_RICH_PRESENCE=OFF",
  "-DCMAKE_PROJECT_INCLUDE=$root/scripts/uwp-core-flags.cmake",
  "-DCMAKE_TOOLCHAIN_FILE=$vcpkg",
  "-DVCPKG_TARGET_TRIPLET=x64-uwp",
  "-DVCPKG_INSTALL_OPTIONS=--allow-unsupported",
  "-DUSE_LTO=NEVER",
  "-DDYNARMIC_ENABLE_NO_EXECUTE_SUPPORT=ON",
  "-DDYNARMIC_WARNINGS_AS_ERRORS=OFF",
  "-DVITA3K_FORCE_SYSTEM_BOOST=ON"
)
$log = Join-Path $out 'configure.log'
& cmake @cmdArgs 2>&1 | Tee-Object -FilePath $log
$configurationExit = $LASTEXITCODE
"Configure exit: $configurationExit" | Add-Content -Path (Join-Path $out 'assessment.txt')
Get-Content $log -Tail 90 | Out-File -FilePath (Join-Path $out 'configure-tail.txt')
if ($configurationExit -ne 0) {
  throw "Actual Vita3K core failed to configure for UWP. See uploaded configure.log for the exact blocking dependencies."
}

# SDL3's UWP backend is C++/CX (/ZW), which MSVC rejects together with
# /std:c++latest (cl D8016 seen in the CI build log). Pin SDL to C++17.
& "$PSScriptRoot/fix-sdl-uwp-vcxproj.ps1" -BuildDir "$root/build/core-uwp-probe" 2>&1 | Tee-Object -FilePath (Join-Path $out 'sdl-cxx17-pin.log')

"Starting genuine Vita3K libretro UWP compilation..." | Add-Content -Path (Join-Path $out 'assessment.txt')
$buildLog = Join-Path $out 'build.log'
& cmake --build "$root/build/core-uwp-probe" --config Release --target vita3k_libretro --parallel 4 2>&1 | Tee-Object -FilePath $buildLog
$buildExit = $LASTEXITCODE
"Build exit: $buildExit" | Add-Content -Path (Join-Path $out 'assessment.txt')
Get-Content $buildLog -Tail 90 | Out-File (Join-Path $out 'build-tail.txt')
if ($buildExit -ne 0) {
  throw "Real Vita3K libretro code could not be linked for UWP. See build.log and keepgoing-errors.txt."
}
$cores = Get-ChildItem "$root/build/core-uwp-probe" -Recurse -Filter 'vita3k_libretro.dll' -File
if (!$cores) { throw 'No vita3k_libretro.dll was produced by the Windows Store build' }
foreach ($core in $cores) {
    Copy-Item $core.FullName (Join-Path $out $core.Name)
    "Produced native UWP DLL: $($core.FullName)" | Add-Content (Join-Path $out 'assessment.txt')
}
"Native UWP core DLL compiled. Xbox Vulkan and actual game execution remain to be tested." |
  Add-Content (Join-Path $out 'assessment.txt')
