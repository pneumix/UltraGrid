/**
 * @file   lavc_common.c
 * @author Martin Pulec     <pulec@cesnet.cz>
 * @author Martin Piatka    <445597@mail.muni.cz>
 */
/*
 * Copyright (c) 2013-2023 CESNET, z. s. p. o.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, is permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of CESNET nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * @file
 * References:
 * 1. [v210](https://wiki.multimedia.cx/index.php/V210)
 *
 * @todo
 * Some conversions to RGBA ignore RGB-shifts - either fix that or deprecate RGB-shifts
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#endif // HAVE_CONFIG_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "host.h"
#include "libavcodec/lavc_common.h"
#include "video.h"

#define MOD_NAME "[lavc_common] "

//
// UG <-> FFMPEG format translations
//
static const struct {
        enum AVCodecID av;
        codec_t uv;
} av_to_uv_map[] = {
        { AV_CODEC_ID_H264, H264 },
        { AV_CODEC_ID_HEVC, H265 },
        { AV_CODEC_ID_MJPEG, MJPG },
        { AV_CODEC_ID_JPEG2000, J2K },
        { AV_CODEC_ID_VP8, VP8 },
        { AV_CODEC_ID_VP9, VP9 },
        { AV_CODEC_ID_HUFFYUV, HFYU },
        { AV_CODEC_ID_FFV1, FFV1 },
        { AV_CODEC_ID_AV1, AV1 },
        { AV_CODEC_ID_PRORES, PRORES },
};

codec_t get_av_to_ug_codec(enum AVCodecID av_codec)
{
        for (unsigned int i = 0; i < sizeof av_to_uv_map / sizeof av_to_uv_map[0]; ++i) {
                if (av_to_uv_map[i].av == av_codec) {
                        return av_to_uv_map[i].uv;
                }
        }
        return VIDEO_CODEC_NONE;
}

enum AVCodecID get_ug_to_av_codec(codec_t ug_codec)
{
        for (unsigned int i = 0; i < sizeof av_to_uv_map / sizeof av_to_uv_map[0]; ++i) {
                if (av_to_uv_map[i].uv == ug_codec) {
                        return av_to_uv_map[i].av;
                }
        }
        return AV_CODEC_ID_NONE;
}

//
// utility functions
//
void print_libav_error(int verbosity, const char *msg, int rc) {
        char errbuf[1024];
        av_strerror(rc, errbuf, sizeof(errbuf));

        log_msg(verbosity, "%s: %s\n", msg, errbuf);
}

void printf_libav_error(int verbosity, int rc, const char *msg, ...) {
        char message[1024];

        va_list ap;
        va_start(ap, msg);
        vsnprintf(message, sizeof message, msg, ap);
        va_end(ap);

        print_libav_error(verbosity, message, rc);
}

bool libav_codec_has_extradata(codec_t codec) {
        return codec == HFYU || codec == FFV1;
}

static inline int av_to_uv_log(int level) {
        level /= 8;
        if (level <= 0) { // av_quiet + av_panic
                return level + 1;
        }
        if (level <= 3) {
                return level;
        }
        return level + 1;
}

static inline int uv_to_av_log(int level) {
        level *= 8;
        if (level == 8 * LOG_LEVEL_QUIET) {
                return level - 8;
        }
        if (level <= 8 * LOG_LEVEL_NOTICE) { // LOG_LEVEL_NOTICE maps to AV_LOG_INFO
                return level;
        }
        return level - 8;
}

/**
 * Filters out annoying messages that should not be passed to UltraGrid logger,
 * eg. complains on JPEG APP markers that FFmpeg decoder almost doesn't use.
 * @returns 0 - should be printed; 1 - filtered
 */
static _Bool av_log_filter(const char *ff_module_name, const char *fmt) {
        if (ff_module_name && strcmp(ff_module_name, "mjpeg") == 0 && strstr(fmt, "APP") != NULL) {
                return 1;
        }
        return 0;
}

static void av_log_ug_callback(void *avcl, int av_level, const char *fmt, va_list vl) {
        int level = av_to_uv_log(av_level);
        if (level > log_level) {
                return;
        }
        // avcl handling is taken from av_log_default_callback
        AVClass* avc = avcl ? *(AVClass **) avcl : NULL;
        const char *ff_module_name = avc ? avc->item_name(avcl) : NULL;
        if (av_log_filter(ff_module_name, fmt)) {
                return;
        }
        static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
        static _Bool nl_presented = 1;
        char new_fmt[1024];
        pthread_mutex_lock(&lock);
        if (nl_presented) {
                if (ff_module_name) {
                        snprintf(new_fmt, sizeof new_fmt, "[lavc %s @ %p] %s", ff_module_name, avcl, fmt);
                } else {
                        snprintf(new_fmt, sizeof new_fmt, "[lavc] %s", fmt);
                }
                fmt = new_fmt;
        }
        nl_presented = fmt[strlen(fmt) - 1] == '\n';
        log_vprintf(level, fmt, vl);
        pthread_mutex_unlock(&lock);
}

