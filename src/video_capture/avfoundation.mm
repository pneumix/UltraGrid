/**
 * @file   video_capture/avfoundation.mm
 * @author Martin Pulec     <pulec@cesnet.cz>
 */
/*
 * Copyright (c) 2014-2023 CESNET z.s.p.o.
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
#include "config_unix.h"
#endif

#include "debug.h"
#include "lib_common.h"
#include "utils/color_out.h"
#include "video.h"
#include "video_capture.h"

#import <AVFoundation/AVFoundation.h>
#include <AppKit/NSApplication.h>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>

#define MOD_NAME "[AVFoundation] "

#define NSAppKitVersionNumber10_8 1187
#define NSAppKitVersionNumber10_9 1265

using std::string;

namespace vidcap_avfoundation {
std::unordered_map<std::string, NSString *> preset_to_av = {
        { "high", AVCaptureSessionPresetHigh },
        { "medium", AVCaptureSessionPresetMedium },
        { "low", AVCaptureSessionPresetLow },
        { "VGA", AVCaptureSessionPreset640x480 },
        { "HD", AVCaptureSessionPreset1280x720 },
};

std::unordered_map<CMPixelFormatType, std::tuple<codec_t, int, int, int>> av_to_uv = {
        {kCVPixelFormatType_32ARGB, {RGBA, 8, 16, 24}},
        {kCVPixelFormatType_32BGRA, {RGBA, 16, 8, 0}},
        {kCVPixelFormatType_24RGB, {RGB, 0, 8, 16}},
	//kCMPixelFormat_16BE555
	//kCMPixelFormat_16BE565
	//kCMPixelFormat_16LE555
	//kCMPixelFormat_16LE565
	//kCMPixelFormat_16LE5551
        {kCVPixelFormatType_422YpCbCr8, {UYVY, 0, 0, 0}},
        {kCVPixelFormatType_422YpCbCr8_yuvs, {YUYV, 0, 0, 0}},
	//kCMPixelFormat_444YpCbCr8
	//kCMPixelFormat_4444YpCbCrA8
	//kCMPixelFormat_422YpCbCr16
        {kCVPixelFormatType_422YpCbCr10, {v210, 0, 0, 0}},
	//kCMPixelFormat_444YpCbCr10
	//kCMPixelFormat_8IndexedGray_WhiteIsZero
};

constexpr int MAX_CAPTURE_QUEUE_SIZE = 2;
} // namespace vidcap_avfoundation

using namespace std;
using namespace vidcap_avfoundation;
using std::chrono::milliseconds;

@interface vidcap_avfoundation_state : NSObject
{
        AVCaptureDevice *m_device;
        AVCaptureSession *m_session;
        mutex m_lock;
        condition_variable m_frame_ready;
        queue<struct video_frame *> m_queue;
	chrono::steady_clock::time_point m_t0;
        double m_fps_req;
	int m_frames;
}

- (void)captureOutput:(AVCaptureOutput *)captureOutput didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
fromConnection:(AVCaptureConnection *)connection;
- (struct video_frame *) grab;
@end

@interface vidcap_avfoundation_state () <AVCaptureVideoDataOutputSampleBufferDelegate>
@end

@implementation vidcap_avfoundation_state

/// sort devices according to UID because macOS orders last used device first (doesn't keep stable order)
+(NSArray *) devices
{
   return [[AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo] sortedArrayUsingComparator: ^(AVCaptureDevice* o1, AVCaptureDevice* o2) {
          return [[o1 uniqueID] compare: [o2 uniqueID]];
   }];
}

+ (void)usage: (BOOL) verbose
{
        col() << "AV Foundation capture usage:" << "\n";
        col() << "\t" << SBOLD(SRED("-t avfoundation") << "[:device=<idx>|:name=<name>|:uid=<uid>][:preset=<preset>|:mode=<mode>[:fps=<fps>|:fr_idx=<fr_idx>]]") << "\n";
        col() << "\n";
        col() << "where:\n";
        col() << "\t" << SBOLD("<idx>") << " represents a device index in a list below\n";
        col() << "\t" << SBOLD("<name>") << " of the device\n";
        col() << "\t" << SBOLD("<uid>") << " is a device unique identifier\n";
        col() << "\t" << SBOLD("<fps>") << " is a number of frames per second (can be a number with a decimal point)\n";
        col() <<  "\t" << SBOLD("<fr_idx>") << " is index of frame rate obtained from '-t avfoundation:fullhelp'\n";
        col() <<"\t" << SBOLD("<preset>") << " may be " << SBOLD("\"low\"") << ", " << SBOLD("\"medium\"") << ", " << SBOLD("\"high\"") << ", " << SBOLD("\"VGA\"") << " or " << SBOLD("\"HD\"") << "\n";
        col() << "\n";
        col() << "All other parameters are represented by appropriate numeric index." << "\n\n";
        col() << "Examples:" << "\n";
        col() << "\t" << SBOLD("-t avfoundation") << "\n";
        col() << "\t" << SBOLD("-t avfoundation:preset=high") << "\n";
        col() << "\t" << SBOLD("-t avfoundation:device=0:preset=high") << "\n";
        col() << "\t" << SBOLD("-t avfoundation:device=0:mode=24:fps=30") << " (advanced)" << "\n";
        col() << "\n";
        col() << "Available AV foundation capture devices and modes:" << "\n";
        int i = 0;
        // deprecated rewrite example: https://github.com/flutter/plugins/blob/e85f8ac1502db556e03953794ad0aa9149ddb02a/packages/camera/camera_avfoundation/ios/Classes/CameraPlugin.m#L108
        // but the new API doesn't seem to be eligible since individual AVCaptureDeviceType must be enumerated, perhaps better to keep the old one
        for (AVCaptureDevice *device in [vidcap_avfoundation_state devices]) {
                int j = 0;
                string default_dev = device == [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo] ? "*" : "";
                col() << default_dev << i << ": " << SBOLD([[device localizedName] UTF8String]) << " (uid: " << [[device uniqueID] UTF8String] << ")\n";
                for ( AVCaptureDeviceFormat *format in [device formats] ) {
                        CMVideoFormatDescriptionRef formatDesc = [format formatDescription];
                        FourCharCode fcc = CMFormatDescriptionGetMediaSubType(formatDesc);
                        CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(formatDesc);
                        FourCharCode fcc_host = CFSwapInt32BigToHost(fcc);

                        printf("\t%d: %.4s %dx%d", j, (const char *) &fcc_host, dim.width, dim.height);
                        if (verbose) {
                                cout << endl;
                                int k = 0;
                                for ( AVFrameRateRange *range in format.videoSupportedFrameRateRanges ) {
                                        cout << "\t\t" << k++ << ": " << range.minFrameRate << "-" << range.maxFrameRate << endl;
                                }
                        } else {
                                for ( AVFrameRateRange *range in format.videoSupportedFrameRateRanges ) {
                                        cout << " (max frame rate " << range.maxFrameRate << " FPS)";
                                        break;
                                }
                        }
                        printf("\n");
                        j++;
                }
                col() << "\n";
                i++;
        }
        col() << "(type '-t avfoundation:fullhelp' to see available framerates; device marked with an asterisk ('*') is default)" << "\n";
}

#ifdef __MAC_10_14
// http://anasambri.com/ios/accessing-camera-and-photos-in-ios.html
static void (^cb)(BOOL) = ^void(BOOL granted) {
        if (!granted) {
                dispatch_async(dispatch_get_main_queue(), ^{
                                //show alert
                                });
        }
};
#endif // defined __MAC_10_14

- (id)initWithParams: (NSDictionary *) params
{
        self = [super init];
        bool use_preset = true;

	m_t0 = chrono::steady_clock::now();
	m_frames = 0;
        m_fps_req = 0.0;

#ifdef __MAC_10_14
        AVAuthorizationStatus authorization_status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
        if (authorization_status == AVAuthorizationStatusRestricted ||
                        authorization_status == AVAuthorizationStatusDenied) {
                [NSException raise:@"Perimission denied" format:@"Application is not authorized to capture input!"];
        }
        if (authorization_status == AVAuthorizationStatusNotDetermined) {
                [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:cb];
        }
#endif // defined __MAC_10_14

        NSError *error = nil;

        m_session = [[AVCaptureSession alloc] init];
        // Add inputs and outputs.

        // Configure the session to produce lower resolution video frames, if your
        // processing algorithm can cope. We'll specify medium quality for the
        // chosen device.

        // Find a suitable AVCaptureDevice
        int device_idx = [params valueForKey:@"device"] ? [[params valueForKey:@"device"] intValue] : -1;
        NSString *device_uid = [params valueForKey:@"uid"];
        NSString *device_name = [params valueForKey:@"name"];
        if (device_idx != 0 || device_uid) {
                int i = -1;
                for (AVCaptureDevice *device in [vidcap_avfoundation_state devices]) {
                        i++;
                        if (device_idx != -1 && i == device_idx) {
                                m_device = device;
                                break;
                        }
                        if (device_uid && [[device uniqueID] caseInsensitiveCompare: device_uid] == NSOrderedSame) {
                                m_device = device;
                                break;
                        }
                        if (device_name && [[device localizedName] caseInsensitiveCompare: device_name] == NSOrderedSame) {
                                m_device = device;
                                break;
                        }
                }
                if (!m_device) {
                        if (device_idx != -1) {
                                [NSException raise:@"Invalid argument" format:@"Device index %d is invalid", device_idx];
                        } else if (device_uid) {
                                [NSException raise:@"Invalid argument" format:@"Device uid %@ is invalid", device_uid];
                        } else {
                                [NSException raise:@"Invalid argument" format:@"Device name %@ is invalid", device_name];
                        }
                }
        } else {
                m_device = [AVCaptureDevice
                        defaultDeviceWithMediaType:AVMediaTypeVideo];
        }

        if (m_device == nil) {
                [NSException raise:@"No device" format:@"No capture device was found!"];
        }

        LOG(LOG_LEVEL_NOTICE) << MOD_NAME << "Using device: " << [[m_device localizedName] UTF8String] << "\n";

        // Create a device input with the device and add it to the session.
        AVCaptureDeviceInput *input = [AVCaptureDeviceInput deviceInputWithDevice:m_device
                error:&error];
        if (!input) {
                [NSException raise:@"No media" format:@"No media input!"];
        }
        [m_session addInput:input];

        // Create a VideoDataOutput and add it to the session
        AVCaptureVideoDataOutput *output = [[AVCaptureVideoDataOutput alloc] init];
        output.alwaysDiscardsLateVideoFrames = YES;
        [m_session addOutput:output];

        // Configure your output.
        dispatch_queue_t queue = dispatch_queue_create("myQueue", NULL);
        [output setSampleBufferDelegate:self queue:queue];
        dispatch_release(queue);

#if 0
        // TODO: do not do this, AV foundation usually selects better codec than we
        // Specify the pixel format
        output.videoSettings =
                [NSDictionary dictionaryWithObject:
                [NSNumber numberWithInt:kCVPixelFormatType_422YpCbCr10]
                forKey:(id)kCVPixelBufferPixelFormatTypeKey];
#endif

        // check if all options we get are recognized
        id objects[] = { @"device", @"uid", @"name",  @"mode", @"fps", @"fr_idx", @"preset"};
        NSUInteger count = sizeof(objects) / sizeof(id);
        NSArray *knownKeys = [NSArray arrayWithObjects:objects
                count:count];
        NSMutableDictionary *params_copy = [params mutableCopy];
        [params_copy removeObjectsForKeys: knownKeys];
        if ([params_copy count] > 0) {
                LOG(LOG_LEVEL_WARNING) << "[AVFoundation] Unknown options in config string!\n";
        }
        [params_copy release];

        if ([params valueForKey:@"mode"]) {
                use_preset = false;
                int mode = [[params valueForKey:@"mode"] intValue];
                int i = -1;
                // Find a suitable AVCaptureDevice
                AVCaptureDeviceFormat *format = nil;
                AVFrameRateRange *rate = nil;
                int rate_idx_req = -1;
                for (format in [m_device formats] ) {
                        i++;
                        if (i == mode)
                                break;
                }
                if (i != mode) {
                        NSLog(@"Mode index out of bounds!");
                        format = nil;
                }
                if ([params valueForKey:@"fps"]) {
                        m_fps_req = [[params valueForKey:@"fps"] doubleValue];
                }
                if ([params valueForKey:@"fr_idx"]) {
                        rate_idx_req = [[params valueForKey:@"fr_idx"] intValue];
                }
                if (format && (m_fps_req != 0 || rate_idx_req != -1)) {
			int rate_idx = 0;
			for (AVFrameRateRange *it in format.videoSupportedFrameRateRanges) {
				if (rate_idx_req != -1) {
					if (rate_idx == rate_idx_req) {
						rate = it;
						break;
					}
				} else { // m_fps_req != 0
                                        const double eps = 0.01; // needed because there are ranges for like 30,00003-30,00003 FPS
					if (m_fps_req >= (double) it.minFrameDuration.timescale / it.minFrameDuration.value - eps && m_fps_req <= (double) it.maxFrameDuration.timescale / it.maxFrameDuration.value + eps) {
						rate = it;
						break;
					}
				}
				rate_idx++;
			}
			if (rate == nil) {
				LOG(LOG_LEVEL_WARNING) << "[AVFoundation] Selected FPS not available! See '-t avfoundation:fullhelp'\n";
			}
                }
                if ([m_device lockForConfiguration:&error]) {
                        if (format) {
                                [m_device setActiveFormat: format];
				if (rate) {
                                        /**
                                         * @todo
                                         * Allow selection of exact frame rate instead of the whole range.
                                         */
					m_device.activeVideoMinFrameDuration = rate.minFrameDuration;
					m_device.activeVideoMaxFrameDuration = rate.maxFrameDuration;
				}
                        }
                        [m_device unlockForConfiguration];
                } else {
                        NSLog(@"Unable to set mode!");
                }
		// try to use native format if possible
		if (format) {
			CMVideoFormatDescriptionRef formatDesc = [[m_device activeFormat] formatDescription];
			FourCharCode fcc = CMFormatDescriptionGetMediaSubType(formatDesc);
			if (av_to_uv.find(fcc) != av_to_uv.end()) {
				output.videoSettings =
					[NSDictionary dictionaryWithObject:
					[NSNumber numberWithInt:fcc]
					forKey:(id)kCVPixelBufferPixelFormatTypeKey];
			}
		}
        } else {
                NSString *preset = AVCaptureSessionPresetMedium;
                if ([params valueForKey:@"preset"]) {
                        auto it = preset_to_av.find([[params valueForKey:@"preset"] UTF8String]);
                        if (it != preset_to_av.end()) {
                                preset = it->second;
                        } else {
                                NSLog(@"Unknown preset %@!", [params valueForKey:@"preset"]);
                        }
                }
                m_session.sessionPreset = preset;
        }

	// set device frame rate also to capture output to prevent rate oscilation
        AVCaptureConnection *conn = [output connectionWithMediaType: AVMediaTypeVideo];
        if (conn.isVideoMinFrameDurationSupported)
                conn.videoMinFrameDuration = m_device.activeVideoMinFrameDuration;
        if (NSAppKitVersionNumber >= NSAppKitVersionNumber10_9) {
                if (conn.isVideoMaxFrameDurationSupported)
                        conn.videoMaxFrameDuration = m_device.activeVideoMaxFrameDuration;
        }

        // You must also call lockForConfiguration: before calling the AVCaptureSession method startRunning, or the session's preset will override the selected active format on the capture device.
        //https://developer.apple.com/library/mac/documentation/AVFoundation/Reference/AVCaptureDevice_Class/Reference/Reference.html
        if (!use_preset) {
                [m_device lockForConfiguration:&error];
        }
        // Start the session running to start the flow of data
        [m_session startRunning];
        if (!use_preset) {
                [m_device unlockForConfiguration];
        }

        return self;
}

