#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Experimental, isolated port adapter for real Vita3K libretro sources.

Only patches a fresh pinned checkout under upstream-core/. It never replaces
original code with fake emulator functions. std::filesystem is an experiment
to remove Boost.Filesystem's desktop-only Win32 API use from a UWP build.
"""
from __future__ import annotations

import json
import pathlib
import re
import sys

if len(sys.argv) != 2:
    raise SystemExit("Usage: patch-vita3k-uwp.py <pinned-vita3k-source-directory>")
source = pathlib.Path(sys.argv[1]).resolve()
if not (source / "vita3k/util/include/util/fs.h").is_file():
    raise SystemExit("Missing real upstream Vita3K filesystem header")

# Upstream Vita3K uses std::filesystem and header-only Boost since its
# 2026-10 filesystem switch (merged into the pinned core): no Boost.Filesystem
# to take out any more. The Xbox build still needs its VITA3K_UWP define.
manifest_file = source / "vcpkg.json"
manifest = json.loads(manifest_file.read_text())
assert "boost-filesystem" not in manifest["dependencies"], "Boost.Filesystem is back in the core"

cmake = source / "CMakeLists.txt"
text = cmake.read_text()
assert text.count("\nget_boost()\n") == 1, "Upstream Boost integration moved"
text = text.replace(
    "\nget_boost()\n",
    "\nif(CMAKE_SYSTEM_NAME STREQUAL \"WindowsStore\")\n"
    "  add_compile_definitions(VITA3K_UWP=1)\n"
    "endif()\n"
    "get_boost()\n",
)
cmake.write_text(text)
filesystem = source / "vita3k/util/include/util/fs.h"
assert "namespace fs = std::filesystem;" in filesystem.read_text(), "fs is no longer std::filesystem"
print("Core uses std::filesystem; VITA3K_UWP defined for WindowsStore builds.")

# Host CPU details for the log: the registry and process-memory calls are
# desktop-only APIs that an Xbox app cannot call (or link).
sysinfo = source / 'vita3k/util/src/sysinfo.cpp'
text = sysinfo.read_text()
old = '#elif defined(_WIN32)\n    DWORD mhz = 0, size = sizeof(mhz);'
assert text.count(old) == 1
text = text.replace(old, '#elif defined(VITA3K_UWP)\n    return 0;\n' + old, 1)
old = 'uint64_t get_process_memory_usage() {\n#if defined(_WIN32)'
assert text.count(old) == 1
text = text.replace(old, 'uint64_t get_process_memory_usage() {\n#if defined(VITA3K_UWP)\n    return 0;\n#elif defined(_WIN32)', 1)
sysinfo.write_text(text)
print('Host CPU/memory details: no desktop-only registry or psapi calls on Xbox.')

# The libretro backend delivers guest audio to the frontend. Do not build
# the desktop Cubeb fallback or include its desktop-only headers for Xbox.
audio_cmake = source / 'vita3k/audio/CMakeLists.txt'
text = audio_cmake.read_text()
first, rest = text.split('else()', 1)
assert 'src/impl/cubeb_audio.cpp' in first and 'util cubeb kernel' in first
first = first.replace('        src/impl/cubeb_audio.cpp\n', '').replace('util cubeb kernel', 'util kernel')
audio_cmake.write_text(first + 'else()' + rest)
audio_source = source / 'vita3k/audio/src/audio.cpp'
text = audio_source.read_text()
text = text.replace('#include <audio/impl/cubeb_audio.h>', '#ifndef VITA3K_UWP\n#include <audio/impl/cubeb_audio.h>\n#endif')
start = text.index('#ifdef BUILD_LIBRETRO', text.index('void AudioState::set_backend'))
end = text.index('#else', start)
block = text[start:end]
old = '    } else if (adapter_name == "Cubeb") {\n        adapter = std::make_unique<CubebAudioAdapter>(*this);'
assert old in block
block = block.replace(old, '')
text = text[:start] + block + text[end:]
audio_source.write_text(text)
external = source / 'external/CMakeLists.txt'
text = external.read_text()
old = 'add_subdirectory(cubeb EXCLUDE_FROM_ALL)'
assert old in text
external.write_text(text.replace(old, 'if(NOT CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")\n  '+old+'\nendif()'))
print('Routed Xbox guest audio exclusively through the libretro frontend backend.')
text = external.read_text()
start = text.index('if (ARCHITECTURE STREQUAL "arm64")', text.index('add_subdirectory(cubeb'))
end = text.index('\nif(ANDROID)', start)
text = text[:start] + 'if(TARGET speex)\n' + text[start:end] + '\nendif()\n' + text[end:]
external.write_text(text)

# Match the guest address-space allocations to the Xbox AppContainer APIs,
# independently of Dynarmic's already-patched executable code-cache allocator.
memory = source / 'vita3k/mem/src/mem.cpp'
text = memory.read_text()
assert '#include <Windows.h>' in text
text = text.replace('#include <Windows.h>', '#include <Windows.h>\n#ifdef VITA3K_UWP\n#define VirtualAlloc VirtualAllocFromApp\n#define VirtualProtect VirtualProtectFromApp\n#endif', 1)
memory.write_text(text)
print('Guest address-space allocation/protection now uses the FromApp APIs on UWP.')

# Upstream ffmpeg-core downloads desktop static libraries and links user32,
# psapi, and other desktop dependencies. Build the codecs for x64-uwp instead.
manifest = json.loads(manifest_file.read_text())
manifest['dependencies'].append({'name': 'ffmpeg', 'default-features': False,
    'features': ['avcodec', 'avdevice', 'avfilter', 'avformat', 'swresample', 'swscale']})
# Preserve the older codec API used by this pinned Vita3K source.
manifest.setdefault('overrides', []).append({'name': 'ffmpeg', 'version': '6.1.1', 'port-version': 11})
manifest_file.write_text(json.dumps(manifest, indent=2) + '\n')
text = external.read_text()
old = 'add_subdirectory(ffmpeg)'
assert text.count(old) == 1
text = text.replace(old, '''if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
  find_package(FFMPEG REQUIRED)
  add_library(ffmpeg INTERFACE)
  target_include_directories(ffmpeg INTERFACE ${FFMPEG_INCLUDE_DIRS})
  target_link_directories(ffmpeg INTERFACE ${FFMPEG_LIBRARY_DIRS})
  target_link_libraries(ffmpeg INTERFACE ${FFMPEG_LIBRARIES})
else()
  add_subdirectory(ffmpeg)
endif()''')
external.write_text(text)
print('Codec dependencies use source-built UWP FFmpeg 6.1.1 instead of desktop binaries.')

# Pin at the source level too, so a CMake regeneration cannot bring back
# C++23 on SDL's C++/CX files. Keep Vita3K and the other dependencies unchanged.
text = external.read_text()
assert text.count('add_subdirectory(sdl)') == 1
text = text.replace('add_subdirectory(sdl)', '''if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
  set(_vita3k_saved_cxx_standard ${CMAKE_CXX_STANDARD})
  set(CMAKE_CXX_STANDARD 17)
endif()
add_subdirectory(sdl)
if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
  set(CMAKE_CXX_STANDARD ${_vita3k_saved_cxx_standard})
  unset(_vita3k_saved_cxx_standard)
endif()''')
external.write_text(text)
print('Isolated SDL C++/CX targets at C++17 without downgrading the emulator core.')

# (generic_path(): upstream now calls fs_utils::generic_path, nothing to adapt.)

# The libretro host owns presentation; Xbox never creates a Win32 HWND surface.
screen = source / 'vita3k/renderer/src/vulkan/screen_renderer.cpp'
text = screen.read_text()
old = '#ifdef _WIN32\n        vk::Win32SurfaceCreateInfoKHR create_info{};'
assert old in text
screen.write_text(text.replace(old, '#if defined(_WIN32) && !defined(VITA3K_UWP)\n        vk::Win32SurfaceCreateInfoKHR create_info{};', 1))

# Xbox has no desktop console to rename. Retain file/debugger/core callbacks.
logging = source / 'vita3k/util/src/logging.cpp'
text = logging.read_text()
old = '#ifdef _WIN32\n    // set console codepage to UTF-8'
assert old in text
logging.write_text(text.replace(old, '#if defined(_WIN32) && !defined(VITA3K_UWP)\n    // set console codepage to UTF-8', 1))

# Sparse-file hints are an optional storage optimization. Without desktop
# volume APIs, conservatively report no holes rather than assuming sparsity.
# Keep decryption, archive reads and the ordinary-file cache path intact.
lazy = source / 'vita3k/libretro/libretro_lazy_pkg.cpp'
text = lazy.read_text()
old = 'void mark_sparse(const fs::path &path) {\n#ifdef _WIN32'
assert old in text
text = text.replace(old, 'void mark_sparse(const fs::path &path) {\n#if defined(VITA3K_UWP)\n    (void)path;\n#elif defined(_WIN32)', 1)
old = 'static std::string fs_signature(const fs::path &dir) {\n#ifdef _WIN32'
assert old in text
text = text.replace(old, 'static std::string fs_signature(const fs::path &dir) {\n#if defined(VITA3K_UWP)\n    (void)dir;\n    return {};\n#elif defined(_WIN32)', 1)
old = 'static bool probe_holes(const fs::path &dir) {\n'
assert old in text
text = text.replace(old, old + '#if defined(VITA3K_UWP)\n    (void)dir;\n    return false;\n#else\n', 1)
old = '    return holes;\n}\n\nbool holes_supported'
assert old in text
text = text.replace(old, '    return holes;\n#endif\n}\n\nbool holes_supported', 1)
lazy.write_text(text)
print('Adapted native filesystem paths and excluded desktop window/console/sparse-volume helpers on Xbox.')

# Winsock 1's wsock32.dll is a desktop dependency. Keep guest networking on
# Winsock 2 and IP Helper rather than letting the linker resolve it via wsock32.
text = external.read_text()
old = 'target_link_libraries(winsock INTERFACE WSOCK32 WS2_32 IPHLPAPI)'
assert text.count(old) == 1
text = text.replace(old, '''if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
    target_link_libraries(winsock INTERFACE WS2_32 IPHLPAPI)
  else()
    target_link_libraries(winsock INTERFACE WSOCK32 WS2_32 IPHLPAPI)
  endif()''')
external.write_text(text)

# The optional host crash logger used a desktop-only exception hook. Keep
# terminate/file/debugger logging; guest memory exception machinery is separate.
logging = source / 'vita3k/util/src/logging.cpp'
text = logging.read_text()
old = '#ifdef _WIN32\nstatic LONG WINAPI exception_handler'
assert old in text
logging.write_text(text.replace(old, '#if defined(_WIN32) && !defined(VITA3K_UWP)\nstatic LONG WINAPI exception_handler', 1))

# GetAdaptersInfo is hidden by the UWP SDK. Its supported successor returns
# real assigned IPv4 addresses and on-link prefix lengths for the guest.
network = source / 'vita3k/util/src/net_utils.cpp'
text = network.read_text()
old = '#include <winsock2.h>'
assert old in text
text = text.replace(old, old + '\n#include <ws2tcpip.h>', 1)
old = '#ifdef _WIN32\n    PIP_ADAPTER_INFO pAdapterInfo;'
assert old in text
text = text.replace(old, '''#if defined(VITA3K_UWP)
    ULONG bytes = 15 * 1024;
    std::vector<IP_ADAPTER_ADDRESSES> adapters;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (unsigned attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        adapters.resize((bytes + sizeof(IP_ADAPTER_ADDRESSES) - 1) / sizeof(IP_ADAPTER_ADDRESSES));
        result = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, adapters.data(), &bytes);
    }
    if (result != NO_ERROR) {
        LOG_WARN("GetAdaptersAddresses failed with error: {}", result);
        return ret_addrs();
    }
    for (auto *adapter = adapters.data(); adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        for (auto *unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            if (!unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            const auto *address = reinterpret_cast<const sockaddr_in *>(unicast->Address.lpSockaddr);
            const unsigned prefix = unicast->OnLinkPrefixLength;
            if (prefix > 32) continue;
            in_addr mask{};
            mask.s_addr = htonl(prefix == 0 ? 0u : (0xffffffffu << (32 - prefix)));
            char ip[INET_ADDRSTRLEN] = {}, netmask[INET_ADDRSTRLEN] = {};
            if (!inet_ntop(AF_INET, &address->sin_addr, ip, sizeof(ip)) ||
                !inet_ntop(AF_INET, &mask, netmask, sizeof(netmask))) continue;
            out_addrs.push_back({ adapter->AdapterName ? adapter->AdapterName : "network", ip, netmask });
        }
    }
#elif defined(_WIN32)
    PIP_ADAPTER_INFO pAdapterInfo;''', 1)
network.write_text(text)

# (spdlog 1.17, with the core since the upstream merge, uses fmt.get() itself.)
print('Fixed UWP utility exception logging, network enumeration and fmt 12 compatibility.')

# The libretro wrapper has its own optional desktop VEH logger too. Keep its
# trace callback, but do not compile the unavailable registration functions.
core = source / 'vita3k/libretro/libretro_vita3k.cpp'
text = core.read_text()
for suffix in [
    'static PVOID g_lr_vectored_exception_handler_handle',
    'static LONG CALLBACK lr_vectored_exception_handler',
    '    lr_install_vectored_exception_handler();',
    '    lr_remove_vectored_exception_handler();',
]:
    old = '#ifdef _WIN32\n' + suffix
    assert old in text, suffix
    text = text.replace(old, '#if defined(_WIN32) && !defined(VITA3K_UWP)\n' + suffix, 1)
core.write_text(text)

# The frontend stages the user's PUP files in system/vita3k. Install missing
# parts before loading guest modules; copying a PUP alone cannot boot a game.
header = source / 'vita3k/libretro/libretro_installer.h'
text = header.read_text()
old = 'std::string install_firmware_from_folder(const fs::path &pref_path, const fs::path &system_dir);'
assert old in text
text = text.replace(old, 'std::string install_firmware_from_folder(const fs::path &pref_path, const fs::path &system_dir, bool missing_only = false);', 1)
header.write_text(text)
installer = source / 'vita3k/libretro/libretro_installer.cpp'
text = installer.read_text()
old = 'std::string install_firmware_from_folder(const fs::path &pref_path, const fs::path &system_dir) {'
assert old in text
text = text.replace(old, 'std::string install_firmware_from_folder(const fs::path &pref_path, const fs::path &system_dir, bool missing_only) {', 1)
old = '    for (const auto &[kind, label] : parts) {\n        const fs::path pup = firmware_pup(pref_path, system_dir, kind);'
assert old in text
new = '''    const FirmwareStatus before = check_firmware_status(pref_path);
    for (const auto &[kind, label] : parts) {
        if (missing_only && ((kind == PupKind::Preinst && before.preinst_installed) ||
            (kind == PupKind::Firmware && before.firmware_installed) ||
            (kind == PupKind::Font && before.font_installed))) continue;
        const fs::path pup = firmware_pup(pref_path, system_dir, kind);'''
text = text.replace(old, new, 1)
installer.write_text(text)
text = core.read_text()
old = '    if (!check_firmware_installed(pref_path, system_dir)) {'
assert text.count(old) == 1
text = text.replace(old, '''#ifdef VITA3K_UWP
    if (!check_firmware_status(pref_path).all_installed()) {
        lr_log(RETRO_LOG_INFO, "Preparing staged firmware before game load\\n");
        install_firmware_from_folder(pref_path, system_dir, true);
    }
    if (!check_firmware_installed(pref_path, system_dir)) {
        lr_log(RETRO_LOG_ERROR, "Game load stopped: system firmware is not installed\\n");
        return false;
    }
#endif
''' + old, 1)
core.write_text(text)
print('Connected staged firmware installation to UWP game loading; skipped installed components.')

# The UWP headers hide AddVectoredExceptionHandler. MemoryFaultHandler finds
# it at run time and takes guest faults first, as desktop Vita3K does, with
# the top-level filter as the fallback. Preserve the real callback and
# protect-tree invalidation; never replace guest fault handling with a no-op.
import shutil
helper = pathlib.Path(__file__).resolve().parent.parent / 'src/platform/UwpMemoryFaultHandler.h'
shutil.copyfile(helper, source / 'vita3k/mem/include/mem/uwp_fault_handler.h')
text = memory.read_text()
old = '#include <Windows.h>'
assert old in text
text = text.replace(old, old + '\n#ifdef VITA3K_UWP\n#include <mem/uwp_fault_handler.h>\n#endif', 1)
old = '''    if (!AddVectoredExceptionHandler(1, exception_handler)) {
        LOG_CRITICAL("Failed to register an exception handler");
    }'''
assert old in text
text = text.replace(old, '''#ifdef VITA3K_UWP
    vita3k_uwp::MemoryFaultHandler::Install([](uint8_t *addr, bool write) noexcept {
        return access_violation_handler && access_violation_handler(addr, write);
    });
#else
''' + old + '\n#endif', 1)
old = 'void deinit_mem(MemState &state) {\n'
assert old in text
text = text.replace(old, old + '''#ifdef VITA3K_UWP
    vita3k_uwp::MemoryFaultHandler::Remove();
    access_violation_handler = {};
#endif
''', 1)
memory.write_text(text)
print('Preserved guest memory fault recovery using the UWP top-level exception filter.')
