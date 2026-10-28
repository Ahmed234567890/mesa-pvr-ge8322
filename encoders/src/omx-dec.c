/*
 * omx-dec - decode video with the Unisoc/SPRD OpenMAX decoders.
 *
 * This is the decode counterpart to omx-enc.  It drives the vendor OpenMAX IL
 * components through libhybris from the Kali chroot, so the dedicated video
 * sub-processor (/dev/sprd_vsp) does the work instead of the CPU.
 *
 * The same Android 14 compatibility workarounds as omx-enc are required and
 * reused here:
 *
 *   1. libstagefrighthw.so allocates its AHandlerReflector with new(56), but
 *      Android 14's AHandler::deliverMessage() writes up to +68.  The probe
 *      widens that allocation to 120 bytes.
 *   2. The vendor looper path corrupts the handler's mutex, so AMessage::post
 *      is redirected to a shim that calls onMessageReceived() synchronously.
 *
 * The vendor marks both hardware decoders with the
 * "requires-allocate-on-output-ports" quirk, so output buffers are allocated by
 * the component (OMX_AllocateBuffer), never supplied by the caller.
 *
 * Input is an Annex B elementary stream (H.264 or HEVC).  Output is raw
 * NV12 (default) or I420, cropped to the visible picture, one frame after
 * another.
 *
 * Usage:
 *   omx-dec -c h264|h265 [-i IN|-] [-o OUT|-] [-n FRAMES] [-p nv12|i420]
 *           [-q] [-v] [--info]
 *
 * Examples:
 *   omx-dec -c h264 -i in.h264 -o out.nv12
 *   omx-dec -c h264 -i in.h264 -o - | ffmpeg -f rawvideo \
 *       -pix_fmt nv12 -s 320x240 -i - out.mp4
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>

#include <OMX_Core.h>
#include <OMX_Component.h>
#include <OMX_Index.h>
#include <OMX_Video.h>
#include <OMX_IVCommon.h>

#ifndef OMX_VERSION
#define OMX_VERSION_MAJOR 1
#define OMX_VERSION_MINOR 1
#define OMX_VERSION_REVISION 2
#define OMX_VERSION_STEP 0
#define OMX_VERSION ((OMX_VERSION_STEP << 24) | (OMX_VERSION_REVISION << 16) | \
                     (OMX_VERSION_MINOR << 8) | (OMX_VERSION_MAJOR))
#endif

#define INIT_STRUCT(x) do { \
    memset(&(x), 0, sizeof(x)); \
    (x).nSize = sizeof(x); \
    (x).nVersion.nVersion = OMX_VERSION; \
} while (0)

typedef OMX_ERRORTYPE (*pfn_OMX_Init)(void);
typedef OMX_ERRORTYPE (*pfn_OMX_Deinit)(void);
typedef OMX_ERRORTYPE (*pfn_OMX_GetHandle)(OMX_HANDLETYPE *, OMX_STRING,
                                           OMX_PTR, OMX_CALLBACKTYPE *);
typedef OMX_ERRORTYPE (*pfn_OMX_FreeHandle)(OMX_HANDLETYPE);
typedef OMX_ERRORTYPE (*pfn_OMX_GetParameter)(OMX_HANDLETYPE, OMX_INDEXTYPE,
                                              OMX_PTR);
typedef OMX_ERRORTYPE (*pfn_OMX_SetParameter)(OMX_HANDLETYPE, OMX_INDEXTYPE,
                                              OMX_PTR);
typedef OMX_ERRORTYPE (*pfn_OMX_GetConfig)(OMX_HANDLETYPE, OMX_INDEXTYPE,
                                           OMX_PTR);
typedef OMX_ERRORTYPE (*pfn_OMX_SendCommand)(OMX_HANDLETYPE, OMX_COMMANDTYPE,
                                             OMX_U32, OMX_PTR);
typedef OMX_ERRORTYPE (*pfn_OMX_AllocateBuffer)(OMX_HANDLETYPE,
                                                OMX_BUFFERHEADERTYPE **,
                                                OMX_U32, OMX_PTR, OMX_U32);
typedef OMX_ERRORTYPE (*pfn_OMX_UseBuffer)(OMX_HANDLETYPE,
                                           OMX_BUFFERHEADERTYPE **, OMX_U32,
                                           OMX_PTR, OMX_U32, OMX_U8 *);
typedef OMX_ERRORTYPE (*pfn_OMX_FreeBuffer)(OMX_HANDLETYPE, OMX_U32,
                                            OMX_BUFFERHEADERTYPE *);
typedef OMX_ERRORTYPE (*pfn_OMX_EmptyThisBuffer)(OMX_HANDLETYPE,
                                                 OMX_BUFFERHEADERTYPE *);
typedef OMX_ERRORTYPE (*pfn_OMX_FillThisBuffer)(OMX_HANDLETYPE,
                                                OMX_BUFFERHEADERTYPE *);
typedef OMX_ERRORTYPE (*pfn_OMX_GetState)(OMX_HANDLETYPE, OMX_STATETYPE *);

static pfn_OMX_GetParameter    p_GetParameter;
static pfn_OMX_SetParameter    p_SetParameter;
static pfn_OMX_GetConfig       p_GetConfig;
static pfn_OMX_SendCommand     p_SendCommand;
static pfn_OMX_AllocateBuffer  p_AllocateBuffer;
static pfn_OMX_UseBuffer       p_UseBuffer;
static pfn_OMX_FreeBuffer      p_FreeBuffer;
static pfn_OMX_EmptyThisBuffer p_EmptyThisBuffer;
static pfn_OMX_FillThisBuffer  p_FillThisBuffer;
static pfn_OMX_GetState        p_GetState;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static volatile int g_idle_done;
static volatile int g_exec_done;
static volatile int g_error;
static volatile OMX_ERRORTYPE g_error_code;
static volatile int g_fill_count;
static volatile int g_eos;
static volatile int g_input_done;
static volatile int g_port_changed;

#define MAX_BUF 16

static OMX_HANDLETYPE g_comp;
static int g_quiet;
static int g_verbose;
static long g_frames;

/* Output buffers are component-allocated; keep them reachable for
 * reconfiguration after OMX_EventPortSettingsChanged. */
static OMX_BUFFERHEADERTYPE *g_out_buf[MAX_BUF];
static int g_n_out;
static OMX_U32 g_alloc_size;
static volatile int g_flush_done;
static volatile int g_port_disabled;
static volatile int g_port_enabled;
static volatile int g_loaded_done;
static volatile int g_reconfigured;

/* Input buffers are reallocated by the Loaded reset, so they are global. */
static OMX_BUFFERHEADERTYPE *g_in_buf[MAX_BUF];
static int g_n_in;
static OMX_U32 g_in_size;

