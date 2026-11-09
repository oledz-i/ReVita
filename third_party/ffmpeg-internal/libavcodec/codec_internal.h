/*
 * Reduced copy of FFmpeg n6.1.1 libavcodec/codec_internal.h.
 *
 * Why this exists: Vita3K's AAC decoder (vita3k/codec/src/aac.cpp) calls the
 * decoder callback of FFmpeg's internal FFCodec struct directly. vcpkg (and every
 * normal FFmpeg install) only ships the *public* headers, so the include fails
 * with C1083. Only the struct layout and ffcodec() are needed, and the layout
 * has no configure-dependent members, so it is taken verbatim from the exact
 * FFmpeg release the build pins (vcpkg ffmpeg 6.1.1). Keep these in sync:
 * if the FFmpeg override in patch-vita3k-uwp.py changes version, re-copy the
 * struct from that tag.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVCODEC_CODEC_INTERNAL_H
#define AVCODEC_CODEC_INTERNAL_H

#include <stdint.h>

#include "libavutil/attributes.h"
#include "libavcodec/codec.h"
#include "libavcodec/version.h"
#if LIBAVCODEC_VERSION_INT != AV_VERSION_INT(60, 31, 102)
#error The private AAC decoder ABI requires FFmpeg n6.1.1 (libavcodec 60.31.102)
#endif

/**
 * FFCodec.codec_tags termination value
 */
#define FF_CODEC_TAGS_END -1

typedef struct FFCodecDefault {
    const char *key;
    const char *value;
} FFCodecDefault;

struct AVCodecContext;
struct AVSubtitle;
struct AVPacket;

enum FFCodecType {
    /* The codec is a decoder using the decode callback;
     * audio and video codecs only. */
    FF_CODEC_CB_TYPE_DECODE,
    /* The codec is a decoder using the decode_sub callback;
     * subtitle codecs only. */
    FF_CODEC_CB_TYPE_DECODE_SUB,
    /* The codec is a decoder using the receive_frame callback;
     * audio and video codecs only. */
    FF_CODEC_CB_TYPE_RECEIVE_FRAME,
    /* The codec is an encoder using the encode callback;
     * audio and video codecs only. */
    FF_CODEC_CB_TYPE_ENCODE,
    /* The codec is an encoder using the encode_sub callback;
     * subtitle codecs only. */
    FF_CODEC_CB_TYPE_ENCODE_SUB,
    /* The codec is an encoder using the receive_packet callback;
     * audio and video codecs only. */
    FF_CODEC_CB_TYPE_RECEIVE_PACKET,
};

typedef struct FFCodec {
    /**
     * The public AVCodec. See codec.h for it.
     */
    AVCodec p;

    /**
     * Internal codec capabilities FF_CODEC_CAP_*.
     */
    unsigned caps_internal:29;

    /**
     * This field determines the type of the codec (decoder/encoder)
     * and also the exact callback cb implemented by the codec.
     * cb_type uses enum FFCodecType values.
     */
    unsigned cb_type:3;

    int priv_data_size;
    /**
     * Copy necessary context variables from a previous thread context to the current one.
     */
    int (*update_thread_context)(struct AVCodecContext *dst, const struct AVCodecContext *src);

    /**
     * Copy variables back to the user-facing context
     */
    int (*update_thread_context_for_user)(struct AVCodecContext *dst, const struct AVCodecContext *src);

    /**
     * Private codec-specific defaults.
     */
    const FFCodecDefault *defaults;

    /**
     * Initialize codec static data, called from av_codec_iterate().
     */
    void (*init_static_data)(struct FFCodec *codec);

    int (*init)(struct AVCodecContext *);

    union {
        /**
         * Decode to an AVFrame.
         * cb is in this state if cb_type is FF_CODEC_CB_TYPE_DECODE.
         *
         * @return amount of bytes read from the packet on success,
         *         negative error code on failure
         */
        int (*decode)(struct AVCodecContext *avctx, struct AVFrame *frame,
                      int *got_frame_ptr, struct AVPacket *avpkt);
        int (*decode_sub)(struct AVCodecContext *avctx, struct AVSubtitle *sub,
                          int *got_frame_ptr, const struct AVPacket *avpkt);
        int (*receive_frame)(struct AVCodecContext *avctx, struct AVFrame *frame);
        int (*encode)(struct AVCodecContext *avctx, struct AVPacket *avpkt,
                      const struct AVFrame *frame, int *got_packet_ptr);
        int (*encode_sub)(struct AVCodecContext *avctx, uint8_t *buf,
                          int buf_size, const struct AVSubtitle *sub);
        int (*receive_packet)(struct AVCodecContext *avctx, struct AVPacket *avpkt);
    } cb;

    int (*close)(struct AVCodecContext *);

    /**
     * Flush buffers.
     * Will be called when seeking
     */
    void (*flush)(struct AVCodecContext *);

    /**
     * Decoding only, a comma-separated list of bitstream filters to apply to
     * packets before decoding.
     */
    const char *bsfs;

    /**
     * Array of pointers to hardware configurations supported by the codec,
     * or NULL if no hardware supported.  The array is terminated by a NULL
     * pointer.
     */
    const struct AVCodecHWConfigInternal *const *hw_configs;

    /**
     * List of supported codec_tags, terminated by FF_CODEC_TAGS_END.
     */
    const uint32_t *codec_tags;
} FFCodec;

static av_always_inline const FFCodec *ffcodec(const AVCodec *codec)
{
    return (const FFCodec*)codec;
}

#endif /* AVCODEC_CODEC_INTERNAL_H */
