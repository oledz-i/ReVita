# Full core link confirmed

Run 40200706328 (full-core build #10, af557fcbca999f2833aae54ab2f922d97db37da4) successfully compiled and normally linked vita3k_libretro.dll for WindowsStore x64. This supersedes earlier review statements that no complete DLL had linked.

Evidence:
- Core logs: https://github.com/oledz-i/Vita3K-UWP/actions/runs/40200706328/artifacts/12603045039
- Runtime DLLs: https://github.com/oledz-i/Vita3K-UWP/actions/runs/40200706328/artifacts/12603045040
- build-tail.txt records the normal linker producing lib/Release/vita3k_libretro.lib and bin/Release/vita3k_libretro.dll.
- Local PE inspection confirms retro_api_version, retro_init, retro_load_game and retro_run exports. Dependency traversal of the downloaded DLLs finds the core plus zlib1, avformat-60, avcodec-60, swresample-4, swscale-7, avutil-58, libssl-3-x64 and libcrypto-3-x64.

The #10 APPX job failed on _wfopen deprecation in the host; 357874e already fixes that. Build #13 (40201913268, 864aac6) stopped at the newly added formatter test because its cl command omitted /utf-8. This commit adds the flag required by fmt. This was a test-command regression, not a new core compiler failure.

The old runtime collector also included unrelated desktop build tools such as pkgconf-7.dll (which imports ADVAPI32). Collection is now limited to the core output and x64-uwp runtime bin directory. A transitive PE import scan confirmed pkgconf-7.dll is not required by the core. MMDevAPI is a platform DLL; vccorlib140_app is supplied by the UWP VC framework. The APPX verification now requires both the Microsoft.VCLibs.140.00 manifest dependency and an x64 VCLibs dependency APPX in the installation bundle.

Still unverified: the latest changed core's link, signed full-core APPX, Xbox DLL loading, firmware installation, guest execution and Undertale display. Do not use the older af557fc DLL as evidence that later Vulkan or Tsubomi changes have executed. The full-core workflow rebuilds the current source rather than silently substituting that older binary.
