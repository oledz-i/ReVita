# Gameplay core port: round 2

The full-core run 40160215898 failed at SDL3-static with MSVC D8016:
`/ZW` and `/std:c++latest` are incompatible. Configuration succeeded, but this
blocked dependent Vita3K libraries. No working emulator DLL was produced.

## Applied changes

- gameplay-fix-2.zip: generated SDL projects use C++17, four build
  workers, and the build driver invokes the SDL compatibility pass.
- Also scope C++17 around SDL's source CMake directory. A regeneration keeps
  the fix; the Vita3K core retains C++23.
- Force the Windows API family header and Windows 10 target macros consistently
  via a project-include file. Disable unused Discord integration.
- Adapt remaining Boost-only generic_path() calls to native std::filesystem
  paths. Guard Win32 HWND surface creation and desktop console naming on UWP.
- Optional sparse-file/volume probes conservatively return unsupported on UWP;
  archive decryption and ordinary file caching remain present. Sparse cache
  files may occupy more storage.
- Keep guest networking on Winsock 2 instead of the desktop Winsock 1 DLL.
- Unload Vita3K renderer allocations before destroying the borrowed Vulkan
  device, including partial initialization failures.
- Capture MSVC Dxxxx and MSBuild MSBxxxx errors in ERROR-LOG.txt. D8016 was
  present in the full log but missed by the earlier summary filter.

## Sources reviewed

Onyx first: pinned revision 78f2561a16fe7a65e59893f896611263f10575c5.
[build-azahar.ps1](https://github.com/oledz-i/OnyxEMU/blob/78f2561a16fe7a65e59893f896611263f10575c5/tools/build-azahar.ps1)
disables unused frontends/backends and enables Dynarmic W^X support.
[uwp-flags.cmake](https://github.com/oledz-i/OnyxEMU/blob/78f2561a16fe7a65e59893f896611263f10575c5/tools/uwp-flags.cmake)
provides the project-include pattern for consistent API family and target macros.
The W^X port is already applied by apply-onyx-dynarmic.ps1.

Also reviewed the official
[PPSSPP CoreUWP project](https://github.com/hrydgard/ppsspp/blob/master/UWP/CoreUWP/CoreUWP.vcxproj)
and
[RetroArch UWP project](https://github.com/libretro/RetroArch/blob/master/pkg/msvc-uwp/RetroArch-msvcUWP/RetroArch-msvcUWP.vcxproj).
They separate native emulator compilation from the platform frontend rather
than enabling C++/CX across every C++ library. No emulator code from Dan Price
or SternXD was added in this round. The pre-existing SDL3 UWP library dependency
is still selected in core-candidates.lock.json; it is not an emulator core.

## Verification and limits

All source adapters were applied successfully to a fresh archive of the pinned
Vita3K libretro source. Repository and workflow syntax checks pass. These are
source-level checks; Windows CI must compile the affected targets. Xbox runtime
loading, firmware setup, Vulkan features and Undertale execution remain unverified.

## Next compiler batch: utility library

Full-core run 40162246205 got past SDL and failed in the util target:

- C3861: AddVectoredExceptionHandler in the optional host crash logger.
  UWP keeps terminate, file and debugger logging without registering that
  desktop-only logger hook. The CPU/guest memory exception code is untouched.
- C3861: GetAdaptersInfo in network adapter enumeration. The UWP branch now
  uses GetAdaptersAddresses and computes IPv4 subnet masks from prefix lengths.
  See Microsoft's [supported API documentation](https://learn.microsoft.com/en-us/windows/win32/api/iphlpapi/nf-iphlpapi-getadaptersaddresses).
- C4996: spdlog's implicit conversion from fmt 12's format string. The pinned
  char logger overload now uses fmt.get() explicitly. A C++23 syntax check
  with deprecation warnings treated as errors reproduced the failure before
  the patch and passed after it, using the exact pinned fmt/spdlog revisions.

The errors-summary script also had a PowerShell variable-colon parse error.
It now uses braced interpolation, and the workflow parses every PowerShell
script before fetching and compiling the core. Full logs were preserved even
when the old summary script failed.

## Firmware preparation before guest loading

The UWP loader now asks the existing Vita3K installer to unpack staged PUPs
before opening game content. A missing-only mode skips already installed
components; the core's explicit reinstall command retains its previous behavior.
The load stops with a core log error if system firmware remains absent. This
uses only the PUPs supplied by the user; no firmware is added to the package.

The libretro wrapper's separate desktop VEH trace hook is also guarded on UWP,
matching the utility logger fix while preserving ordinary core trace logging.
These source changes applied cleanly to the pinned source; firmware extraction
has not been exercised here because no firmware/game data or Xbox is attached.

## Guest memory fault recovery (run 40163302619)

The next build reached mem.cpp and failed only on its
AddVectoredExceptionHandler call. Unlike the optional logging hooks, this
callback implements protected guest-memory invalidation and cannot be dropped.

The UWP adapter installs the same access-violation callback through
SetUnhandledExceptionFilter, which Microsoft documents for UWP and permits to
return EXCEPTION_CONTINUE_EXECUTION after a handled exception. The new shared
helper rejects execute faults, noncontinuable exceptions and malformed records,
chains unrelated faults, and restores the previous filter during core teardown.

Before compiling the core, Windows CI builds and runs a native test with UWP
API-family declarations enabled. Two guest-like worker threads fault on a
read-only write and a no-access read, respectively; the test requires both
instructions to resume and verifies delegation and filter restoration. The test
uses the exact header copied into Vita3K. It is not an AppContainer/Xbox test:
platform exception boundaries, debuggers, and other installed filters can alter
delivery, so Xbox validation is still necessary.

## Config, codec and IO compilation (run 40164666388)

Applied the third patch's header and diagnostic-build approach, with checked
source replacements rather than a broad regex sweep. CLI11 now uses its normal
supplied-argv branch on UWP instead of a null CommandLineToArgvW result. File
removal, dot entries and trailing-separator handling use std::filesystem, with
root preservation verified against actual directory operations.

The supplied FFCodec struct exactly matches the pinned FFmpeg n6.1.1 declaration.
Added a compile-time ABI/version guard and retained the raw-AAC consumed-byte
contract. Also fixed unpadded guest packet input, an uninitialized got_frame
flag, and release-build acceptance of a missing frame. The actual patched AAC
source passed decoding and exact-PCM comparison against FFmpeg 6.1.1, plus
malformed/empty-input checks. See third_party/ffmpeg-internal/README.md.

The diagnostic build compiles each core project with /t:ClCompile and
BuildProjectReferences=false. It does not attempt to link incomplete projects
or rebuild all external dependencies. Every per-project log is uploaded, along
with keepgoing-errors.txt and keepgoing-top-causes.txt, so skipped dependencies
no longer hide all later source errors. Diagnostic results cannot produce a
package: only a successful ordinary core build proceeds to APPX packaging.

Native checks are in tests/aac-decoder-test.cpp and tests/uwp-path-compat-test.cpp.
AAC fixture data is a generated 440 Hz stereo tone, with no game or firmware.
The codec test can optionally compare PCM against an independently decoded
reference supplied as its second argument. Linux checks used the exact pinned
source and FFmpeg 6.1.1; Windows and Xbox verification remain necessary.

A further source sweep also adapted app-cache dot-entry checks and native file-clock timestamps, which are downstream of the failed config/IO targets.

The follow-up workflow uploads primary failure logs before running downstream diagnostics, so the first failure is downloadable while the longer diagnostic pass continues.