- (void)captureOutput:(AVCaptureOutput *)captureOutput didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
fromConnection:(AVCaptureConnection *)connection
{
#pragma unused (captureOutput)
#pragma unused (connection)
        unique_lock<mutex> lock(m_lock);
        struct video_frame *frame = [self imageFromSampleBuffer: sampleBuffer];
        if (frame) {
                if (m_queue.size() < MAX_CAPTURE_QUEUE_SIZE) {
                        m_queue.push(frame);
                        lock.unlock();
                        m_frame_ready.notify_one();
                } else {
                        NSLog(@"Frame dropped!");
                        VIDEO_FRAME_DISPOSE(frame);
                }
        }
}

- (void)dealloc
{
        [m_session stopRunning];
        [m_session release];

        [super dealloc];
}

// Create a UIImage from sample buffer data
- (struct video_frame *) imageFromSampleBuffer:(CMSampleBufferRef) sampleBuffer
{
        //NSLog(@"imageFromSampleBuffer: called");

	CMBlockBufferRef blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
	CMFormatDescriptionRef videoDesc = CMSampleBufferGetFormatDescription(sampleBuffer);
	CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(videoDesc);
	CMTime dur = CMSampleBufferGetOutputDuration(sampleBuffer);
	FourCharCode fcc = CMFormatDescriptionGetMediaSubType(videoDesc);

	auto codec_it = av_to_uv.find(fcc);
	if (codec_it == av_to_uv.end()) {
		NSLog(@"Unhandled codec: %.4s!\n", (const char *) &fcc);
		return NULL;
	}

	struct video_desc desc;
	desc.color_spec = get<0>(codec_it->second);
	desc.width = dim.width;
	desc.height = dim.height;
	desc.fps = m_fps_req != 0 ? m_fps_req : 1.0 / CMTimeGetSeconds(dur);
	desc.tile_count = 1;
	desc.interlacing = PROGRESSIVE;

	struct video_frame *ret = nullptr;

        // Get a CMSampleBuffer's Core Video image buffer for the media data
        CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
	if (imageBuffer) {
		[(id) imageBuffer retain];

		ret = vf_alloc_desc(desc);
		// Lock the base address of the pixel buffer
		CVPixelBufferLockBaseAddress(imageBuffer, 0);
		ret->tiles[0].data = (char *) CVPixelBufferGetBaseAddress(imageBuffer);
		ret->callbacks.dispose_udata = imageBuffer;
                static auto dispose = [](struct video_frame *frame) {
                        CVImageBufferRef imageBuffer = (CVImageBufferRef) frame->callbacks.dispose_udata;
                        // Unlock the pixel buffer
                        CVPixelBufferUnlockBaseAddress(imageBuffer, 0);
                        [(id) imageBuffer release];
                        vf_free(frame);
                };
                ret->callbacks.dispose = dispose;
	} else {
		ret = vf_alloc_desc_data(desc);
		CMBlockBufferCopyDataBytes(blockBuffer, 0, ret->tiles[0].data_len, ret->tiles[0].data);
		ret->callbacks.dispose = vf_free;
	}

	if (desc.color_spec == RGBA && (get<1>(codec_it->second) != 0 || get<2>(codec_it->second) != 8 ||
				get<2>(codec_it->second) != 16)) {
		int linesize = vc_get_linesize(desc.width, desc.color_spec);
		for (unsigned int y = 0; y < desc.height; ++y) {
			vc_copylineToRGBA_inplace((unsigned char *) ret->tiles[0].data + y * linesize,
					(unsigned char *) ret->tiles[0].data + y * linesize,
					linesize, get<1>(codec_it->second),
					get<2>(codec_it->second), get<3>(codec_it->second));
		}
	}

	return ret;
}

