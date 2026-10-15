// Windows backend: Media Foundation Source Reader.
//
// The Source Reader picks whatever decoder Windows has for the file
// RGB32 is really BGRX, so we swap Blue/Red and force alpha while copying.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdlib.h>

#include "nativevideo.h"
#include "nv_internal.h"

static const DWORD kStream = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;

enum Mode { MODE_RGB32_ADVANCED, MODE_RGB32_BASIC, MODE_NV12, MODE_COUNT };

struct nv_video {
    IMFSourceReader* reader;
    nv_info info;
    bool   nv12;
    // Decoded buffers can be larger than the picture (e.g. 1920x1088) and the
    // visible area may be offset. These describe where the picture is.
    UINT32 buf_w, buf_h;
    LONG   default_stride;
    int    off_x, off_y;
    int    bt709, full_range;   // NV12 colour conversion settings
    bool   com_owned;
};

extern "C" const char* nv_backend(void) { return "mediafoundation"; }

template <class T> static void safe_release(T*& p) { if (p) { p->Release(); p = NULL; } }

// Read frame size, visible area, stride and colour info from the output type.
static bool read_format(nv_video* v)
{
    IMFMediaType* cur = NULL;
    if (FAILED(v->reader->GetCurrentMediaType(kStream, &cur))) return false;

    UINT32 w = 0, h = 0;
    MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
    v->buf_w = w; v->buf_h = h;
    v->off_x = 0; v->off_y = 0;
    int vis_w = (int)w, vis_h = (int)h;

    MFVideoArea area;
    if (SUCCEEDED(cur->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&area, sizeof area, NULL))
        && area.Area.cx > 0 && area.Area.cy > 0) {
        v->off_x = area.OffsetX.value;
        v->off_y = area.OffsetY.value;
        vis_w = area.Area.cx;
        vis_h = area.Area.cy;
    }

    UINT32 stride = 0;
    if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) {
        v->default_stride = (LONG)stride;   // may be negative (bottom-up RGB)
    } else {
        LONG s = 0;
        MFGetStrideForBitmapInfoHeader(v->nv12 ? MFVideoFormat_NV12.Data1
                                               : MFVideoFormat_RGB32.Data1, w, &s);
        v->default_stride = s;
    }

    UINT32 num = 0, den = 0;
    if (SUCCEEDED(MFGetAttributeRatio(cur, MF_MT_FRAME_RATE, &num, &den)) && den)
        v->info.fps = (double)num / den;

    UINT32 matrix = 0, range = 0;
    v->bt709 = SUCCEEDED(cur->GetUINT32(MF_MT_YUV_MATRIX, &matrix))
        ? matrix == MFVideoTransferMatrix_BT709 : (vis_h >= 720);
    v->full_range = SUCCEEDED(cur->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range))
        && range == MFNominalRange_0_255;

    // Only set the public size once; later format changes are clamped to it

    if (v->info.width == 0) { v->info.width = vis_w; v->info.height = vis_h; }

    cur->Release();
    return w > 0 && h > 0;
}

static IMFSourceReader* create_reader(const wchar_t* wpath, Mode mode, HRESULT* out_hr)
{
    IMFAttributes* attrs = NULL;
    IMFSourceReader* reader = NULL;
    IMFMediaType* type = NULL;
    HRESULT hr = MFCreateAttributes(&attrs, 3);
    if (SUCCEEDED(hr)) {
        if (mode == MODE_RGB32_ADVANCED)
            attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        else if (mode == MODE_RGB32_BASIC)
            attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        hr = MFCreateSourceReaderFromURL(wpath, attrs, &reader);
    }
    if (SUCCEEDED(hr)) {
        reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        hr = reader->SetStreamSelection(kStream, TRUE);
    }
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) {
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, mode == MODE_NV12 ? MFVideoFormat_NV12 : MFVideoFormat_RGB32);
        hr = reader->SetCurrentMediaType(kStream, NULL, type);
    }
    safe_release(type);
    safe_release(attrs);
    if (FAILED(hr)) safe_release(reader);
    *out_hr = hr;
    return reader;
}