#define OUTQ_LEN 64
static OMX_BUFFERHEADERTYPE *g_outq[OUTQ_LEN];
static int g_outq_head, g_outq_tail, g_outq_count;

typedef struct {
    const char *name;
    const char *component;
    const char *lib;
    OMX_VIDEO_CODINGTYPE coding;
    int hardware;
} codec_def;

/*
 * The vendor core registers these decoders.  AV1 is deliberately absent: the
 * OMX headers available for this build have no OMX_VIDEO_CodingAV1 enumerant,
 * and OMX.sprd.av1.decoder is the vendor's software decoder anyway.
 */
static const codec_def g_codecs[] = {
    { "h264",  "OMX.sprd.h264.decoder",  "libomx_avcdec_hw_sprd.so",
      OMX_VIDEO_CodingAVC,   1 },
    { "h265",  "OMX.sprd.hevc.decoder",  "libomx_hevcdec_hw_sprd.so",
      OMX_VIDEO_CodingHEVC,  1 },
    { "h263",  "OMX.sprd.h263.decoder",  "libomx_m4vh263dec_hw_sprd.so",
      OMX_VIDEO_CodingH263,  1 },
    { "mpeg4", "OMX.sprd.mpeg4.decoder", "libomx_m4vh263dec_hw_sprd.so",
      OMX_VIDEO_CodingMPEG4, 1 },
    { "vp9",   "OMX.sprd.vp9.decoder",   "libomx_vp9dec_hw_sprd.so",
      OMX_VIDEO_CodingVP9,   1 },
};

static const codec_def *find_codec(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof(g_codecs) / sizeof(g_codecs[0]); i++) {
        if (!strcmp(g_codecs[i].name, name))
            return &g_codecs[i];
    }
    return NULL;
}

static const char *err_name(OMX_ERRORTYPE e)
{
    switch (e) {
    case OMX_ErrorNone: return "None";
    case OMX_ErrorInsufficientResources: return "InsufficientResources";
    case OMX_ErrorUndefined: return "Undefined";
    case OMX_ErrorInvalidComponentName: return "InvalidComponentName";
    case OMX_ErrorComponentNotFound: return "ComponentNotFound";
    case OMX_ErrorInvalidComponent: return "InvalidComponent";
    case OMX_ErrorBadParameter: return "BadParameter";
    case OMX_ErrorNotImplemented: return "NotImplemented";
    case OMX_ErrorUnderflow: return "Underflow";
    case OMX_ErrorOverflow: return "Overflow";
    case OMX_ErrorHardware: return "Hardware";
    case OMX_ErrorInvalidState: return "InvalidState";
    case OMX_ErrorStreamCorrupt: return "StreamCorrupt";
    case OMX_ErrorUnsupportedSetting: return "UnsupportedSetting";
    case OMX_ErrorUnsupportedIndex: return "UnsupportedIndex";
    default: return "other";
    }
}

static void check(const char *what, OMX_ERRORTYPE rc)
{
    if (rc != OMX_ErrorNone || !g_quiet)
        fprintf(stderr, "%-48s rc=%d (%s)\n", what, (int)rc, err_name(rc));
}

static void on_alarm(int sig)
{
    (void)sig;
    static const char msg[] = "omx-dec: TIMEOUT: decoder did not respond\n";
    write(2, msg, sizeof(msg) - 1);
    _exit(3);
}

static OMX_ERRORTYPE cb_event(OMX_HANDLETYPE h, OMX_PTR app, OMX_EVENTTYPE e,
                              OMX_U32 d1, OMX_U32 d2, OMX_PTR p)
{
    (void)h; (void)app; (void)p;
    if (g_verbose)
        fprintf(stderr, "omx-dec: event %d d1=%u d2=%u\n",
                (int)e, (unsigned)d1, (unsigned)d2);
    if (e == OMX_EventCmdComplete) {
        if (d1 == OMX_CommandStateSet) {
            if (d2 == OMX_StateIdle) g_idle_done = 1;
            else if (d2 == OMX_StateExecuting) g_exec_done = 1;
        } else if (d1 == OMX_CommandFlush) {
            g_flush_done = 1;
        } else if (d1 == OMX_CommandPortDisable) {
            g_port_disabled = 1;
        } else if (d1 == OMX_CommandPortEnable) {
            g_port_enabled = 1;
        }
        if (d1 == OMX_CommandStateSet && d2 == OMX_StateLoaded)
            g_loaded_done = 1;
    } else if (e == OMX_EventPortSettingsChanged) {
        /*
         * The decoder reconfigures its output port once it has parsed the
         * sequence headers.  Re-read the port definition and the crop
         * rectangle before writing the next frame.
         */
        if (d1 == 1) {
            g_port_changed = 1;
            if (g_verbose)
                fprintf(stderr, "omx-dec: output port settings changed\n");
        }
    } else if (e == OMX_EventBufferFlag) {
        if ((d2 & OMX_BUFFERFLAG_EOS) != 0)
            g_eos = 1;
    } else if (e == OMX_EventError) {
        g_error = 1;
        g_error_code = (OMX_ERRORTYPE)d1;
        fprintf(stderr, "omx-dec: decoder error event 0x%x\n", (unsigned)d1);
    }
    pthread_mutex_lock(&g_lock);
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE cb_empty_done(OMX_HANDLETYPE h, OMX_PTR app,
                                   OMX_BUFFERHEADERTYPE *buf)
{
    (void)h; (void)app; (void)buf;
    pthread_mutex_lock(&g_lock);
    g_input_done++;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE cb_fill_done(OMX_HANDLETYPE h, OMX_PTR app,
                                  OMX_BUFFERHEADERTYPE *buf)
{
    (void)h; (void)app;
    pthread_mutex_lock(&g_lock);
    g_fill_count++;
    if (buf->nFlags & OMX_BUFFERFLAG_EOS)
        g_eos = 1;
    if (g_outq_count < OUTQ_LEN) {
        g_outq[g_outq_head] = buf;
        g_outq_head = (g_outq_head + 1) % OUTQ_LEN;
        g_outq_count++;
    }
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    return OMX_ErrorNone;
}

static OMX_CALLBACKTYPE g_cbs = {
    cb_event, cb_empty_done, cb_fill_done
};

static unsigned long find_lib_base(const char *name)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    unsigned long base = 0;
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, name) && strchr(line, '/')) {
            base = strtoul(line, NULL, 16);
            break;
        }
    }
    fclose(f);
    return base;
}

/*
 * Widen SprdSimpleOMXComponent's new(56) handler allocation so Android 14's
 * AHandler::deliverMessage() stores at +56..+68 stay inside the object.
 */