- (struct video_frame *) grab
{
        unique_lock<mutex> lock(m_lock);
        m_frame_ready.wait_for(lock, milliseconds(100), [&]{return m_queue.size() != 0;});
        if (m_queue.size() == 0) {
                return NULL;
        } else {
                struct video_frame *ret;
                ret = m_queue.front();
                m_queue.pop();

		m_frames++;

		chrono::steady_clock::time_point now = chrono::steady_clock::now();
		double seconds = chrono::duration_cast<chrono::microseconds>(now - m_t0).count() / 1000000.0;
		if (seconds >= 5) {
			cout << "[AVfoundation capture] " << m_frames << " frames in "
				<< seconds << " seconds = " <<  m_frames / seconds << " FPS\n";
			m_t0 = now;
			m_frames = 0;
		}

                return ret;
        }
}
@end

static void vidcap_avfoundation_probe(struct device_info **available_cards, int *count, void (**deleter)(void *))
{
        *deleter = free;

        struct device_info *cards = NULL;
        int card_count = 0;

        int i = 0;
        NSArray *devices = [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo];
        for (AVCaptureDevice *device in devices) {
                card_count += 1;
                cards = (struct device_info *) realloc(cards, card_count * sizeof(struct device_info));
                memset(&cards[card_count - 1], 0, sizeof(struct device_info));
                snprintf(cards[card_count - 1].dev, sizeof cards[card_count - 1].dev,
                                ":device=%d", i);
                snprintf(cards[card_count - 1].name, sizeof cards[card_count - 1].name,
                                "AV Foundation %s", [[device localizedName] UTF8String]);

                int j = 0;
                for ( AVCaptureDeviceFormat *format in [device formats] ) {
                        if (j >= (int) (sizeof cards[card_count - 1].modes /
                                                sizeof cards[card_count - 1].modes[0])) { // no space
                                break;
                        }

                        CMVideoFormatDescriptionRef formatDesc = [format formatDescription];
                        FourCharCode fcc = CMFormatDescriptionGetMediaSubType(formatDesc);
                        CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(formatDesc);
                        FourCharCode fcc_host = CFSwapInt32BigToHost(fcc);
                        int maxrate = 0;
                        for ( AVFrameRateRange *range in format.videoSupportedFrameRateRanges ) {
                                maxrate = range.maxFrameRate;
                                break;
                        }

                        snprintf(cards[card_count - 1].modes[j].id,
                                        sizeof cards[card_count - 1].modes[j].id,
                                        "{\"mode\":\"%d\", \"fps\":\"%d\"}", j, maxrate);
                        snprintf(cards[card_count - 1].modes[j].name,
                                        sizeof cards[card_count - 1].modes[j].name,
                                        "%.4s %dx%d (%d FPS)", (const char *) &fcc_host, dim.width, dim.height, maxrate);
                        j++;
                }

                i++;
        }

        *available_cards = cards;
        *count = card_count;
}

