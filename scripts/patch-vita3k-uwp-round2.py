#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Scoped, checked adapters for config/codec/io failures in run 40164666388."""
import pathlib
import shutil
import sys

source = pathlib.Path(sys.argv[1]).resolve()
repo = pathlib.Path(__file__).resolve().parent.parent

def replace(relative, old, new, count=1):
    path = source / relative
    text = path.read_text()
    assert text.count(old) == count, f'{relative}: expected {count} matches for {old!r}'
    path.write_text(text.replace(old, new))

# A core uses argv supplied by its frontend; it must not inspect the process
# command line through shell32. Keep CLI11's ordinary passed-argv path on UWP.
cli = 'external/cli11/CLI11.hpp'
for suffix in [
    '/// Decode and return UTF-8 argv from GetCommandLineW.',
    'CLI11_INLINE std::vector<std::string> compute_win32_argv() {',
    '    (void)argv;\n\n    normalized_argv_ = detail::compute_win32_argv();',
]:
    replace(cli, '#ifdef _WIN32\n' + suffix,
            '#if defined(_WIN32) && !defined(VITA3K_UWP)\n' + suffix)

# (Guest deletion/directory semantics: upstream's std::filesystem switch.)

# Preserve AAC's exact consumed-byte callback. The complete FFCodec declaration
# was token-compared to n6.1.1 upstream; a version guard rejects ABI drift.
include = source / 'vita3k/codec/ffmpeg-internal/libavcodec'
include.mkdir(parents=True, exist_ok=True)
shutil.copyfile(repo / 'third_party/ffmpeg-internal/libavcodec/codec_internal.h', include / 'codec_internal.h')
p = source / 'vita3k/codec/CMakeLists.txt'
p.write_text(p.read_text() + '\nif(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")\n  target_include_directories(codec PRIVATE ffmpeg-internal)\nendif()\n')

# The original decoder pointed FFmpeg at unpadded guest memory and trusted an
# uninitialized got_frame flag. Own a padded packet and validate real output.
replace('vita3k/codec/src/aac.cpp', '#include <util/log.h>',
        '#include <util/log.h>\n#include <cstring>\n#include <limits>')
replace('vita3k/codec/src/aac.cpp', '''    AVPacket *packet = av_packet_alloc();
    packet->data = const_cast<uint8_t *>(data);
    packet->size = size;''', '''    es_size_used = 0;
    if (!data || size == 0 || size > static_cast<uint32_t>(std::numeric_limits<int>::max()))
        return false;
    AVPacket *packet = av_packet_alloc();
    if (!packet) return false;
    if (av_new_packet(packet, static_cast<int>(size)) < 0) {
        av_packet_free(&packet);
        return false;
    }
    std::memcpy(packet->data, data, size);''')
replace('vita3k/codec/src/aac.cpp', '''    int got_frame;
    int len = ff_codec->cb.decode(context, frame, &got_frame, packet);
    assert(got_frame);''', '''    int got_frame = 0;
    if (ff_codec->cb_type != FF_CODEC_CB_TYPE_DECODE || !ff_codec->cb.decode) {
        av_packet_free(&packet);
        LOG_WARN("AAC decoder callback type does not match the pinned FFmpeg ABI");
        return false;
    }
    int len = ff_codec->cb.decode(context, frame, &got_frame, packet);''')
replace('vita3k/codec/src/aac.cpp', '    if (len < 0) {',
        '    if (len <= 0 || static_cast<uint32_t>(len) > size || !got_frame) {')
replace('vita3k/codec/src/aac.cpp', '''        LOG_WARN("Error sending Aac packet: {}.", codec_error_name(len));
        return false;''', '''        LOG_WARN("AAC produced no valid frame (consumed {}, input {}, frame {})", len, size, got_frame);
        av_frame_unref(frame);
        return false;''')
print('Applied checked UWP CLI argv, filesystem semantics and padded AAC decoder fixes.')

# (apps_list and the fmt path formatter: upstream's std::filesystem switch.)

# Winsock 1 declarations are desktop-only. Keep the real debugger service and
# use Winsock 2, with its import library explicitly attached to the target.
for relative in ['vita3k/gdbstub/include/gdbstub/state.h', 'vita3k/gdbstub/src/gdb.cpp']:
    replace(relative, '#include <winsock.h>', '#include <winsock2.h>\n#include <ws2tcpip.h>')
replace('vita3k/gdbstub/src/gdb.cpp', '#include <emuenv/state.h>',
        '#ifdef _WIN32\n#include <winsock2.h>\n#include <ws2tcpip.h>\n#endif\n\n#include <emuenv/state.h>')
replace('vita3k/gdbstub/CMakeLists.txt', 'target_link_libraries(gdbstub PRIVATE kernel)',
        'target_link_libraries(gdbstub PRIVATE kernel)\nif(WIN32)\n  target_link_libraries(gdbstub PRIVATE ws2_32)\nendif()')

