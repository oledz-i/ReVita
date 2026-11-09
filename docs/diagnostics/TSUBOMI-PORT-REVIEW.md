# Tsubomi 0.47.0 comparison

Source: https://github.com/125hz/Tsubomi/tree/7e53bac87f442e770a39b1d9b032133418337f6d
Release: https://github.com/125hz/Tsubomi/releases/tag/0.47.0
License: GPL-2.0; adapted CPU source retains upstream GPL-2.0-or-later terms.

The release source was cloned and inspected at the exact tag. The repository has TWO paths: ios/core/CMakeLists.txt is an older partial bootstrap, while .github/workflows/ios-upstream.yml and gen-ios-upstream.sh build the actual upstream core. Do not infer the release capabilities from the stale bootstrap README.

## How the full core compiles

- macos-15 runner, Xcode 26, iPhoneOS SDK 26, ARM64 target.
- VITA3K_BUILD_IOS_UPSTREAM_CORE enables platform-specific CMake branches and excludes desktop UI/update integration.
- Pinned vcpkg 77df67c builds arm64-ios dependencies; FFmpeg comes through find_package rather than desktop prebuilts.
- MoltenVK 1.4.1 is linked statically with Apple frameworks. Vulkan instance/device dispatch is adapted to that linkage.
- ios/patches/0001-oaknut-ios-rwx-jit.patch modifies ARM64 Dynarmic/Oaknut and spinlock support. The runtime requires an iOS JIT enabler.
- The core retains its CPU, kernel, HLE, renderer and GDB modules. UIKit/SwiftUI supplies the host.

Xbox equivalents are MSVC WindowsStore x64, x64-uwp dependencies, SDL UWP, Onyx-derived x64 Dynarmic memory changes, Dozen Vulkan over D3D12, and the libretro host. Neither the IPA nor its ARM64 JIT/Apple framework code can be relinked into an Xbox APPX.

## Implemented adaptation

scripts/patch-vita3k-uwp-round2.py now adapts Tsubomi's vita3k/cpu/src/cpu.cpp exception handling for UWP. std::exception from DynarmicCPU construction is logged with the thread ID and returns an empty CPUStatePtr. The pinned core's kernel/src/thread.cpp already checks that result and returns SCE_KERNEL_ERROR_ERROR. Successful construction follows the same path as before. This prevents this category of allocation/construction exception from escaping guest thread setup; it does not make unavailable executable memory available or catch hardware access violations.

Validation: both complete adapters applied to a fresh archive of our pinned core with its pinned spdlog source. Python syntax and whitespace checks pass. Full Windows build remains pending; no Xbox execution result is claimed.

## GPU findings requiring a deliberate separate port

Tsubomi adds can_mprotect_buffer_trapping and support_unmapped_surface_sync. For iOS, debugger interception of protection faults and 16 KiB host pages motivate staging-buffer surface synchronization and reduced fault-based dirty tracking.

Its fallback is NOT simply 'copy all buffers every frame'. BufferTrapping::access_buffer treats always_trap shader-store buffers specially: recopying stale CPU RAM over GPU-owned mirror data caused garbled Persona 4 models according to its source comments. The fallback preserves GPU ownership after initial upload, an approximation which can miss subsequent CPU authorship when faults are unavailable.

Our Xbox fault handler has a native Windows component test but no AppContainer test. Do not turn off dirty tracking solely because iOS does. If Xbox logs show undelivered protection faults, port the complete buffer ownership and unmapped surface readback changes together, with GPU-to-CPU and CPU-to-GPU tests. The changed feature flags, renderer state and surface_cache code must move together; copying only the renderer assignment silently omits synchronization.

## Next evidence required

Finish current normal WindowsStore core link, resolve imports, package DLLs, then capture Xbox initialization/JIT/renderer/first-frame logs with user firmware and game content. Tsubomi's release is useful source evidence, not proof that the same title boots under Dozen or UWP.
