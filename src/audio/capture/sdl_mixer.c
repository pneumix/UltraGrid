/**
 * @file   audio/capture/sdl_mixer.c
 * @author Martin Pulec     <pulec@cesnet.cz>
 */
/*
 * Copyright (c) 2011-2023 CESNET, z. s. p. o.
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif // defined HAVE_CONFIG_H
#include "config_unix.h"
#include "config_win32.h"

#ifdef HAVE_SDL2
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#else
#include <SDL/SDL.h>
#include <SDL/SDL_mixer.h>
#endif // defined HAVE_SDL2

#include <stdio.h>

#include "audio/audio_capture.h"
#include "audio/types.h"
#include "debug.h"
#include "host.h"
#include "lib_common.h"
#include "song1.h"
#include "types.h"
#include "utils/color_out.h"
#include "utils/fs.h"
#include "utils/macros.h"
#include "utils/ring_buffer.h"

#define DEFAULT_SDL_MIXER_BPS 2
#define DEFAULT_MIX_MAX_VOLUME (MIX_MAX_VOLUME / 4)
#define SDL_MIXER_SAMPLE_RATE 48000
#define MOD_NAME "[SDL_mixer] "

struct state_sdl_mixer_capture {
        struct audio_frame audio;
        struct ring_buffer *sdl_mixer_buf;
        int volume;
        char *req_filename;
};

static void audio_cap_sdl_mixer_done(void *state);

static void audio_cap_sdl_mixer_probe(struct device_info **available_devices, int *count, void (**deleter)(void *))
{
        *deleter = free;
        *count = 1;
        *available_devices = calloc(1, sizeof **available_devices);
        strncat((*available_devices)[0].dev, "sdl_mixer", sizeof (*available_devices)[0].dev - 1);
        strncat((*available_devices)[0].name, "Sample midi song", sizeof (*available_devices)[0].name - 1);
}

static void sdl_mixer_audio_callback(int chan, void *stream, int len, void *udata)
{
        UNUSED(chan);
        struct state_sdl_mixer_capture *s = udata;

        ring_buffer_write(s->sdl_mixer_buf, stream, len);
        memset(stream, 0, len); // do not playback anything to PC output
}

static int parse_opts(struct state_sdl_mixer_capture *s, char *cfg) {
        char *save_ptr = NULL;
        char *item = NULL;
        while ((item = strtok_r(cfg, ":", &save_ptr)) != NULL) {
                cfg = NULL;
                if (strcmp(item, "help") == 0) {
                        color_printf(TBOLD("sdl_mixer") " is a capture device capable playing various audio files like FLAC,\n"
                                        "MIDI, mp3, Vorbis or WAV.\n\n"
                                        "The main functional difference to " TBOLD("file") " video capture (that is able to play audio\n"
                                        "files as well) is the support for " TBOLD("MIDI") " (and also having one song bundled).\n\n");
                        color_printf("Usage:\n");
                        color_printf(TBOLD(TRED("\t-s sdl_mixer") "[:file=<filename>][:volume=<vol>]") "\n");
                        color_printf("where\n");
                        color_printf(TBOLD("\t<filename>") " - name of file to be used\n");
                        color_printf(TBOLD("\t<vol>     ") " - volume [0..%d], default %d\n", MIX_MAX_VOLUME, DEFAULT_MIX_MAX_VOLUME);
                        color_printf("\n");
                        color_printf(TBOLD("SDL_SOUNDFONTS") " - environment variable with path to sound fonts for MIDI playback (eg. freepats)\n\n");
                        return 1;
                }
                if (strstr(item, "file=") == item) {
                        s->req_filename = strdup(strchr(item, '=') + 1);
                } else if (strstr(item, "volume=") == item) {
                        s->volume = atoi(strchr(item, '=') + 1);
                } else {
                        log_msg(LOG_LEVEL_ERROR, MOD_NAME "Wrong option: %s!\n", item);
                        color_printf("Use " TBOLD("-s sdl_mixer:help") " to see available options.\n");
                        return -1;
                }
        }
        return 0;
}

static const char *load_song1() {
        const char *filename = NULL;
        FILE *f = get_temp_file(&filename);
        if (f == NULL) {
                perror("fopen audio");
                return NULL;
        }
        size_t nwritten = fwrite(song1, sizeof song1, 1, f);
        fclose(f);
        if (nwritten != 1) {
                unlink(filename);
                return NULL;
        }
        return filename;
}

static void try_open_soundfont() {
        if (getenv("SDL_SOUNDFONT") != NULL) {
                return;
        }
        const char *root = get_install_root();
        const char *suffix = "/share/soundfonts/default.sf2";
        const size_t len = strlen(root) + strlen(suffix) + 1;
        char path[len];
        strncpy(path, root, len - 1);
        strncat(path, suffix, len - strlen(path) - 1);
        FILE *f = fopen(path, "r");
        if (!f) {
                return;
        }
        fclose(f);
        Mix_SetSoundFonts(path);
}

static void * audio_cap_sdl_mixer_init(struct module *parent, const char *cfg)
{
        UNUSED(parent);
        SDL_Init(SDL_INIT_AUDIO);

        struct state_sdl_mixer_capture *s = calloc(1, sizeof *s);
        s->volume = DEFAULT_MIX_MAX_VOLUME;
        char *ccfg = strdup(cfg);
        int ret = parse_opts(s, ccfg);
        free(ccfg);
        if (ret != 0) {
                free(s);
                return ret < 0 ? NULL : INIT_NOERR;
        }

        s->audio.bps = audio_capture_bps ? audio_capture_bps : DEFAULT_SDL_MIXER_BPS;
        s->audio.ch_count = audio_capture_channels > 0 ? audio_capture_channels : DEFAULT_AUDIO_CAPTURE_CHANNELS;
        s->audio.sample_rate = SDL_MIXER_SAMPLE_RATE;

        int audio_format = 0;
        switch (s->audio.bps) {
                case 1: audio_format = AUDIO_S8; break;
                case 2: audio_format = AUDIO_S16LSB; break;
                case 4: audio_format = AUDIO_S32LSB; break;
                default: UG_ASSERT(0 && "BPS can be only 1, 2 or 4");
        }

        if( Mix_OpenAudio(SDL_MIXER_SAMPLE_RATE, audio_format,
                                s->audio.ch_count, 4096 ) == -1 ) {
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "error initalizing sound: %s\n", Mix_GetError());
                goto error;
        }
        const char *filename = s->req_filename;
        if (!filename) {
                filename = load_song1();
                if (!filename) {
                        goto error;
                }
        }
        try_open_soundfont();
        Mix_Music *music = Mix_LoadMUS(filename);
        if (filename != s->req_filename) {
                unlink(filename);
        }
        if (music == NULL) {
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "error loading file: %s\n", Mix_GetError());
                goto error;
        }

        s->audio.max_size =
                s->audio.data_len = s->audio.ch_count * s->audio.bps * s->audio.sample_rate /* 1 sec */;
        s->audio.data = malloc(s->audio.data_len);
        s->sdl_mixer_buf = ring_buffer_init(s->audio.data_len);

        // register grab as a postmix processor
        if (!Mix_RegisterEffect(MIX_CHANNEL_POST, sdl_mixer_audio_callback, NULL, s)) {
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "Mix_RegisterEffect: %s\n", Mix_GetError());
                goto error;
        }

        Mix_VolumeMusic(s->volume);
        if(Mix_PlayMusic(music,-1)==-1){
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "error playing file: %s\n", Mix_GetError());
                goto error;
        }

        log_msg(LOG_LEVEL_NOTICE, MOD_NAME "Initialized SDL_mixer\n");

        return s;
error:
        audio_cap_sdl_mixer_done(s);
        return NULL;
}

static struct audio_frame *audio_cap_sdl_mixer_read(void *state)
{
        struct state_sdl_mixer_capture *s = state;
        s->audio.data_len = ring_buffer_read(s->sdl_mixer_buf, s->audio.data, s->audio.max_size);
        if (s->audio.data_len == 0) {
                return NULL;
        }
        return &s->audio;
}

static void audio_cap_sdl_mixer_done(void *state)
{
        Mix_HaltMusic();
        Mix_CloseAudio();
        struct state_sdl_mixer_capture *s = state;
        free(s->audio.data);
        free(s->req_filename);
        free(s);
}

static const struct audio_capture_info acap_sdl_mixer_info = {
        audio_cap_sdl_mixer_probe,
        audio_cap_sdl_mixer_init,
        audio_cap_sdl_mixer_read,
        audio_cap_sdl_mixer_done
};

REGISTER_MODULE(sdl_mixer, &acap_sdl_mixer_info, LIBRARY_CLASS_AUDIO_CAPTURE, AUDIO_CAPTURE_ABI_VERSION);

