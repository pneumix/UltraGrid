/*
 * FILE:   vidcap_hdstation.c
 * AUTHOR: Colin Perkins <csp@isi.edu>
 *         Martin Benes     <martinbenesh@gmail.com>
 *         Lukas Hejtmanek  <xhejtman@ics.muni.cz>
 *         Petr Holub       <hopet@ics.muni.cz>
 *         Milos Liska      <xliska@fi.muni.cz>
 *         Jiri Matela      <matela@ics.muni.cz>
 *         Dalibor Matura   <255899@mail.muni.cz>
 *         Ian Wesley-Smith <iwsmith@cct.lsu.edu>
 *
 * Copyright (c) 2001-2003 University of Southern California
 * Copyright (c) 2005-2010 CESNET z.s.p.o.
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
 *      This product includes software developed by the University of Southern
 *      California Information Sciences Institute. This product also includes
 *      software developed by CESNET z.s.p.o.
 * 
 * 4. Neither the name of the University, Institute, CESNET nor the names of
 *    its contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING,
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
 * $Revision: 1.6 $
 * $Date: 2010/07/08 11:14:08 $
 *
 */

#include "host.h"
#include "config.h"
#include "config_unix.h"
#include "config_win32.h"

#ifdef HAVE_HDSTATION		/* From config.h */

#include "debug.h"
#include "video_types.h"
#include "video_capture.h"
#include "video_capture/hdstation.h"
#include "tv.h"
#include "dvs_clib.h"		/* From the DVS SDK */
#include "dvs_fifo.h"		/* From the DVS SDK */


struct vidcap_hdstation_state {
	sv_handle 	*sv;
	sv_fifo	 	*fifo;
	sv_fifo_buffer	*dma_buffer;
	char		*rtp_buffer;
	char		*tmp_buffer;
	int		 buffer_size;
	pthread_t	 thread_id;
	pthread_mutex_t	 lock;
	pthread_cond_t	 boss_cv;
	pthread_cond_t	 worker_cv;
	int		 boss_waiting;
	int		 worker_waiting;
	int		 work_to_do;
	char		*bufs[2];
	int		 bufs_index;
	int 		 mode;
};

static void *
vidcap_hdstation_grab_thread(void *arg)
{
	int		res;
	struct vidcap_hdstation_state *s = (struct vidcap_hdstation_state *) arg;

	while (1) {
		s->dma_buffer = NULL;
		res = sv_fifo_vsyncwait(s->sv, s->fifo);
		//if (res != SV_OK) {
	//		printf("Unable to ysyncwait %s\n", sv_geterrortext(res));
	//	}

		res = sv_fifo_getbuffer(s->sv, s->fifo, &(s->dma_buffer), NULL, SV_FIFO_FLAG_VIDEOONLY|SV_FIFO_FLAG_FLUSH);
		if (res != SV_OK) {
			printf("Unable to getbuffer %s\n", sv_geterrortext(res));
			continue;
		}
		s->bufs_index = (s->bufs_index + 1) % 2;
		s->dma_buffer->dma.addr = s->bufs[s->bufs_index];
		s->dma_buffer->dma.size = s->buffer_size;

		res = sv_fifo_putbuffer(s->sv, s->fifo, s->dma_buffer, NULL);
		if (res != SV_OK) {
			printf("Unable to putbuffer %s\n", sv_geterrortext(res));
		}

		pthread_mutex_lock(&(s->lock));

		while (s->work_to_do == FALSE) {
			s->worker_waiting = TRUE;
			pthread_cond_wait(&(s->worker_cv), &(s->lock));
			s->worker_waiting = FALSE;
		}

		s->tmp_buffer = s->dma_buffer->dma.addr;
		s->work_to_do = FALSE;

		if (s->boss_waiting) {
			pthread_cond_signal(&(s->boss_cv));
		}
		pthread_mutex_unlock(&(s->lock));
	}
	return NULL;
}

