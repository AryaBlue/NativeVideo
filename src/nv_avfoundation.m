// macOS backend: AVFoundation (AVAssetReader).
//
// AVAssetReader uses VideoToolbox, so H.264/HEVC/ProRes decode on the GPU's
// media engine where available. We ask for BGRA frames and swizzle to RGBA.
// Built with ARC; Objective-C objects are kept in the C struct as retained
// CF pointers.
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#include <stdlib.h>

#include "nativevideo.h"
#include "nv_internal.h"

struct nv_video {
    void* asset;    // AVURLAsset*
    void* track;    // AVAssetTrack*
    void* reader;   // AVAssetReader*
    void* output;   // AVAssetReaderTrackOutput*
    nv_info info;
};

const char* nv_backend(void) { return "avfoundation"; }

#define OBJ(p) ((__bridge id)(p))

static void drop_reader(nv_video* v)
{
    if (v->reader) {
        [(AVAssetReader*)OBJ(v->reader) cancelReading];
        CFBridgingRelease(v->reader);
        v->reader = NULL;
    }
    if (v->output) { CFBridgingRelease(v->output); v->output = NULL; }
}

// AVAssetReader can't rewind, so every seek builds a fresh reader that starts
// at the requested time. It decodes from the previous keyframe internally and
// only hands back frames from `start` onward.
static int start_reader(nv_video* v, double start)
{
    drop_reader(v);
    NSError* err = nil;
    AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:(AVAsset*)OBJ(v->asset) error:&err];
    if (!reader) {
        nv_set_error("AVAssetReader: %s", err ? err.localizedDescription.UTF8String : "?");
        return 0;
    }
    NSDictionary* settings = @{ (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA) };
    AVAssetReaderTrackOutput* out =
        [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:(AVAssetTrack*)OBJ(v->track)
                                                   outputSettings:settings];
    out.alwaysCopiesSampleData = NO;
    if (start > 0)
        reader.timeRange = CMTimeRangeMake(CMTimeMakeWithSeconds(start, 600), kCMTimePositiveInfinity);
    [reader addOutput:out];
    if (![reader startReading]) {
        nv_set_error("startReading: %s",
                     reader.error ? reader.error.localizedDescription.UTF8String : "?");
        return 0;
    }
    v->reader = (void*)CFBridgingRetain(reader);
    v->output = (void*)CFBridgingRetain(out);
    return 1;
}

nv_video* nv_open(const char* path)
{
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url
            options:@{ AVURLAssetPreferPreciseDurationAndTimingKey : @YES }];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        NSArray* tracks = [asset tracksWithMediaType:AVMediaTypeVideo];
#pragma clang diagnostic pop
        if (tracks.count == 0) {
            nv_set_error("no video track (or unreadable file): %s", path);
            return NULL;
        }
        AVAssetTrack* track = tracks[0];

        nv_video* v = (nv_video*)calloc(1, sizeof *v);
        v->asset = (void*)CFBridgingRetain(asset);
        v->track = (void*)CFBridgingRetain(track);
        v->info.width = (int32_t)track.naturalSize.width;
        v->info.height = (int32_t)track.naturalSize.height;
        v->info.fps = track.nominalFrameRate;
        v->info.duration = CMTimeGetSeconds(asset.duration);

        if (!start_reader(v, 0)) { nv_close(v); return NULL; }
        return v;
    }
}

void nv_get_info(nv_video* v, nv_info* out) { if (v && out) *out = v->info; }

int nv_read_frame(nv_video* v, uint8_t* rgba_out, double* pts_out)
{
    if (!v || !v->output) return -1;
    @autoreleasepool {
        AVAssetReaderTrackOutput* out = (AVAssetReaderTrackOutput*)OBJ(v->output);
        AVAssetReader* reader = (AVAssetReader*)OBJ(v->reader);
        for (;;) {
            CMSampleBufferRef sb = [out copyNextSampleBuffer];
            if (!sb) {
                if (reader.status == AVAssetReaderStatusCompleted) return 0;
                nv_set_error("decode: %s",
                             reader.error ? reader.error.localizedDescription.UTF8String : "?");
                return -1;
            }
            CVImageBufferRef img = CMSampleBufferGetImageBuffer(sb);
            if (!img) { CFRelease(sb); continue; }   // non-picture sample

            CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
            int w = (int)CVPixelBufferGetWidth(img);
            int h = (int)CVPixelBufferGetHeight(img);
            if (w > v->info.width)  w = v->info.width;
            if (h > v->info.height) h = v->info.height;
            nv_copy_rows(rgba_out, (const uint8_t*)CVPixelBufferGetBaseAddress(img),
                         (long)CVPixelBufferGetBytesPerRow(img), w, h, 1);
            CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);

            if (pts_out) *pts_out = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sb));
            CFRelease(sb);
            return 1;
        }
    }
}

int nv_seek(nv_video* v, double seconds)
{
    if (!v) return 0;
    @autoreleasepool { return start_reader(v, seconds < 0 ? 0 : seconds); }
}

void nv_close(nv_video* v)
{
    if (!v) return;
    @autoreleasepool {
        drop_reader(v);
        if (v->track) CFBridgingRelease(v->track);
        if (v->asset) CFBridgingRelease(v->asset);
    }
    free(v);
}
