# FFmpeg private AAC interface

`libavcodec/codec_internal.h` was supplied in gameplay-fix-3.zip. Its FFCodec
structure was compared token-for-token, excluding comments/whitespace, with
FFmpeg n6.1.1 at commit e38092ef9395d7049f871ef4d5411eb410e283e0:
https://github.com/FFmpeg/FFmpeg/blob/n6.1.1/libavcodec/codec_internal.h

The compile-time version guard requires libavcodec 60.31.102. The vcpkg override
must stay on FFmpeg 6.1.1 while this private interface is used. The header keeps
upstream's LGPL-2.1-or-later notice; the license text is included here.

Vita3K's AAC callback reports bytes consumed from a buffer that can contain
several raw AAC access units. Public send/receive alone does not expose that
count. The adapter retains this behavior and supplies an owned padded AVPacket,
initializes got_frame and rejects invalid counts/missing frames in release builds.

The actual patched decoder was linked against FFmpeg 6.1.1 and tested with
five generated raw AAC access units. It consumed 133, 230, 177, 135 and 7 bytes
and produced 20,480 PCM bytes identical to FFmpeg's ADTS decode output. Empty
and malformed input were rejected. This validates the native codec path, not
Xbox runtime loading or game execution.