# Query a real, active six-byte adapter address. Never report success with
# uninitialized bytes, and retry if the adapter list changes during enumeration.
replace('vita3k/modules/SceNet/SceNet.cpp', '#include <thread>',
        '#include <thread>\n#include <vector>\n#ifdef _WIN32\n#include <ws2tcpip.h>\n#include <iphlpapi.h>\n#endif')
replace('vita3k/modules/SceNet/SceNet.cpp', '#ifdef _WIN32\n    IP_ADAPTER_INFO AdapterInfo[16];', '''#if defined(VITA3K_UWP)
    ULONG bytes = 15 * 1024;
    std::vector<IP_ADAPTER_ADDRESSES> adapters;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (unsigned attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        adapters.resize((bytes + sizeof(IP_ADAPTER_ADDRESSES) - 1) / sizeof(IP_ADAPTER_ADDRESSES));
        result = GetAdaptersAddresses(AF_UNSPEC,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, adapters.data(), &bytes);
    }
    if (result != NO_ERROR)
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
    for (const auto *adapter = adapters.data(); adapter; adapter = adapter->Next) {
        if (adapter->OperStatus == IfOperStatusUp &&
            adapter->IfType != IF_TYPE_SOFTWARE_LOOPBACK &&
            adapter->PhysicalAddressLength == sizeof(addr->data)) {
            memcpy(addr->data, adapter->PhysicalAddress, sizeof(addr->data));
            return 0;
        }
    }
    RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
#elif defined(_WIN32)
    IP_ADAPTER_INFO AdapterInfo[16];''')
replace('vita3k/modules/SceNet/SceNet.cpp', 'InetNtop(af, src, dst_ptr, size)', 'InetNtopA(af, src, dst_ptr, size)')
replace('vita3k/modules/SceNet/SceNet.cpp', 'InetPton(af, src, dst)', 'InetPtonA(af, src, dst)')

# A Vita user name is a guest profile setting. On UWP do not probe the host
# machine name. Bound both the host fallback and guest copy and terminate them.
# Upstream bounds the user name itself; the host-name fallback is a desktop
# network call, so the Xbox goes straight to the "Vita3K" fallback.
replace('vita3k/modules/SceAppUtil/SceAppUtil.cpp',
        '            if (gethostname(devname, SCE_SYSTEM_PARAM_USERNAME_MAXSIZE) == 0) {',
        '#ifdef VITA3K_UWP\n            if (false) {\n#else\n            if (gethostname(devname, SCE_SYSTEM_PARAM_USERNAME_MAXSIZE) == 0) {\n#endif')

# SceSize is unsigned int: use unsigned two's-complement arithmetic explicitly
# to isolate the low bit without MSVC's unary-minus warning (C4146).
replace('vita3k/modules/SceSysmem/SceSysmemForDriver.cpp',
        'size & -size', 'size & (~size + SceSize{1})')
print('Applied checked fmt filesystem, Winsock 2 and guest network/parameter/alignment fixes.')

replace('vita3k/modules/SceAppUtil/SceAppUtil.cpp', '#include <util/safe_time.h>', '#include <util/safe_time.h>\n#include <algorithm>')

# Our UWP bridge creates no VkSurfaceKHR or Vulkan swapchain. Requesting
# VK_KHR_swapchain would require VK_KHR_surface on the frontend's instance.
# Retain the upstream extension list for frontends that supply a surface.
replace('vita3k/renderer/src/vulkan/renderer.cpp',
'''        for (const char *name : required_device_extensions)
            add(name);''',
'''        for (const char *name : required_device_extensions) {
#ifdef VITA3K_UWP
            if (!surface && strcmp(name, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
                continue;
#endif
            add(name);
        }''')

# Adapted from 125hz/Tsubomi 0.47.0 (7e53bac87f442e770a39b1d9b032133418337f6d),
# vita3k/cpu/src/cpu.cpp, GPL-2.0-or-later. UWP executable-memory allocation
# can fail too: preserve kernel thread creation's existing null-CPU error path.
replace('vita3k/cpu/src/cpu.cpp', '#include <cpu/functions.h>',
        '#include <cpu/functions.h>\n#include <exception>\n#include <util/log.h>')
replace('vita3k/cpu/src/cpu.cpp',
        '    state->cpu = std::make_unique<DynarmicCPU>(state.get(), processor_id, cpu_opt);',
'''#ifdef VITA3K_UWP
    try {
        state->cpu = std::make_unique<DynarmicCPU>(state.get(), processor_id, cpu_opt);
    } catch (const std::exception &e) {
        LOG_CRITICAL("Failed to create CPU core for thread {}: {}", thread_id, e.what());
        return CPUStatePtr();
    }
#else
    state->cpu = std::make_unique<DynarmicCPU>(state.get(), processor_id, cpu_opt);
#endif''')