/* External API ***********************************************************************************/

/* HELP */

int
hdstation_help(struct vidcap_hdstation_state *s)
{

        sv_handle               *sv = s->sv;
        sv_rasterheader         current;

        int nrasters;
        int res;
        int i;
	
	
	printf("set -g  mode:colormode(8|10) \n");
        res = sv_raster_status(sv, -1, &current, sizeof(current), &nrasters, 0);
        for(i = 0; (res == SV_OK) && (i < nrasters); i++) {
                sv_rasterheader raster;
                res = sv_raster_status(sv, i, &raster, sizeof(raster), NULL, 0);
                if(res == SV_OK) {
                        //pozriet aj raster.svind
                        printf("mode:%d  SV_MODE__%s\n",raster.svind,raster.name);
                }
         }
        
	return res;
        
}                    

int set_xy_size(sv_handle *sv){
	sv_info info;
	int res;

	res=sv_status(sv,&info);
	if(res==SV_OK){
		printf("Current video size %dx%d \n",info.xsize,info.ysize);
		hd_size_x=info.xsize;
		hd_size_y=info.ysize;
	}
	else {
		printf("Error:sv_status{} failed %d '%s' \n",res,sv_geterrortext(res));
	}

	return res;
}                                       

/* SETTINGS  */

int
settings_init(struct vidcap_hdstation_state *s, char *fmt)
{

        if(strcmp(fmt, "help") == 0) {
                hdstation_help(s);
                return 0;
        }


        char *tmp;

	//set mode
        tmp = strtok(fmt, ":");
        if(!tmp) {
                fprintf(stderr, "Wrong config %s\n", fmt);
                return 0;
        }
        s->mode = atoi(tmp);

        tmp = strtok(NULL, ":");		
        if(!tmp) {
		printf("set -g  mode:colormode(8|10) \n");
                fprintf(stderr, "Wrong config %s\n", fmt);
                return 0;
        }
	if (atoi(tmp) == 8){
		hd_color_bpp=2;
		printf("Current colormode %d \n",hd_color_bpp);
	}
	else if(atoi(tmp) == 10){ 
		hd_color_bpp=3;
		printf("Current colormode %d \n",hd_color_bpp);
	}
	else{
		printf("wrong colormode set -g mode:colormode(8 or 10) \n");
                fprintf(stderr, "Wrong config %s\n", tmp);
		return 0;
	} 
	

        return 1;
}

struct vidcap_type *
vidcap_hdstation_probe(void)
{
	struct vidcap_type	*vt;
	sv_handle 		*sv;

	sv = sv_open("");
	if (sv == NULL) {
		debug_msg("Cannot probe HDTV capture device\n");
		return NULL;
	}
	if (sv_videomode(sv, hd_video_mode | SV_MODE_AUDIO_NOAUDIO) != SV_OK) {
		sv_close(sv);
		return NULL;
	}
	sv_close(sv);

	vt = (struct vidcap_type *) malloc(sizeof(struct vidcap_type));
	if (vt != NULL) {
		vt->id          = VIDCAP_HDSTATION_ID;
		vt->name        = "hdtv";
		vt->description = "DVS HDstation (SMPTE 274M/25i)";
		vt->width       = hd_size_x;
		vt->height      = hd_size_y;
		vt->colour_mode = YUV_422;
	}
	return vt;
}