static int vidcap_avfoundation_init(struct vidcap_params *params, void **state)
{
        if (strcasecmp(vidcap_params_get_fmt(params), "help") == 0) {
                [vidcap_avfoundation_state usage: false];
                return VIDCAP_INIT_NOERR;
        } else if (strcasecmp(vidcap_params_get_fmt(params), "fullhelp") == 0) {
                [vidcap_avfoundation_state usage: true];
                return VIDCAP_INIT_NOERR;
        }
        NSMutableDictionary *init_params = [[NSMutableDictionary alloc] init];
        char *tmp = strdup(vidcap_params_get_fmt(params));
        char *item, *save_ptr, *cfg = tmp;
        while ((item = strtok_r(cfg, ":", &save_ptr))) {
                char *key_cstr = item;

                NSString *val;
                if (strchr(item, '=')) {
                        char *val_cstr = strchr(item, '=') + 1;
                        *strchr(item, '=') = '\0';
                        val = [NSString stringWithCString:val_cstr encoding:NSASCIIStringEncoding];
                } else {
                        val = @"";
                }
                NSString *key = [NSString stringWithCString:key_cstr encoding:NSASCIIStringEncoding];
                [init_params setObject:val forKey:key];
                [key release];
                [val release];

                cfg = NULL;
        }
        void *ret = nullptr;
        @try {
                ret = (void *) [[vidcap_avfoundation_state alloc] initWithParams: init_params];
        }
        @catch ( NSException *e ) {
                cerr << [[e reason] UTF8String] << "\n";
                ret = nullptr;
        }
        [init_params release];
        free(tmp);
        if (ret) {
                *state = ret;
                return VIDCAP_INIT_OK;
        } else {
                return VIDCAP_INIT_FAIL;
        }
}

static void vidcap_avfoundation_done(void *state)
{
        [(vidcap_avfoundation_state *) state release];
}

static struct video_frame *vidcap_avfoundation_grab(void *state, struct audio_frame **audio)
{
	*audio = nullptr;
        return [(vidcap_avfoundation_state *) state grab];
}

static const struct video_capture_info vidcap_avfoundation_info = {
        vidcap_avfoundation_probe,
        vidcap_avfoundation_init,
        vidcap_avfoundation_done,
        vidcap_avfoundation_grab,
        VIDCAP_NO_GENERIC_FPS_INDICATOR,
};

REGISTER_MODULE(avfoundation, &vidcap_avfoundation_info, LIBRARY_CLASS_VIDEO_CAPTURE, VIDEO_CAPTURE_ABI_VERSION);

