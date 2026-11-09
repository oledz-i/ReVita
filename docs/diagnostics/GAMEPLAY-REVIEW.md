# Gameplay readiness review — 25 October 2026

No complete core DLL link, core-inclusive APPX, Xbox loader success, guest execution or visible Undertale frame has been established. Do not use the frontend workflow's green status as a gameplay result.

## Build evidence and next step

Run #8 (40198139758, fc60c26) failed its normal build and is collecting downstream diagnostics. Its published primary log is linked in core-run8-primary-errors.txt; that file retains distinct compiler errors from the original build.log. The primary failure is the GDB Winsock declaration cascade. It predates the fixes in d4535a8.

Run 40200020879 contains d4535a8 and was pending behind #8 at review time. Check its result before changing those fixes. They cover Winsock 2/linkage, native fmt path formatting, remaining guest network APIs, username copying and unsigned alignment. Both adapters apply to fresh pinned source; an actual patched util/fs.h formatting test passes against pinned fmt. Windows compilation remains the decisive check.

## Additional confirmed diagnostics bug fixed in this patch

VitaLibretroHost::RecentLog passed its 12,000-byte ring buffer through a converter rejecting text over about 2,048 UTF-16 characters, replacing useful diagnostics with '(unavailable)'. The limit now accommodates the ring. Trimming drops leading UTF-8 continuation bytes.

Frontend core callbacks also append to LocalFolder/vita3k-runtime.log, closing the file after each message. Initialization, content loading result and Vulkan initialization failure are recorded. Retrieve this file from the application's local data through Xbox Device Portal after testing. It is created at runtime, not inside the downloaded APPX ZIP. Writes are best effort: a crash during a message, inaccessible storage or a platform failure can still prevent a complete log. It appends across launches and may grow during verbose sessions. This path is not console-tested yet.

## Source review findings for Astra

1. Game packages are copied through StorageFile::CopyAsync into LocalFolder/Vita3KContent before native loading. This addresses picker-only access for single-file packages. Extracted directories are explicitly unsupported by AttemptBoot; test a supported package. Native external-drive loading is not established.
2. Firmware must really install, not just be staged. Adapter calls the existing PUP installer before game loading. Verify both PUP locations, extraction and guest module resolution with user-provided firmware. No PUP or Undertale data is available here.
3. Vulkan initialization invokes the core's device-creation negotiation and then context_reset. The core's context_reset_finalize starts pending guest execution. The host therefore must preserve this order; returning true from retro_load_game alone does not establish execution.
4. Vulkan presentation uses synchronous readback. Present waits with UINT64_MAX for its fence while holding queueMutex_; Wait and shutdown use queue/device idle waits. A stalled GPU can hang the frame thread and StopRunning's join. This is a concrete liveness risk, not an observed Xbox failure. A timeout patch must retain resources still referenced by GPU work; merely timing out and freeing them is unsafe.
5. Readback only accepts RGBA/BGRA 8-bit UNORM/SRGB, a coherent host memory type, and the graphics queue family (or IGNORED). Verify the actual images/queue family/memory types from Dozen. Device feature, extension and shader compatibility is still untested.
6. Context/device teardown must remain coordinated: guest renderer resources are freed before the bridge destroys the shared device. Verify load-failure cleanup, unload and second launch; no claim that these paths are console-proven.
7. Memory-fault recovery passed a native Windows worker-thread test with UWP declarations, not actual AppContainer/Xbox/JIT execution. Obtain console evidence before assuming guest page protections work.
8. Guest control options are not implemented by the host's GET_VARIABLE service; core defaults apply. Review the core's fallback defaults if runtime logs show unexpected renderer/memory settings. Do not claim every frontend settings toggle controls the core.
9. Older InspectLaunch text still describes missing integration. Use actual loader results and logs to assess the package; this review does not spend gameplay effort rewriting catalog copy.

## Validation sequence

Normally link vita3k_libretro; inspect staged runtime dependency/import reports; verify core and dependent DLLs in the signed APPX; install on Xbox; confirm LoadPackagedLibrary and exports; install firmware; load a supported Undertale package from internal app storage; capture runtime log through context_reset, guest start and first video callback. Only then judge whether gameplay works. Report exact commit and APPX artifact with each log.

Preserve source pins and real CPU/kernel/HLE/rendering paths. Prefer Onyx references. The existing SternXD SDL UWP library pin remains inherited and has not been replaced; do not add other code from the excluded developers.
