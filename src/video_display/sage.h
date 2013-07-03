/*
 * FILE:    video_display/sage.h
 * AUTHORS: Martin Benes     <martinbenesh@gmail.com>
 *          Lukas Hejtmanek  <xhejtman@ics.muni.cz>
 *          Petr Holub       <hopet@ics.muni.cz>
 *          Milos Liska      <xliska@fi.muni.cz>
 *          Jiri Matela      <matela@ics.muni.cz>
 *          Dalibor Matura   <255899@mail.muni.cz>
 *          Ian Wesley-Smith <iwsmith@cct.lsu.edu>
 *
 * Copyright (c) 2005-2209 CESNET z.s.p.o.
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
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 * 
 *      This product includes software developed by CESNET z.s.p.o.
 * 
 * 4. Neither the name of CESNET nor the names of its contributors may be used 
 *    to endorse or promote products derived from this software without specific
 *    prior written permission.
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
 *
 *
 */

#include <video_codec.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_SAGE_ID	0xba370a2f

struct audio_frame;

display_type_t		*display_sage_probe(void);
void 			*display_sage_init(char *fmt, unsigned int flags);
void 			 display_sage_run(void *state);
void 			 display_sage_done(void *state);
void 			 display_sage_finish(void *state);
struct video_frame	*display_sage_getf(void *state);
int  			 display_sage_putf(void *state, struct video_frame *frame,
                int nonblock);
int                      display_sage_reconfigure(void *state, struct video_desc desc);
int                      display_sage_get_property(void *state, int property, void *val, size_t *len);

int			 display_sage_handle_events(void);

void                     display_sage_put_audio_frame(void *state, struct audio_frame *frame);
int                      display_sage_reconfigure_audio(void *state, int quant_samples, int channels,
                int sample_rate);

#ifdef __cplusplus
}
#endif