static int patch_handler_alloc(void)
{
    unsigned long base = find_lib_base("libstagefrighthw.so");
    unsigned long page, addr;
    unsigned short insn;

    if (!base)
        return -1;
    addr = base + 0xab82;
    page = addr & ~0xffful;
    if (mprotect((void *)page, 0x2000,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return -1;
    insn = *(volatile unsigned short *)addr;
    if (insn != 0x2038)          /* movs r0, #56 */
        return -1;
    *(volatile unsigned short *)addr = 0x2078;   /* movs r0, #120 */
    __builtin___clear_cache((char *)addr, (char *)addr + 2);
    return 0;
}

static int patch_post_got(void *post_sync)
{
    unsigned long base = find_lib_base("libstagefrighthw.so");
    unsigned long addr, page;
    volatile unsigned int *slot;

    if (!base || !post_sync)
        return -1;
    addr = base + 0x10dbc;       /* .got.plt slot for AMessage::post */
    page = addr & ~0xffful;
    if (mprotect((void *)page, 0x1000, PROT_READ | PROT_WRITE) != 0)
        return -1;
    slot = (volatile unsigned int *)addr;
    *slot = (unsigned int)(unsigned long)post_sync;
    __builtin___clear_cache((char *)addr, (char *)addr + 4);
    return 0;
}

static int wait_flag(volatile int *flag, int seconds)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += seconds;
    pthread_mutex_lock(&g_lock);
    while (!*flag && !g_error) {
        if (pthread_cond_timedwait(&g_cond, &g_lock, &ts) != 0)
            break;
    }
    pthread_mutex_unlock(&g_lock);
    return *flag;
}

enum { PIX_NV12 = 0, PIX_I420 = 1 };

static int g_pixfmt = PIX_NV12;

/* Geometry of the current output port. */
static int g_out_stride;
static int g_out_slice;
static int g_vis_w;
static int g_vis_h;
static int g_vis_left;
static int g_vis_top;
static OMX_U32 g_out_size;

static void write_plane(FILE *fp, const uint8_t *base, int stride,
                        int left, int top, int width, int height)
{
    int y;
    for (y = 0; y < height; y++) {
        const uint8_t *row = base + (size_t)(top + y) * stride + left;
        if (fwrite(row, 1, (size_t)width, fp) != (size_t)width) {
            fprintf(stderr, "omx-dec: write failed: %s\n", strerror(errno));
            g_error = 1;
            return;
        }
    }
}

static void write_frame(FILE *fp, const uint8_t *buf, OMX_U32 filled)
{
    const uint8_t *y_plane = buf;
    const uint8_t *uv_plane = buf + (size_t)g_out_stride * g_out_slice;
    int cw = g_vis_w / 2;
    int ch = g_vis_h / 2;

    (void)filled;
    if (g_vis_w <= 0 || g_vis_h <= 0 || g_out_stride <= 0 || g_out_slice <= 0)
        return;

    write_plane(fp, y_plane, g_out_stride, g_vis_left, g_vis_top,
                g_vis_w, g_vis_h);
    if (g_pixfmt == PIX_NV12) {
        /* Interleaved UV: one row of `g_vis_w` bytes per chroma row. */
        write_plane(fp, uv_plane, g_out_stride, g_vis_left,
                    g_vis_top / 2, g_vis_w, ch);
    } else {
        /* I420: U plane then V plane, each g_vis_w/2 bytes per row. */
        const uint8_t *u = uv_plane;
        const uint8_t *v = uv_plane + (size_t)g_out_stride * g_out_slice / 4;
        write_plane(fp, u, g_out_stride / 2, g_vis_left / 2,
                    g_vis_top / 2, cw, ch);
        write_plane(fp, v, g_out_stride / 2, g_vis_left / 2,
                    g_vis_top / 2, cw, ch);
    }
}

/*
 * Re-read the output port definition and crop rectangle.  Called after the
 * decoder reports OMX_EventPortSettingsChanged, and once before the first
 * frame so that the crop is known even when the event never arrives.
 */
static void read_output_geometry(void)
{
    OMX_PARAM_PORTDEFINITIONTYPE def;
    OMX_CONFIG_RECTTYPE crop;
    OMX_ERRORTYPE rc;

    INIT_STRUCT(def);
    def.nPortIndex = 1;
    rc = p_GetParameter(g_comp, OMX_IndexParamPortDefinition, &def);
    if (rc != OMX_ErrorNone) {
        check("GetParameter(output geometry)", rc);
        return;
    }
    g_out_stride = (int)def.format.video.nStride;
    g_out_slice = (int)def.format.video.nSliceHeight;
    g_vis_w = (int)def.format.video.nFrameWidth;
    g_vis_h = (int)def.format.video.nFrameHeight;
    g_vis_left = 0;
    g_vis_top = 0;
    g_out_size = def.nBufferSize;

    INIT_STRUCT(crop);
    crop.nPortIndex = 1;
    rc = p_GetConfig(g_comp, OMX_IndexConfigCommonOutputCrop, &crop);
    if (rc == OMX_ErrorNone && crop.nWidth > 0 && crop.nHeight > 0) {
        g_vis_left = (int)crop.nLeft;
        g_vis_top = (int)crop.nTop;
        g_vis_w = (int)crop.nWidth;
        g_vis_h = (int)crop.nHeight;
    }

    if (g_verbose)
        fprintf(stderr,
                "omx-dec: output %dx%d crop (%d,%d) stride=%d slice=%d size=%u\n",
                g_vis_w, g_vis_h, g_vis_left, g_vis_top,
                g_out_stride, g_out_slice, (unsigned)g_out_size);
}

/*
 * True when the output port geometry no longer matches the buffers we hold.
 *
 * H.264 keeps the same stride/slice/buffer size after its sequence header, so
 * it needs no reset.  HEVC rounds the slice height up, and MPEG-4/H.263 start
 * at a 352x288 default and shrink to the real resolution; both need the buffers
 * reallocated or the decoder stops consuming input.
 */
static int output_needs_realloc(void)
{
    OMX_PARAM_PORTDEFINITIONTYPE def;
    INIT_STRUCT(def);
    def.nPortIndex = 1;
    if (p_GetParameter(g_comp, OMX_IndexParamPortDefinition, &def) != OMX_ErrorNone)
        return 0;
    if (def.nBufferSize != g_alloc_size)
        return 1;
    if ((int)def.format.video.nStride != g_out_stride)
        return 1;
    if ((int)def.format.video.nSliceHeight != g_out_slice)
        return 1;
    return 0;
}

/*
 * Output-port reconfiguration after OMX_EventPortSettingsChanged.
 *
 * This component never completes OMX_CommandPortDisable (the port is left in
 * PortInfo::DISABLING and SprdSimpleOMXComponent.cpp:621 then rejects the
 * matching PortEnable), so the Android "disable / free / enable / allocate"
 * sequence cannot work.  The sequence it does support is the teardown used at
 * the end of every playback: Idle, then Loaded, freeing all buffers, then
 * allocating again in Loaded and walking back up through Idle to Executing.
 *
 * Both input and output ports must be emptied for Loaded to complete, so the
 * caller has to resubmit the current access unit afterwards; submit_stream()
 * rewinds to the most recent SPS.
 */
static int alloc_input_buffers(OMX_U32 count, OMX_U32 size)
{
    int i;

    g_n_in = 0;
    for (i = 0; i < (int)count; i++) {
        OMX_ERRORTYPE rc = p_AllocateBuffer(g_comp, &g_in_buf[i], 0, NULL, size);
        if (rc != OMX_ErrorNone) {
            /* Some components only accept caller-owned input buffers. */
            uint8_t *mem = malloc(size);
            if (mem)
                rc = p_UseBuffer(g_comp, &g_in_buf[i], 0, NULL, size, mem);
        }
        if (rc != OMX_ErrorNone) {
            check("AllocateBuffer(input)", rc);
            break;
        }
        g_n_in++;
    }
    g_in_size = size;
    return g_n_in;
}

static int alloc_output_buffers(OMX_U32 count, OMX_U32 size)
{
    int i;

    g_n_out = 0;
    for (i = 0; i < (int)count; i++) {
        OMX_ERRORTYPE rc = p_AllocateBuffer(g_comp, &g_out_buf[i], 1, NULL, size);
        if (rc != OMX_ErrorNone) {
            check("AllocateBuffer(output)", rc);
            break;
        }
        g_n_out++;
    }
    g_alloc_size = size;
    return g_n_out;
}

static int reconfigure_output(void)
{
    OMX_PARAM_PORTDEFINITIONTYPE def;
    OMX_ERRORTYPE rc;
    OMX_U32 in_count, in_size, out_count, out_size;
    int i;

    if (g_verbose)
        fprintf(stderr, "omx-dec: reconfiguring output port\n");

    /* Executing -> Loaded is rejected; go through Idle first. */
    g_idle_done = 0;
    rc = p_SendCommand(g_comp, OMX_CommandStateSet, OMX_StateIdle, NULL);
    if (rc != OMX_ErrorNone) {
        check("SendCommand(Idle for reconfig)", rc);
        return -1;
    }
    if (!wait_flag(&g_idle_done, 15)) {
        fprintf(stderr, "omx-dec: Idle transition for reconfig timed out\n");
        return -1;
    }

    g_loaded_done = 0;
    rc = p_SendCommand(g_comp, OMX_CommandStateSet, OMX_StateLoaded, NULL);
    if (rc != OMX_ErrorNone) {
        check("SendCommand(Loaded for reconfig)", rc);
        return -1;
    }

    /* All buffers are back after Idle; drop anything still queued. */
    pthread_mutex_lock(&g_lock);
    g_outq_head = g_outq_tail = g_outq_count = 0;
    pthread_mutex_unlock(&g_lock);

    for (i = 0; i < g_n_out; i++) {
        if (g_out_buf[i]) {
            rc = p_FreeBuffer(g_comp, 1, g_out_buf[i]);
            if (rc != OMX_ErrorNone)
                check("FreeBuffer(output)", rc);
            g_out_buf[i] = NULL;
        }
    }
    g_n_out = 0;

    for (i = 0; i < g_n_in; i++) {
        if (g_in_buf[i]) {
            rc = p_FreeBuffer(g_comp, 0, g_in_buf[i]);
            if (rc != OMX_ErrorNone)
                check("FreeBuffer(input)", rc);
            g_in_buf[i] = NULL;
        }
    }
    g_n_in = 0;

    if (!wait_flag(&g_loaded_done, 15)) {
        fprintf(stderr, "omx-dec: Loaded transition for reconfig timed out\n");
        return -1;
    }

    INIT_STRUCT(def);
    def.nPortIndex = 0;
    rc = p_GetParameter(g_comp, OMX_IndexParamPortDefinition, &def);
    if (rc != OMX_ErrorNone) {
        check("GetParameter(input after change)", rc);
        return -1;
    }
    in_count = def.nBufferCountActual;
    in_size = def.nBufferSize;

    INIT_STRUCT(def);
    def.nPortIndex = 1;
    rc = p_GetParameter(g_comp, OMX_IndexParamPortDefinition, &def);
    if (rc != OMX_ErrorNone) {
        check("GetParameter(output after change)", rc);
        return -1;
    }
    out_count = def.nBufferCountActual;
    out_size = def.nBufferSize;

    if (in_count > MAX_BUF) in_count = MAX_BUF;
    if (out_count > MAX_BUF) out_count = MAX_BUF;
    if (in_count < 2) in_count = 2;
    if (out_count < 2) out_count = 2;
    if (in_size < 4096) in_size = 4096;

    if (alloc_input_buffers(in_count, in_size) < 2)
        return -1;
    if (alloc_output_buffers(out_count, out_size) < 1)
        return -1;

    g_idle_done = 0;
    rc = p_SendCommand(g_comp, OMX_CommandStateSet, OMX_StateIdle, NULL);
    if (rc != OMX_ErrorNone) {
        check("SendCommand(Idle after realloc)", rc);
        return -1;
    }
    if (!wait_flag(&g_idle_done, 15)) {
        fprintf(stderr, "omx-dec: Idle transition after realloc timed out\n");
        return -1;
    }

    g_exec_done = 0;
    rc = p_SendCommand(g_comp, OMX_CommandStateSet, OMX_StateExecuting, NULL);
    if (rc != OMX_ErrorNone) {
        check("SendCommand(Executing after realloc)", rc);
        return -1;
    }
    if (!wait_flag(&g_exec_done, 15)) {
        fprintf(stderr, "omx-dec: Executing transition after realloc timed out\n");
        return -1;
    }

    read_output_geometry();

    for (i = 0; i < g_n_out; i++) {
        g_out_buf[i]->nFilledLen = 0;
        g_out_buf[i]->nOffset = 0;
        if (p_FillThisBuffer(g_comp, g_out_buf[i]) != OMX_ErrorNone) {
            fprintf(stderr, "omx-dec: FillThisBuffer after realloc failed\n");
            g_error = 1;
            return -1;
        }
    }

    g_reconfigured = 1;
    if (g_verbose)
        fprintf(stderr,
                "omx-dec: output port reallocated: %d buffers of %u bytes\n",
                g_n_out, (unsigned)out_size);
    return 0;
}

static int drain_outputs(FILE *fp, int resubmit)
{
    int wrote = 0;

    for (;;) {
        OMX_BUFFERHEADERTYPE *b = NULL;
        pthread_mutex_lock(&g_lock);
        if (g_outq_count > 0) {
            b = g_outq[g_outq_tail];
            g_outq_tail = (g_outq_tail + 1) % OUTQ_LEN;
            g_outq_count--;
        }
        pthread_mutex_unlock(&g_lock);
        if (!b)
            break;

        if (b->nFilledLen > 0 &&
            !(b->nFlags & OMX_BUFFERFLAG_DECODEONLY) &&
            !(b->nFlags & OMX_BUFFERFLAG_EXTRADATA)) {
            write_frame(fp, (const uint8_t *)b->pBuffer + b->nOffset,
                        b->nFilledLen);
            wrote++;
        }

        if (resubmit && !g_eos && !(b->nFlags & OMX_BUFFERFLAG_EOS)) {
            b->nFilledLen = 0;
            b->nOffset = 0;
            if (p_FillThisBuffer(g_comp, b) != OMX_ErrorNone) {
                fprintf(stderr, "omx-dec: re-submit failed\n");
                g_error = 1;
            }
        }
    }
    fflush(fp);
    return wrote;
}

/*
 * Empty the output queue and apply any pending port reconfiguration.
 *
 * Hardware decoders stop consuming input once all output buffers are full, so
 * this must be called while the stream is being submitted, not only afterwards.
 */
static void service_decoder(FILE *out)
{
    g_frames += drain_outputs(out, 1);
    if (g_port_changed) {
        g_port_changed = 0;
        if (output_needs_realloc()) {
            if (reconfigure_output() != 0)
                g_error = 1;
        } else {
            read_output_geometry();
        }
    }
}

/*
 * Wait for an input buffer to come back, servicing the output queue meanwhile.
 * Returns 1 on success, 0 on timeout or decoder error.
 */
static int wait_input_serviced(FILE *out, int target)
{
    struct timespec deadline;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 30;

    while (g_input_done < target && !g_error) {
        struct timespec ts;
        struct timespec now;

        clock_gettime(CLOCK_REALTIME, &ts);
        now = ts;
        if (now.tv_sec > deadline.tv_sec)
            break;
        ts.tv_nsec += 50 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_mutex_lock(&g_lock);
        if (g_input_done < target && !g_error && g_outq_count == 0)
            pthread_cond_timedwait(&g_cond, &g_lock, &ts);
        pthread_mutex_unlock(&g_lock);
        service_decoder(out);
    }
    return g_input_done >= target;
}

/*
 * True when this NAL unit is a sequence parameter set, i.e. a safe place to
 * restart submission after an output-port reconfiguration.
 */
static int is_sps(const uint8_t *nal, OMX_VIDEO_CODINGTYPE coding)
{
    if (coding == OMX_VIDEO_CodingAVC)
        return (nal[0] & 0x1f) == 7;
    if (coding == OMX_VIDEO_CodingHEVC)
        return ((nal[0] >> 1) & 0x3f) == 33;
    return 0;
}

/*
 * Offset of the next whole-frame boundary at or after `from`.
 *
 * H.264/HEVC are split at every Annex B NAL start code; MPEG-4 is split at VOP
 * start codes (00 00 01 b6) and H.263 at its 22-bit picture start code
 * (00 00 10xxxxxx), which is not a 00 00 01 code at all.
 */
static size_t find_frame_start(const uint8_t *data, size_t len, size_t from,
                               OMX_VIDEO_CODINGTYPE coding)
{
    size_t i;

    if (coding == OMX_VIDEO_CodingMPEG4) {
        for (i = from; i + 4 <= len; i++)
            if (data[i] == 0 && data[i + 1] == 0 &&
                data[i + 2] == 1 && data[i + 3] == 0xb6)
                return i;
        return len;
    }
    if (coding == OMX_VIDEO_CodingH263) {
        for (i = from; i + 3 <= len; i++)
            if (data[i] == 0 && data[i + 1] == 0 &&
                (data[i + 2] & 0xfc) == 0x80)
                return i;
        return len;
    }
    return len;
}

/*
 * VP9 frames carry no byte-level delimiter, so the input has to be an IVF
 * container: a 32-byte "DKIF" header followed by frames, each with a 4-byte
 * little-endian size, an 8-byte timestamp, then the raw frame payload.
 */
static int submit_ivf(const uint8_t *data, size_t len, int fps, FILE *out)
{
    size_t pos = 32;
    long submitted = 0;
    int buf_index = 0;

    if (len < 32 || memcmp(data, "DKIF", 4) != 0) {
        fprintf(stderr, "omx-dec: vp9 input must be an IVF file (DKIF header)\n");
        return -1;
    }

    while (pos + 12 <= len && !g_error) {
        uint32_t sz = (uint32_t)data[pos] |
                      ((uint32_t)data[pos + 1] << 8) |
                      ((uint32_t)data[pos + 2] << 16) |
                      ((uint32_t)data[pos + 3] << 24);
        size_t start = pos + 12;
        size_t avail = len - start;

        if (sz > avail)
            sz = (uint32_t)avail;
        if (sz > g_in_size)
            sz = g_in_size;

        if (submitted >= g_n_in &&
            !wait_input_serviced(out, (int)(submitted - g_n_in + 1))) {
            fprintf(stderr, "omx-dec: input stall at frame %ld "
                    "(%d/%ld input buffers returned)\n",
                    submitted, g_input_done, submitted);
            return -1;
        }

        {
            OMX_BUFFERHEADERTYPE *b = g_in_buf[buf_index];
            memcpy(b->pBuffer, data + start, sz);
            b->nFilledLen = (OMX_U32)sz;
            b->nOffset = 0;
            b->nFlags = 0;
            b->nTimeStamp = (OMX_U64)submitted * (1000000 / (fps > 0 ? fps : 30));
            if (p_EmptyThisBuffer(g_comp, b) != OMX_ErrorNone) {
                fprintf(stderr, "omx-dec: EmptyThisBuffer failed at frame %ld\n",
                        submitted);
                return -1;
            }
        }
        submitted++;
        buf_index = (buf_index + 1) % g_n_in;
        pos = start + sz;

        service_decoder(out);

        if (g_reconfigured) {
            g_reconfigured = 0;
            submitted = 0;
            buf_index = 0;
            g_input_done = 0;
            pos = 32;
        }
    }

    if (submitted >= g_n_in)
        wait_input_serviced(out, (int)submitted);
    {
        OMX_BUFFERHEADERTYPE *b = g_in_buf[buf_index];
        b->nFilledLen = 0;
        b->nOffset = 0;
        b->nFlags = OMX_BUFFERFLAG_EOS;
        b->nTimeStamp = 0;
        if (p_EmptyThisBuffer(g_comp, b) != OMX_ErrorNone)
            fprintf(stderr, "omx-dec: EOS submit failed\n");
    }
    return (int)submitted;
}

/*
 * Split a stream into access units and feed each one in its own input buffer.
 *
 * H.264/HEVC: one NAL unit per buffer.
 * MPEG-4/H.263: one picture per buffer, with the leading sequence/vol headers
 * folded into the first picture, which is what Android's MPEG4Source does and
 * what the vendor decoder expects.
 * VP9: one IVF frame per buffer.
 *
 * If the decoder reconfigures its output port, every input buffer is released
 * as well (the Loaded reset), so the submission restarts from the most recent
 * SPS (or from the beginning for MPEG-4/H.263/VP9, whose headers are not
 * repeated).
 */
static int submit_stream(const uint8_t *data, size_t len,
                         OMX_VIDEO_CODINGTYPE coding, int fps, FILE *out)
{
    if (coding == OMX_VIDEO_CodingVP9)
        return submit_ivf(data, len, fps, out);

    int whole = (coding == OMX_VIDEO_CodingMPEG4 ||
                 coding == OMX_VIDEO_CodingH263);
    size_t pos = 0;
    size_t last_sps = 0;
    size_t first_end = (size_t)-1;
    long submitted = 0;
    int buf_index = 0;

    if (whole) {
        size_t f0 = find_frame_start(data, len, 0, coding);
        first_end = (f0 < len) ? find_frame_start(data, len, f0 + 1, coding)
                               : len;
    }

    while (pos < len && !g_error) {
        size_t start, next;
        size_t nal_len;

        if (whole) {
            start = pos;
            next = (first_end != (size_t)-1)
                       ? first_end
                       : find_frame_start(data, len, pos + 1, coding);
            first_end = (size_t)-1;
            if (next <= start)
                next = len;
        } else {
            /* Find the next start code (00 00 01 or 00 00 00 01). */
            while (pos + 3 < len &&
                   !(data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1))
                pos++;
            if (pos + 3 >= len)
                break;
            start = pos;

            next = start + 3;
            while (next + 3 < len &&
                   !(data[next] == 0 && data[next + 1] == 0 && data[next + 2] == 1))
                next++;
            if (next + 3 >= len)
                next = len;
        }
        nal_len = next - start;
        if (nal_len > g_in_size)
            nal_len = g_in_size;

        if (!whole && next > start + 3 && is_sps(data + start + 3, coding))
            last_sps = start;

        /*
         * The buffer being reused is the one submitted n_in slots ago, so
         * wait for that completion, not for "submitted >= n_in".
         */
        if (submitted >= g_n_in &&
            !wait_input_serviced(out, (int)(submitted - g_n_in + 1))) {
            fprintf(stderr, "omx-dec: input stall at unit %ld "
                    "(%d/%ld input buffers returned)\n",
                    submitted, g_input_done, submitted);
            return -1;
        }

        {
            OMX_BUFFERHEADERTYPE *b = g_in_buf[buf_index];
            memcpy(b->pBuffer, data + start, nal_len);
            b->nFilledLen = (OMX_U32)nal_len;
            b->nOffset = 0;
            b->nFlags = 0;
            b->nTimeStamp = (OMX_U64)submitted * (1000000 / (fps > 0 ? fps : 30));
            if (p_EmptyThisBuffer(g_comp, b) != OMX_ErrorNone) {
                fprintf(stderr, "omx-dec: EmptyThisBuffer failed at unit %ld\n",
                        submitted);
                return -1;
            }
        }
        submitted++;
        buf_index = (buf_index + 1) % g_n_in;
        pos = next;

        /* Keep the decoder fed and its output buffers recycled. */
        service_decoder(out);

        if (g_reconfigured) {
            g_reconfigured = 0;
            submitted = 0;
            buf_index = 0;
            g_input_done = 0;
            first_end = (size_t)-1;
            pos = whole ? 0 : last_sps;
            if (g_verbose)
                fprintf(stderr, "omx-dec: resubmitting from byte %zu\n", pos);
        }
    }

    /* End of stream on an empty buffer, as Android's OMXCodec does. */
    if (submitted >= g_n_in)
        wait_input_serviced(out, (int)submitted);
    {
        OMX_BUFFERHEADERTYPE *b = g_in_buf[buf_index];
        b->nFilledLen = 0;
        b->nOffset = 0;
        b->nFlags = OMX_BUFFERFLAG_EOS;
        b->nTimeStamp = 0;
        if (p_EmptyThisBuffer(g_comp, b) != OMX_ErrorNone)
            fprintf(stderr, "omx-dec: EOS submit failed\n");
    }
    return (int)submitted;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s -c h264|h265|h263|mpeg4|vp9 [options]\n"
        "  -c CODEC   decoder component (h264/h265/h263/mpeg4/vp9 on the VSP)\n"
        "  -i FILE    Annex B elementary stream (h264/h265/h263/mpeg4) or IVF\n"
        "             (vp9); '-' for stdin (default stdin)\n"
        "  -o FILE    raw decoded frames, '-' for stdout (default stdout)\n"
        "  -n FRAMES  stop after FRAMES output frames (default: until EOS)\n"
        "  -p PIXFMT  nv12 or i420 output (default nv12)\n"
        "  -q         quiet\n"
        "  -v         verbose per-frame logging\n"
        "  --info     print the component's port definitions and exit\n",
        prog);
}

