/* Linux backend: GStreamer.
 *
 * Pipeline:  filesrc ! decodebin ! videoconvert ! video/x-raw,format=RGBA ! appsink
 *
 * decodebin picks whatever decoder the system has installed (hardware VA-API
 * decoders when present, otherwise e.g. avdec_h264 from gst-libav). The
 * pipeline runs in GStreamer's own threads and decodes a few frames ahead;
 * nv_read_frame just pulls the next ready frame from appsink.
 */
 
#include "nativevideo.h"
#include "nv_internal.h"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <stdlib.h>

struct nv_video {
    GstElement* pipeline;
    GstAppSink* sink;
    nv_info     info;
};

const char* nv_backend(void) { return "gstreamer"; }

/* Pull an error message off the bus, if any, into nv_last_error. */
static int take_bus_error(GstElement* pipeline)
{
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    int found = 0;
    if (msg) {
        GError* err = NULL;
        gst_message_parse_error(msg, &err, NULL);
        nv_set_error("gstreamer: %s", err ? err->message : "unknown error");
        if (err) g_error_free(err);
        gst_message_unref(msg);
        found = 1;
    }
    gst_object_unref(bus);
    return found;
}

nv_video* nv_open(const char* path)
{
    GError* err = NULL;
    if (!gst_init_check(NULL, NULL, &err)) {
        nv_set_error("gst_init failed: %s", err ? err->message : "?");
        if (err) g_error_free(err);
        return NULL;
    }

    GstElement* pipeline = gst_parse_launch(
        "filesrc name=src ! decodebin ! videoconvert ! "
        "video/x-raw,format=RGBA ! "
        "appsink name=sink sync=false max-buffers=2 drop=false",
        &err);

    if (!pipeline) {
        nv_set_error("pipeline: %s", err ? err->message : "?");
        if (err) g_error_free(err);
        return NULL;
    }
    if (err) g_error_free(err); /* non-fatal warning */

    GstElement* src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    g_object_set(src, "location", path, NULL);
    gst_object_unref(src);

    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");

    /* PAUSED = preroll: opens the file, finds a decoder, decodes frame 0. */
    gst_element_set_state(pipeline, GST_STATE_PAUSED);
    GstStateChangeReturn r = gst_element_get_state(pipeline, NULL, NULL, 10 * GST_SECOND);
    
    if (r == GST_STATE_CHANGE_FAILURE) {
        if (!take_bus_error(pipeline)) nv_set_error("could not open %s", path);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(sink);
        gst_object_unref(pipeline);
        return NULL;
    }

    GstPad* pad = gst_element_get_static_pad(sink, "sink");
    GstCaps* caps = gst_pad_get_current_caps(pad);
    gst_object_unref(pad);
    GstVideoInfo vi;

    if (!caps || !gst_video_info_from_caps(&vi, caps)) {
        if (caps) gst_caps_unref(caps);
        nv_set_error("no video stream in %s", path);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(sink);
        gst_object_unref(pipeline);
        return NULL;
    }
    gst_caps_unref(caps);

    nv_video* v = (nv_video*)calloc(1, sizeof *v);
    v->pipeline = pipeline;
    v->sink = GST_APP_SINK(sink);
    v->info.width = GST_VIDEO_INFO_WIDTH(&vi);
    v->info.height = GST_VIDEO_INFO_HEIGHT(&vi);
    v->info.fps = GST_VIDEO_INFO_FPS_D(&vi)
        ? (double)GST_VIDEO_INFO_FPS_N(&vi) / GST_VIDEO_INFO_FPS_D(&vi) : 0.0;

    gint64 dur = 0;
    if (gst_element_query_duration(pipeline, GST_FORMAT_TIME, &dur) && dur > 0)
        v->info.duration = (double)dur / GST_SECOND;

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    return v;
}

void nv_get_info(nv_video* v, nv_info* out) { if (v && out) *out = v->info; }

int nv_read_frame(nv_video* v, uint8_t* rgba_out, double* pts_out)
{
    if (!v) return -1;
    GstSample* sample = gst_app_sink_pull_sample(v->sink);
    if (!sample) {
        if (gst_app_sink_is_eos(v->sink)) return 0;
        if (!take_bus_error(v->pipeline)) nv_set_error("pull_sample failed");
        return -1;
    }

    GstBuffer* buf = gst_sample_get_buffer(sample);
    GstVideoInfo vi;
    GstVideoFrame frame;
    if (!gst_video_info_from_caps(&vi, gst_sample_get_caps(sample)) ||
        !gst_video_frame_map(&frame, &vi, buf, GST_MAP_READ)) {
        gst_sample_unref(sample);
        nv_set_error("could not map frame");
        return -1;
    }

    int w = GST_VIDEO_FRAME_WIDTH(&frame);
    int h = GST_VIDEO_FRAME_HEIGHT(&frame);
    if (w > v->info.width)  w = v->info.width;
    if (h > v->info.height) h = v->info.height;
    nv_copy_rows(rgba_out,
                 (const uint8_t*)GST_VIDEO_FRAME_PLANE_DATA(&frame, 0),
                 GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0), w, h, 0);

    if (pts_out) {
        GstClockTime pts = GST_BUFFER_PTS(buf);
        const GstSegment* seg = gst_sample_get_segment(sample);
      
        if (GST_CLOCK_TIME_IS_VALID(pts) && seg) {
            guint64 st = gst_segment_to_stream_time(seg, GST_FORMAT_TIME, pts);
            if (GST_CLOCK_TIME_IS_VALID(st)) pts = st;
        }
        *pts_out = GST_CLOCK_TIME_IS_VALID(pts) ? (double)pts / GST_SECOND : 0.0;
    }
    gst_video_frame_unmap(&frame);
    gst_sample_unref(sample);
    return 1;
}

int nv_seek(nv_video* v, double seconds)
{
    if (!v) return 0;
    if (seconds < 0) seconds = 0;
    /* ACCURATE: GStreamer decodes from the previous keyframe and drops
     * frames before the target itself.z  */
    gboolean ok = gst_element_seek_simple(v->pipeline, GST_FORMAT_TIME,
        (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
        (gint64)(seconds * GST_SECOND));
    if (!ok) { nv_set_error("seek failed"); return 0; }
    return 1;
}

void nv_close(nv_video* v)
{
    if (!v) return;
    gst_element_set_state(v->pipeline, GST_STATE_NULL);
    gst_object_unref(v->sink);
    gst_object_unref(v->pipeline);
    free(v);
}