extern "C" nv_video* nv_open(const char* path)
{
    nv_video* v = (nv_video*)calloc(1, sizeof *v);

    // RPC_E_CHANGED_MODE is fine, we just must not uninitialize it later.
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    v->com_owned = SUCCEEDED(hr);

    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        nv_set_error("MFStartup failed (0x%08lx)", (unsigned long)hr);
        if (v->com_owned) CoUninitialize();
        free(v);
        return NULL;
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t* wpath = (wchar_t*)malloc(sizeof(wchar_t) * (wlen > 0 ? wlen : 1));
    wpath[0] = 0;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);

    for (int m = 0; m < MODE_COUNT && !v->reader; m++) {
        v->reader = create_reader(wpath, (Mode)m, &hr);
        v->nv12 = (m == MODE_NV12);

        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
            hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) break;
    }
    free(wpath);

    if (!v->reader || !read_format(v)) {
        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
            hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
            nv_set_error("file not found: %s", path);
        else
            nv_set_error("could not open video (0x%08lx). Unsupported codec? "
                         "HEVC/AV1 need the Microsoft Store extensions.", (unsigned long)hr); //MS wants 80p 
        safe_release(v->reader);
        MFShutdown();
        if (v->com_owned) CoUninitialize();
        free(v);
        return NULL;
    }

    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(v->reader->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE,
                                                      MF_PD_DURATION, &var))) {
        v->info.duration = (double)var.uhVal.QuadPart / 1e7;   // 100ns units
        PropVariantClear(&var);
    }
    return v;
}

extern "C" void nv_get_info(nv_video* v, nv_info* out) { if (v && out) *out = v->info; }

// scan0 = first row of the buffer, pitch = bytes per row (negative = bottom-up)
static void convert(nv_video* v, BYTE* scan0, LONG pitch, uint8_t* out, int w, int h)
{
    if (v->nv12) {
        // UV plane follows the full (padded) Y plane, half height.
        BYTE* y  = scan0 + (LONG)v->off_y * pitch + v->off_x;
        BYTE* uv = scan0 + (LONG)v->buf_h * pitch + (LONG)(v->off_y / 2) * pitch + (v->off_x & ~1);
        nv_nv12_to_rgba(out, y, pitch, uv, pitch, w, h, v->bt709, v->full_range);
    } else {
        nv_copy_rows(out, scan0 + (LONG)v->off_y * pitch + v->off_x * 4, pitch, w, h, 1);
    }
}

static int copy_buffer(nv_video* v, IMFMediaBuffer* buf, uint8_t* out)
{
    int w = v->info.width, h = v->info.height;
    if (v->off_x + w > (int)v->buf_w) w = (int)v->buf_w - v->off_x;
    if (v->off_y + h > (int)v->buf_h) h = (int)v->buf_h - v->off_y;
    if (w <= 0 || h <= 0) return 0;

    IMF2DBuffer* b2 = NULL;
    if (SUCCEEDED(buf->QueryInterface(IID_IMF2DBuffer, (void**)&b2))) {
        BYTE* scan0 = NULL; LONG pitch = 0;
        if (SUCCEEDED(b2->Lock2D(&scan0, &pitch))) {
            // Lock2D gives row 0 even for bottom-up images (pitch is negative then).
            convert(v, scan0, pitch, out, w, h);
            b2->Unlock2D();
            b2->Release();
            return 1;
        }
        b2->Release();
    }

    BYTE* p = NULL; DWORD max_len = 0, len = 0;
    if (FAILED(buf->Lock(&p, &max_len, &len))) return 0;
    LONG pitch = v->default_stride;
    BYTE* scan0 = pitch < 0 ? p + (LONG)(v->buf_h - 1) * (-pitch) : p;
    convert(v, scan0, pitch, out, w, h);
    buf->Unlock();
    return 1;
}

extern "C" int nv_read_frame(nv_video* v, uint8_t* rgba_out, double* pts_out)
{
    if (!v || !v->reader) return -1;
    for (;;) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample* sample = NULL;
        HRESULT hr = v->reader->ReadSample(kStream, 0, NULL, &flags, &ts, &sample);

        if (FAILED(hr)) {
            nv_set_error("ReadSample failed (0x%08lx)", (unsigned long)hr);
            return -1;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { safe_release(sample); return 0; }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) read_format(v);
        if (!sample) continue;   // stream gap / tick: no picture, keep reading

        IMFMediaBuffer* buf = NULL;
        int ok = 0;

        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
            ok = copy_buffer(v, buf, rgba_out);
            buf->Release();
        }

        sample->Release();

        if (!ok) { nv_set_error("could not read frame buffer"); return -1; }
        if (pts_out) *pts_out = (double)ts / 1e7;
        return 1;
    }
}

extern "C" int nv_seek(nv_video* v, double seconds)
{
    if (!v || !v->reader) return 0;
    if (seconds < 0) seconds = 0;
    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = (LONGLONG)(seconds * 1e7);

    // Lands on the keyframe at or before the target; Lua skips the rest.
    HRESULT hr = v->reader->SetCurrentPosition(GUID_NULL, var);
    if (FAILED(hr)) { nv_set_error("seek failed (0x%08lx)", (unsigned long)hr); return 0; }
    return 1;
}

extern "C" void nv_close(nv_video* v)
{
    if (!v) return;
    safe_release(v->reader);
    MFShutdown();
    if (v->com_owned) CoUninitialize();
    free(v);
}