int main(int argc, char **argv)
{
    const codec_def *cd = NULL;
    const char *codec = NULL;
    const char *in_path = "-";
    const char *out_path = "-";
    int fps = 30;
    long max_frames = -1;
    int info_only = 0;
    int opt;
    int i;

    /* Accept --info before getopt(), which would treat it as an error. */
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--info")) {
            int j;
            info_only = 1;
            for (j = i; j < argc - 1; j++)
                argv[j] = argv[j + 1];
            argc--;
            i--;
        }
    }

    while ((opt = getopt(argc, argv, "c:i:o:n:p:qvh")) != -1) {
        switch (opt) {
        case 'c': codec = optarg; break;
        case 'i': in_path = optarg; break;
        case 'o': out_path = optarg; break;
        case 'n': max_frames = atol(optarg); break;
        case 'p':
            if (!strcmp(optarg, "nv12")) g_pixfmt = PIX_NV12;
            else if (!strcmp(optarg, "i420")) g_pixfmt = PIX_I420;
            else { fprintf(stderr, "omx-dec: bad pixel format '%s'\n", optarg); return 2; }
            break;
        case 'q': g_quiet = 1; break;
        case 'v': g_verbose = 1; break;
        default: usage(argv[0]); return 2;
        }
    }
    if (!codec || !(cd = find_codec(codec))) {
        usage(argv[0]);
        return 2;
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, on_alarm);
    alarm(3600);

    /* Read the whole elementary stream.  Decoder probes are small. */
    uint8_t *stream = NULL;
    size_t stream_len = 0;
    if (!info_only) {
        FILE *fp = !strcmp(in_path, "-") ? stdin : fopen(in_path, "rb");
        size_t cap = 0;
        if (!fp) {
            fprintf(stderr, "omx-dec: cannot open %s: %s\n", in_path,
                    strerror(errno));
            return 2;
        }
        for (;;) {
            size_t got;
            if (stream_len == cap) {
                cap = cap ? cap * 2 : 65536;
                uint8_t *grown = realloc(stream, cap);
                if (!grown) {
                    fprintf(stderr, "omx-dec: out of memory\n");
                    return 1;
                }
                stream = grown;
            }
            got = fread(stream + stream_len, 1, cap - stream_len, fp);
            if (got == 0)
                break;
            stream_len += got;
        }
        if (fp != stdin) fclose(fp);
        if (stream_len == 0) {
            fprintf(stderr, "omx-dec: empty input\n");
            return 2;
        }
    }

    FILE *out = NULL;
    if (!info_only) {
        out = !strcmp(out_path, "-") ? stdout : fopen(out_path, "wb");
        if (!out) {
            fprintf(stderr, "omx-dec: cannot open %s: %s\n", out_path,
                    strerror(errno));
            return 2;
        }
    }

    /* ---- libhybris bootstrap ---- */
    void *hybris = dlopen("libhybris-common.so.1", RTLD_NOW);
    if (!hybris) {
        fprintf(stderr, "omx-dec: dlopen libhybris failed: %s\n", dlerror());
        return 1;
    }
    void *(*h_open)(const char *, int) =
        (void *(*)(const char *, int))dlsym(hybris, "hybris_dlopen");
    void *(*h_sym)(void *, const char *) =
        (void *(*)(void *, const char *))dlsym(hybris, "hybris_dlsym");
    if (!h_open || !h_sym) {
        fprintf(stderr, "omx-dec: missing libhybris symbols\n");
        return 1;
    }

    /* LP32 bionic: RTLD_GLOBAL == 0x2, RTLD_NODELETE == 0x1000. */
    void *shim = h_open("libgbm-shim.so", 0x2);
    void *ui = h_open("libui.so", RTLD_NOW);
    void *real_lock4 = ui ? h_sym(ui,
        "_ZN7android19GraphicBufferMapper4lockEPK13native_handlejRKNS_4RectEPPv") : NULL;
    void *real_unlock2 = ui ? h_sym(ui,
        "_ZN7android19GraphicBufferMapper6unlockEPK13native_handlePNS_4base14unique_fd_implINS4_13DefaultCloserEEE") : NULL;
    if (!shim) {
        fprintf(stderr, "omx-dec: cannot load libgbm-shim.so\n");
        return 1;
    }
    {
        void (*set_real)(void *, void *) =
            (void (*)(void *, void *))h_sym(shim, "gbm_shim_set_real");
        void (*set_handler)(void *) =
            (void (*)(void *))h_sym(shim, "omx_shim_set_handler");
        void *post_sync = h_sym(shim, "amessage_post_sync");
        if (set_real)
            set_real(real_lock4, real_unlock2);

        void *core = h_open("libsprd_omx_core.so", RTLD_NOW);
        if (!core) {
            fprintf(stderr, "omx-dec: cannot load libsprd_omx_core.so\n");
            return 1;
        }
        pfn_OMX_Init OMX_Init = (pfn_OMX_Init)h_sym(core, "OMX_Init");
        pfn_OMX_GetHandle OMX_GetHandle =
            (pfn_OMX_GetHandle)h_sym(core, "OMX_GetHandle");
        if (!OMX_Init || !OMX_GetHandle) {
            fprintf(stderr, "omx-dec: missing OMX core symbols\n");
            return 1;
        }
        check("OMX_Init", OMX_Init());

        /*
         * Load the vendor glue and pin the codec implementation before the
         * core does, so the handler patch is in place and unloading at exit
         * does not trip libhybris's linker.
         */
        h_open("libstagefrighthw.so", RTLD_NOW | 0x1000);
        if (patch_handler_alloc() != 0)
            fprintf(stderr, "omx-dec: warning: handler allocation patch failed\n");
        h_open(cd->lib, RTLD_NOW | 0x1000);

        const char *comp_name = cd->component;
        OMX_HANDLETYPE comp = NULL;
        OMX_ERRORTYPE rc = OMX_GetHandle(&comp, (OMX_STRING)comp_name,
                                         NULL, &g_cbs);
        check("OMX_GetHandle", rc);
        if (rc != OMX_ErrorNone || !comp) {
            fprintf(stderr, "omx-dec: cannot get decoder handle\n");
            return 1;
        }
        g_comp = comp;

        {
            void *priv = ((OMX_COMPONENTTYPE *)comp)->pComponentPrivate;
            void *handler = priv ? *(void **)((char *)priv + 100) : NULL;
            if (!handler) {
                fprintf(stderr, "omx-dec: no handler in component\n");
                return 1;
            }
            if (set_handler)
                set_handler(handler);
            if (patch_post_got(post_sync) != 0) {
                fprintf(stderr, "omx-dec: warning: AMessage::post patch failed\n");
                return 1;
            }
        }

        OMX_COMPONENTTYPE *ct = (OMX_COMPONENTTYPE *)comp;
        p_GetParameter = ct->GetParameter;
        p_SetParameter = ct->SetParameter;
        p_GetConfig = ct->GetConfig;
        p_SendCommand = ct->SendCommand;
        p_AllocateBuffer = ct->AllocateBuffer;
        p_UseBuffer = ct->UseBuffer;
        p_FreeBuffer = ct->FreeBuffer;
        p_EmptyThisBuffer = ct->EmptyThisBuffer;
        p_FillThisBuffer = ct->FillThisBuffer;
        p_GetState = ct->GetState;

        /* ---- input port: compressed ---- */
        OMX_PARAM_PORTDEFINITIONTYPE def;
        INIT_STRUCT(def);
        def.nPortIndex = 0;
        rc = p_GetParameter(comp, OMX_IndexParamPortDefinition, &def);
        check("GetParameter(input port)", rc);
        if (g_verbose || info_only)
            fprintf(stderr,
                    "omx-dec: input  %ux%u stride=%u slice=%u size=%u count=%u\n",
                    (unsigned)def.format.video.nFrameWidth,
                    (unsigned)def.format.video.nFrameHeight,
                    (unsigned)def.format.video.nStride,
                    (unsigned)def.format.video.nSliceHeight,
                    (unsigned)def.nBufferSize,
                    (unsigned)def.nBufferCountActual);

        OMX_VIDEO_PARAM_PORTFORMATTYPE fmt;
        INIT_STRUCT(fmt);
        fmt.nPortIndex = 0;
        fmt.eCompressionFormat = cd->coding;
        fmt.eColorFormat = OMX_COLOR_FormatUnused;
        check("SetParameter(input format)",
              p_SetParameter(comp, OMX_IndexParamVideoPortFormat, &fmt));

        /* ---- output port: raw NV12/I420 ---- */
        INIT_STRUCT(def);
        def.nPortIndex = 1;
        rc = p_GetParameter(comp, OMX_IndexParamPortDefinition, &def);
        check("GetParameter(output port)", rc);
        if (g_verbose || info_only)
            fprintf(stderr,
                    "omx-dec: output %ux%u stride=%u slice=%u size=%u count=%u\n",
                    (unsigned)def.format.video.nFrameWidth,
                    (unsigned)def.format.video.nFrameHeight,
                    (unsigned)def.format.video.nStride,
                    (unsigned)def.format.video.nSliceHeight,
                    (unsigned)def.nBufferSize,
                    (unsigned)def.nBufferCountActual);

        /*
         * Ask for the requested layout first.  The vendor decoder is happiest
         * with NV12, so if I420 is rejected fall back to NV12 rather than
         * failing; g_pixfmt always describes what the port really produces.
         */
        INIT_STRUCT(fmt);
        fmt.nPortIndex = 1;
        fmt.eCompressionFormat = OMX_VIDEO_CodingUnused;
        fmt.eColorFormat = (g_pixfmt == PIX_I420)
                               ? OMX_COLOR_FormatYUV420Planar
                               : OMX_COLOR_FormatYUV420SemiPlanar;
        rc = p_SetParameter(comp, OMX_IndexParamVideoPortFormat, &fmt);
        check(g_pixfmt == PIX_I420 ? "SetParameter(output I420)"
                                   : "SetParameter(output NV12)", rc);
        if (rc != OMX_ErrorNone) {
            fmt.eColorFormat = (g_pixfmt == PIX_I420)
                                   ? OMX_COLOR_FormatYUV420SemiPlanar
                                   : OMX_COLOR_FormatYUV420Planar;
            rc = p_SetParameter(comp, OMX_IndexParamVideoPortFormat, &fmt);
            check(fmt.eColorFormat == OMX_COLOR_FormatYUV420Planar
                      ? "SetParameter(output I420 fallback)"
                      : "SetParameter(output NV12 fallback)", rc);
            if (rc == OMX_ErrorNone) {
                g_pixfmt = (fmt.eColorFormat == OMX_COLOR_FormatYUV420Planar)
                               ? PIX_I420
                               : PIX_NV12;
            } else {
                fprintf(stderr,
                        "omx-dec: warning: decoder accepted neither I420 nor "
                        "NV12; output layout may be wrong\n");
            }
        }

        if (info_only) {
            fprintf(stderr, "omx-dec: component %s is present\n", comp_name);
            return 0;
        }

        /* ---- final geometry ---- */
        INIT_STRUCT(def);
        def.nPortIndex = 0;
        rc = p_GetParameter(comp, OMX_IndexParamPortDefinition, &def);
        check("GetParameter(final input)", rc);
        OMX_U32 in_count = def.nBufferCountActual;
        OMX_U32 in_size = def.nBufferSize;

        INIT_STRUCT(def);
        def.nPortIndex = 1;
        rc = p_GetParameter(comp, OMX_IndexParamPortDefinition, &def);
        check("GetParameter(final output)", rc);
        OMX_U32 out_count = def.nBufferCountActual;
        OMX_U32 out_size = def.nBufferSize;

        if (in_count > MAX_BUF) in_count = MAX_BUF;
        if (out_count > MAX_BUF) out_count = MAX_BUF;
        if (in_count < 2) in_count = 2;
        if (out_count < 2) out_count = 2;
        if (in_size < 4096) in_size = 4096;

        if (!g_quiet)
            fprintf(stderr,
                    "omx-dec: %s (%s) %u in / %u out buffers, in=%u out=%u\n",
                    codec, cd->hardware ? "hardware VSP" : "software",
                    (unsigned)in_count, (unsigned)out_count,
                    (unsigned)in_size, (unsigned)out_size);

        int i;

        if (alloc_input_buffers(in_count, in_size) < 2 ||
            alloc_output_buffers(out_count, out_size) < 1) {
            fprintf(stderr, "omx-dec: buffer allocation failed\n");
            return 1;
        }

        check("SendCommand(Idle)",
              p_SendCommand(comp, OMX_CommandStateSet, OMX_StateIdle, NULL));
        if (!wait_flag(&g_idle_done, 15)) {
            fprintf(stderr, "omx-dec: Idle transition failed (error=%d)\n", g_error);
            return 1;
        }
        check("SendCommand(Executing)",
              p_SendCommand(comp, OMX_CommandStateSet, OMX_StateExecuting, NULL));
        if (!wait_flag(&g_exec_done, 15)) {
            fprintf(stderr, "omx-dec: Executing transition failed (error=%d)\n", g_error);
            return 1;
        }

        for (i = 0; i < g_n_out; i++) {
            g_out_buf[i]->nFilledLen = 0;
            g_out_buf[i]->nOffset = 0;
            if (p_FillThisBuffer(comp, g_out_buf[i]) != OMX_ErrorNone) {
                fprintf(stderr, "omx-dec: initial FillThisBuffer failed\n");
                return 1;
            }
        }

        /* The crop may already be valid before the first frame. */
        read_output_geometry();

        int submitted = submit_stream(stream, stream_len, cd->coding, fps, out);
        free(stream);
        stream = NULL;
        if (submitted < 0) {
            fprintf(stderr, "omx-dec: stream submission failed\n");
            return 1;
        }
        if (!g_quiet)
            fprintf(stderr, "omx-dec: submitted %d input units\n", submitted);

        /* ---- decode loop ---- */
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 120;

        while (!g_eos && !g_error) {
            struct timespec ts;

            service_decoder(out);
            if (max_frames >= 0 && g_frames >= max_frames)
                break;

            clock_gettime(CLOCK_REALTIME, &ts);
            if (ts.tv_sec > deadline.tv_sec)
                break;
            ts.tv_nsec += 100 * 1000 * 1000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            pthread_mutex_lock(&g_lock);
            if (g_outq_count == 0 && !g_eos && !g_error)
                pthread_cond_timedwait(&g_cond, &g_lock, &ts);
            pthread_mutex_unlock(&g_lock);
        }

        g_frames += drain_outputs(out, 0);

        if (!g_quiet)
            fprintf(stderr,
                    "omx-dec: decoded %ld frames (%d fill callbacks, eos=%d, error=%d)\n",
                    g_frames, g_fill_count, g_eos, g_error);

        if (out != stdout) fclose(out);

        /*
         * Skip OMX_FreeHandle()/OMX_Deinit(): unloading the vendor codec
         * aborts inside libhybris's linker.  The kernel reclaims the mappings.
         */
        _exit((g_frames > 0 && !g_error) ? 0 : 7);
    }
}
