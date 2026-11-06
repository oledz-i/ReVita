# Attribution and project identity

ReVita (formerly Vita3K-UWP) is an independent project maintained at https://github.com/oledz-i/Vita3K-UWP . It is not an official Vita3K release and is not endorsed by Sony or Microsoft.

The emulation is Vita3K, by the Vita3K team and contributors: https://github.com/Vita3K/Vita3K , built from WizzardSK's libretro core https://github.com/WizzardSK/vita3k-libretro . The pinned revisions are recorded in the lock files. `COPYING.txt` reproduces its GNU GPL version 2 license text.

Some game fixes (`scripts/core-patches/`) are ported from nckstwrt's Vita3K-Plus, https://github.com/nckstwrt/Vita3K-Plus , a GPL-2.0-or-later fork of Vita3K; they keep that license and their authors' copyright.

Original shell, scripts and artwork: Copyright (C) 2026 ReVita contributors. Licensed under GNU GPL version 2 or, at your option, any later version (SPDX: GPL-2.0-or-later). See COPYING.txt. Upstream source and third-party components retain their own copyright notices and licensing terms when integrated.

The ReVita name, mark (`branding/revita-mark.svg`), wordmark, app icons, tiles, splash screen and banner are original ReVita artwork. The wordmark is set in Inter Display (SIL Open Font License); the font itself is not distributed. The previous app icons used the upstream Vita3K logo, which is still preserved at `docs/assets/vita3k-logo.svg` and belongs to its upstream owners. PlayStation, PS Vita, Xbox and other product names remain the trademarks of their respective owners. No Sony or Microsoft endorsement is implied. ReVita includes no Sony firmware, keys or games; the in-app firmware download fetches Sony's system software from PlayStation's own update server on the user's console.

## OnyxEMU/Mesa Dozen Xbox renderer experiment

This branch reuses the publicly released OnyxEMU source from https://github.com/oledz-i/OnyxEMU at commit `78f2561a16fe7a65e59893f896611263f10575c5`. The separate Dozen source-build script invokes OnyxEMU's original GPL-3.0-or-later tooling and Mesa patch series (patch base `a4a47003a194effad8cabe727f74985e9417476f`), and the Vita3K Vulkan ICD startup logic was adapted from OnyxEMU `app/ONYX3DS/Emu/VulkanHost.cpp`. See `COPYING.GPLv3.txt` for the complete GPLv3 text and `COPYING.txt` for the original Vita3K GPLv2-or-later code. When distributing the combined modified GPLv2-or-later/GPLv3-or-later work, comply with GPLv3 terms; the original components retain their notices. Mesa's Dozen driver is MIT-licensed, its own notices remain in the pinned source checkout; Microsoft's DXIL DLL has separate distribution conditions from the DirectX Shader Compiler project. No proprietary PS Vita material is included.

Only the Vulkan ICD integration and Xbox UWP build workflow are reused: **Onyx's Nintendo 3DS GPU emulation and Azahar core cannot execute Vita GXM commands**. A Mesa Dozen DLL and a host Vulkan instance do not, alone, make Vita3K games boot.

## FFmpeg AAC compatibility header

`third_party/ffmpeg-internal/libavcodec/codec_internal.h` contains the FFCodec declaration from FFmpeg n6.1.1, supplied in gameplay-fix-3.zip and checked against the upstream source at https://github.com/FFmpeg/FFmpeg/tree/n6.1.1 . It retains FFmpeg's LGPL-2.1-or-later notice; the license is in `third_party/ffmpeg-internal/COPYING.LGPLv2.1`. The UWP build pins the matching FFmpeg version, and an explicit version guard rejects incompatible private layouts.