ADD_TO_PARAM("lavcd-log-level",
                "* lavcd-log-level=<num>[U][D]\n"
                "  Set libavcodec log level (FFmpeg range semantics, unless 'U' suffix, then UltraGrid)\n"
                " - 'D' - use FFmpeg default log handler\n");
/// Sets specified log level either given explicitly or from UG-wide log_level
void ug_set_av_logging() {
        av_log_set_level(uv_to_av_log(log_level));
        av_log_set_callback(av_log_ug_callback);
        const char *param = get_commandline_param("lavcd-log-level");
        if (param == NULL) {
                return;
        }
        char *endptr = NULL;
        int av_log_level = strtol(param, &endptr, 10);
        if (endptr != param) {
                if (strchr(endptr, 'U') != NULL) {
                        av_log_level = uv_to_av_log(av_log_level);
                }
                av_log_set_level(av_log_level);
        }
        if (strchr(endptr, 'D') != NULL) {
                av_log_set_callback(av_log_default_callback);
        }
}

/// @returns subsampling in 'JabA' format (compatible with @ref get_subsamping)
int av_pixfmt_get_subsampling(enum AVPixelFormat fmt) {
        const struct AVPixFmtDescriptor *pd = av_pix_fmt_desc_get(fmt);
        if (pd->log2_chroma_w == 0 && pd->log2_chroma_h == 0) {
                return 4440;
        }
        if (pd->log2_chroma_w == 1 && pd->log2_chroma_h == 0) {
                return 4220;
        }
        if (pd->log2_chroma_w == 1 && pd->log2_chroma_h == 1) {
                return 4200;
        }
        return 0; // other (todo)
}

struct pixfmt_desc av_pixfmt_get_desc(enum AVPixelFormat pixfmt) {
        struct pixfmt_desc ret;
        const struct AVPixFmtDescriptor *avd = av_pix_fmt_desc_get(pixfmt);
        ret.depth = avd->comp[0].depth;
        ret.rgb = avd->flags & AV_PIX_FMT_FLAG_RGB;
        ret.subsampling = av_pixfmt_get_subsampling(pixfmt);
        return ret;
}


void lavd_flush(AVCodecContext *codec_ctx) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 37, 100)
        int ret = 0;
        ret = avcodec_send_packet(codec_ctx, NULL);
        if (ret != 0) {
                log_msg(LOG_LEVEL_WARNING, MOD_NAME "Unexpected return value %d\n",
                                ret);
        }
        AVFrame *frame = av_frame_alloc();
        do {
                ret = avcodec_receive_frame(codec_ctx, frame);
        } while (ret >= 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN));
        if (ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
                log_msg(LOG_LEVEL_WARNING, MOD_NAME "Unexpected return value %d\n",
                                ret);
        }
        av_frame_free(&frame);
#else
        UNUSED(codec_ctx);
#endif
}
void print_decoder_error(const char *mod_name, int rc) {
        char buf[1024];
        switch (rc) {
                case 0:
                        break;
                case EAGAIN:
                        log_msg(LOG_LEVEL_VERBOSE, "%s No frame returned - needs more input data.\n", mod_name);
                        break;
                case EINVAL:
                        log_msg(LOG_LEVEL_ERROR, "%s Decoder in invalid state!\n", mod_name);
                        break;
                default:
                        av_strerror(rc, buf, 1024);
                        log_msg(LOG_LEVEL_WARNING, "%s Error while decoding frame (rc == %d): %s.\n", mod_name, rc, buf);
                        break;
        }
}

bool pixfmt_has_420_subsampling(enum AVPixelFormat fmt){
        const AVPixFmtDescriptor *fmt_desc = av_pix_fmt_desc_get(fmt);

        return fmt_desc && (fmt_desc->log2_chroma_w == 1 && fmt_desc->log2_chroma_h == 1);
}

/// @retval true if all pixel formats have either 420 subsampling or are HW accelerated
bool pixfmt_list_has_420_subsampling(const enum AVPixelFormat *fmt){
        for(const enum AVPixelFormat *it = fmt; *it != AV_PIX_FMT_NONE; it++){
                const AVPixFmtDescriptor *fmt_desc = av_pix_fmt_desc_get(*it);
                if (!pixfmt_has_420_subsampling(*it) && !(fmt_desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
                        return false;
                }
        }

        return true;
}

/* vi: set expandtab sw=8: */