void *
vidcap_hdstation_init(char *fmt)
{
	int fps = atoi(fmt); //What is fps good for?
	struct vidcap_hdstation_state	*s;
	int 				 res;

	//assert(fps == 60);
    UNUSED(fmt);

	s = (struct vidcap_hdstation_state *) malloc(sizeof(struct vidcap_hdstation_state));
	if (s == NULL) {
	//	debug_msg("Unable to allocate HDstation state\n",fps);
		debug_msg("Unable to allocate HDstation state\n");
		return NULL;
	}

	s->sv = sv_open("");
	if (s->sv == NULL) {
		printf("Unable to open grabber: sv_open() failed (no card present or driver not loaded?)\n");
		free(s);
		return NULL;
	}

	//SET UP mode
	if(settings_init(s, fmt) == 0) {
                free(s);
                return NULL;
        }

	//DO not use SV_MODE_FLAG_PACKED, SV_MODE_STORAGE_FRAME. They are not supported
	hd_video_mode  = s->mode | SV_MODE_NBIT_8B | SV_MODE_COLOR_YUV422 | SV_MODE_ACTIVE_STREAMER  ;


	//set video mode
	res = sv_videomode(s->sv, hd_video_mode | SV_MODE_AUDIO_NOAUDIO);
	
	
	if (res != SV_OK) {
		goto error;
	}
	res = sv_black(s->sv);
	if (res != SV_OK) {
		goto error;
	}
	res = sv_fifo_init(s->sv, &(s->fifo), 1, 1, 1, 0, 0);
	if (res != SV_OK) {
		goto error;
	}
	res = sv_fifo_start(s->sv, s->fifo);
	if (res != SV_OK) {
		goto error;
	}

	pthread_mutex_init(&(s->lock), NULL);
	pthread_cond_init(&(s->boss_cv), NULL);
	pthread_cond_init(&(s->worker_cv), NULL);

	//set hd_size_x, hd_size_y
	set_xy_size(s->sv);
	
	s->buffer_size    = hd_color_bpp * hd_size_x * hd_size_y;
	s->rtp_buffer     = NULL;
	s->dma_buffer     = NULL;
	s->tmp_buffer     = NULL;
	s->boss_waiting   = FALSE;
	s->worker_waiting = FALSE;
	s->work_to_do     = FALSE;
	s->bufs[0] = malloc(s->buffer_size);
	s->bufs[1] = malloc(s->buffer_size);
	s->bufs_index = 0;

	if (pthread_create(&(s->thread_id), NULL, vidcap_hdstation_grab_thread, s) != 0) {
		perror("Unable to create grabbing thread");
		return NULL;
	}

	debug_msg("HDstation capture device enabled\n");
	return s;
error:
	free(s);
	printf("Chyba %s\n",sv_geterrortext(res));
	debug_msg("Unable to open grabber: %s\n", sv_geterrortext(res));
	return NULL;
}

void
vidcap_hdstation_done(void *state)
{
	struct vidcap_hdstation_state *s = (struct vidcap_hdstation_state *) state;

	sv_fifo_free(s->sv, s->fifo);
	sv_close(s->sv);
	free(s);
}

struct video_frame *
vidcap_hdstation_grab(void *state)
{
	struct vidcap_hdstation_state 	*s = (struct vidcap_hdstation_state *) state;
	struct video_frame		*vf;

	pthread_mutex_lock(&(s->lock));

	/* Wait for the worker to finish... */
	while (s->work_to_do) {
		s->boss_waiting = TRUE;
		pthread_cond_wait(&(s->boss_cv), &(s->lock));
		s->boss_waiting = FALSE;
	}

	/* ...and give it more to do... */
	s->rtp_buffer = s->tmp_buffer;
	s->work_to_do = TRUE;

	/* ...and signal the worker... */
	if (s->worker_waiting) {
		pthread_cond_signal(&(s->worker_cv));
	}

	pthread_mutex_unlock(&(s->lock));

	if (s->rtp_buffer != NULL) {
		vf = (struct video_frame *) malloc(sizeof(struct video_frame));
		if (vf != NULL) {
			vf->colour_mode = YUV_422;
			vf->width       = hd_size_x;
			vf->height      = hd_size_y;
			vf->data        = s->rtp_buffer;
			vf->data_len	= hd_size_x * hd_size_y * hd_color_bpp;
		}
		return vf;
	}
	return NULL;
}

#endif /* HAVE_HDSTATION */

