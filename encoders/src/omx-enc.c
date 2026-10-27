/*
 * omx-enc - encode raw YUV video with the Unisoc/SPRD hardware encoder.
 *
 * The vendor OpenMAX IL component only works through libhybris, and only
 * after two workarounds for Android 14 incompatibilities:
 *
 *   1. libstagefrighthw.so allocates its AHandlerReflector with new(56),
 *      but Android 14's AHandler::deliverMessage() writes up to +68.  The
 *      probe widens that allocation to 120 bytes.
 *   2. The vendor looper path corrupts the handler's mutex, so AMessage::post
 *      is redirected to a shim that calls onMessageReceived() synchronously.
 *
 * Input is tightly packed NV12 (default) or I420.  Output is a raw H.264 or
 * HEVC elementary stream, suitable for piping into ffmpeg.
 *
 * Usage:
 *   omx-enc -c h264|h265 [-w W] [-h H] [-r FPS] [-b BPS]
 *           [-p nv12|i420] [-i IN|-] [-o OUT|-] [-n FRAMES] [-q]
 *
 * Examples:
 *   omx-enc -c h264 -w 1280 -h 720 -r 30 -b 4000000 \
 *           -i frame.raw -o out.h264
 *
 *   ffmpeg -i in.mp4 -f rawvideo -pix_fmt nv12 - \
 *     | omx-enc -c h264 -w 1280 -h 720 -r 30 -b 4000000 -i - -o - \
 *     | ffmpeg -f h264 -i - -c copy out.mp4
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
typedef OMX_ERRORTYPE (*pfn_OMX_SendCommand)(OMX_HANDLETYPE, OMX_COMMANDTYPE,
                                             OMX_U32, OMX_PTR);
typedef OMX_ERRORTYPE (*pfn_OMX_AllocateBuffer)(OMX_HANDLETYPE, OMX_BUFFERHEADERTYPE **,
                                                OMX_U32, OMX_PTR, OMX_U32);
typedef OMX_ERRORTYPE (*pfn_OMX_FreeBuffer)(OMX_HANDLETYPE, OMX_U32,
                                            OMX_BUFFERHEADERTYPE *);
typedef OMX_ERRORTYPE (*pfn_OMX_EmptyThisBuffer)(OMX_HANDLETYPE,
                                                 OMX_BUFFERHEADERTYPE *);
typedef OMX_ERRORTYPE (*pfn_OMX_FillThisBuffer)(OMX_HANDLETYPE,
                                                OMX_BUFFERHEADERTYPE *);
typedef OMX_ERRORTYPE (*pfn_OMX_GetState)(OMX_HANDLETYPE, OMX_STATETYPE *);

static pfn_OMX_GetParameter    p_GetParameter;
static pfn_OMX_SetParameter    p_SetParameter;
static pfn_OMX_SendCommand     p_SendCommand;
static pfn_OMX_AllocateBuffer  p_AllocateBuffer;
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

static OMX_HANDLETYPE g_comp;
static int g_quiet;

#define OUTQ_LEN 32
static OMX_BUFFERHEADERTYPE *g_outq[OUTQ_LEN];
static int g_outq_head, g_outq_tail, g_outq_count;

#define MAX_BUF 8

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
    static const char msg[] = "omx-enc: TIMEOUT: encoder did not respond\n";
    write(2, msg, sizeof(msg) - 1);
    _exit(3);
}

static OMX_ERRORTYPE cb_event(OMX_HANDLETYPE h, OMX_PTR app, OMX_EVENTTYPE e,
                              OMX_U32 d1, OMX_U32 d2, OMX_PTR p)
{
    (void)h; (void)app; (void)p;
    if (e == OMX_EventCmdComplete) {
        if (d1 == OMX_CommandStateSet) {
            if (d2 == OMX_StateIdle) g_idle_done = 1;
            else if (d2 == OMX_StateExecuting) g_exec_done = 1;
        }
    } else if (e == OMX_EventError) {
        g_error = 1;
        g_error_code = (OMX_ERRORTYPE)d1;
        fprintf(stderr, "omx-enc: encoder error event 0x%x\n", (unsigned)d1);
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

static int wait_input_done(int target)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 20;
    pthread_mutex_lock(&g_lock);
    while (g_input_done < target && !g_error) {
        if (pthread_cond_timedwait(&g_cond, &g_lock, &ts) != 0)
            break;
    }
    pthread_mutex_unlock(&g_lock);
    return g_input_done >= target;
}

static void drain_outputs(FILE *fp, int resubmit)
{
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
        if (b->nFilledLen > 0) {
            if (fwrite((uint8_t *)b->pBuffer + b->nOffset, 1,
                       b->nFilledLen, fp) != b->nFilledLen) {
                fprintf(stderr, "omx-enc: write failed: %s\n", strerror(errno));
                g_error = 1;
            }
        }
        if (resubmit && !(b->nFlags & OMX_BUFFERFLAG_EOS)) {
            b->nFilledLen = 0;
            b->nOffset = 0;
            if (p_FillThisBuffer(g_comp, b) != OMX_ErrorNone) {
                fprintf(stderr, "omx-enc: re-submit failed\n");
                g_error = 1;
            }
        }
    }
    fflush(fp);
}

enum { PIX_NV12 = 0, PIX_I420 = 1 };

/*
 * Copy one tightly packed frame into the encoder's strided buffer.
 * NV12: Y plane then interleaved UV.  I420: Y, U, V planes.
 */
static void copy_frame(uint8_t *dst, int stride, int slice, int w, int h,
                       const uint8_t *src, int pixfmt)
{
    int y;
    uint8_t *duv = dst + (size_t)stride * slice;

    for (y = 0; y < h; y++)
        memcpy(dst + (size_t)y * stride, src + (size_t)y * w, w);

    if (pixfmt == PIX_NV12) {
        const uint8_t *uv = src + (size_t)w * h;
        for (y = 0; y < h / 2; y++)
            memcpy(duv + (size_t)y * stride, uv + (size_t)y * w, w);
    } else {
        const uint8_t *u = src + (size_t)w * h;
        const uint8_t *v = u + (size_t)(w / 2) * (h / 2);
        for (y = 0; y < h / 2; y++) {
            uint8_t *row = duv + (size_t)y * stride;
            int x;
            for (x = 0; x < w / 2; x++) {
                row[2 * x]     = u[(size_t)y * (w / 2) + x];
                row[2 * x + 1] = v[(size_t)y * (w / 2) + x];
            }
        }
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s -c h264|h265 [options]\n"
        "  -c CODEC   h264 or h265 (required)\n"
        "  -w WIDTH   frame width  (default 1280)\n"
        "  -h HEIGHT  frame height (default 720)\n"
        "  -r FPS     frame rate   (default 30)\n"
        "  -b BPS     target bitrate in bits/s (default 4000000)\n"
        "  -p PIXFMT  nv12 or i420 input (default nv12)\n"
        "  -i FILE    raw input, '-' for stdin (default stdin)\n"
        "  -o FILE    output elementary stream, '-' for stdout (default stdout)\n"
        "  -n FRAMES  stop after FRAMES frames (default: until EOF)\n"
        "  -q         quiet\n"
        "  -v         verbose per-frame logging\n",
        prog);
}

int main(int argc, char **argv)
{
    const char *codec = NULL;
    const char *in_path = "-";
    const char *out_path = "-";
    int width = 1280, height = 720, fps = 30, verbose = 0;
    long bitrate = 4000000;
    long max_frames = -1;
    int pixfmt = PIX_NV12;
    int opt;

    while ((opt = getopt(argc, argv, "c:w:h:r:b:p:i:o:n:qvh")) != -1) {
        switch (opt) {
        case 'c': codec = optarg; break;
        case 'w': width = atoi(optarg); break;
        case 'h': height = atoi(optarg); break;
        case 'r': fps = atoi(optarg); break;
        case 'b': bitrate = atol(optarg); break;
        case 'p':
            if (!strcmp(optarg, "nv12")) pixfmt = PIX_NV12;
            else if (!strcmp(optarg, "i420")) pixfmt = PIX_I420;
            else { fprintf(stderr, "omx-enc: bad pixel format '%s'\n", optarg); return 2; }
            break;
        case 'i': in_path = optarg; break;
        case 'o': out_path = optarg; break;
        case 'n': max_frames = atol(optarg); break;
        case 'q': g_quiet = 1; break;
        case 'v': verbose = 1; break;
        default: usage(argv[0]); return 2;
        }
    }
    if (!codec || (strcmp(codec, "h264") && strcmp(codec, "h265"))) {
        usage(argv[0]);
        return 2;
    }
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
        fprintf(stderr, "omx-enc: width and height must be positive and even\n");
        return 2;
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, on_alarm);
    alarm(3600);

    FILE *fp = NULL;
    if (!strcmp(in_path, "-")) {
        fp = stdin;
    } else {
        fp = fopen(in_path, "rb");
        if (!fp) {
            fprintf(stderr, "omx-enc: cannot open %s: %s\n", in_path, strerror(errno));
            return 2;
        }
    }
    FILE *out = NULL;
    if (!strcmp(out_path, "-")) {
        out = stdout;
    } else {
        out = fopen(out_path, "wb");
        if (!out) {
            fprintf(stderr, "omx-enc: cannot open %s: %s\n", out_path, strerror(errno));
            return 2;
        }
    }

    /* ---- libhybris bootstrap ---- */
    void *hybris = dlopen("libhybris-common.so.1", RTLD_NOW);
    if (!hybris) {
        fprintf(stderr, "omx-enc: dlopen libhybris failed: %s\n", dlerror());
        return 1;
    }
    void *(*h_open)(const char *, int) =
        (void *(*)(const char *, int))dlsym(hybris, "hybris_dlopen");
    void *(*h_sym)(void *, const char *) =
        (void *(*)(void *, const char *))dlsym(hybris, "hybris_dlsym");
    if (!h_open || !h_sym) {
        fprintf(stderr, "omx-enc: missing libhybris symbols\n");
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
        fprintf(stderr, "omx-enc: cannot load libgbm-shim.so\n");
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
            fprintf(stderr, "omx-enc: cannot load libsprd_omx_core.so\n");
            return 1;
        }
        pfn_OMX_Init OMX_Init = (pfn_OMX_Init)h_sym(core, "OMX_Init");
        pfn_OMX_GetHandle OMX_GetHandle =
            (pfn_OMX_GetHandle)h_sym(core, "OMX_GetHandle");
        if (!OMX_Init || !OMX_GetHandle) {
            fprintf(stderr, "omx-enc: missing OMX core symbols\n");
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
            fprintf(stderr, "omx-enc: warning: handler allocation patch failed\n");
        if (!strcmp(codec, "h264"))
            h_open("libomx_avcenc_hw_sprd.so", RTLD_NOW | 0x1000);
        else
            h_open("libomx_hevcenc_hw_sprd.so", RTLD_NOW | 0x1000);

        const char *comp_name = !strcmp(codec, "h264")
            ? "OMX.sprd.h264.encoder" : "OMX.sprd.h265.encoder";
        OMX_HANDLETYPE comp = NULL;
        OMX_ERRORTYPE rc = OMX_GetHandle(&comp, (OMX_STRING)comp_name,
                                         NULL, &g_cbs);
        check("OMX_GetHandle", rc);
        if (rc != OMX_ErrorNone || !comp) {
            fprintf(stderr, "omx-enc: cannot get encoder handle\n");
            return 1;
        }
        g_comp = comp;

        {
            void *priv = ((OMX_COMPONENTTYPE *)comp)->pComponentPrivate;
            void *handler = priv ? *(void **)((char *)priv + 100) : NULL;
            if (!handler) {
                fprintf(stderr, "omx-enc: no handler in component\n");
                return 1;
            }
            if (set_handler)
                set_handler(handler);
            if (patch_post_got(post_sync) != 0) {
                fprintf(stderr, "omx-enc: warning: AMessage::post patch failed\n");
                return 1;
            }
        }

        OMX_COMPONENTTYPE *ct = (OMX_COMPONENTTYPE *)comp;
        p_GetParameter = ct->GetParameter;
        p_SetParameter = ct->SetParameter;
        p_SendCommand = ct->SendCommand;
        p_AllocateBuffer = ct->AllocateBuffer;
        p_FreeBuffer = ct->FreeBuffer;
        p_EmptyThisBuffer = ct->EmptyThisBuffer;
        p_FillThisBuffer = ct->FillThisBuffer;
        p_GetState = ct->GetState;

        /* ---- input port ---- */
        OMX_PARAM_PORTDEFINITIONTYPE def;
        INIT_STRUCT(def);
        def.nPortIndex = 0;
        rc = p_GetParameter(comp, OMX_IndexParamPortDefinition, &def);
        check("GetParameter(input port)", rc);
        def.format.video.nFrameWidth = width;
        def.format.video.nFrameHeight = height;
        def.format.video.nStride = width;
        def.format.video.nSliceHeight = height;
        def.format.video.xFramerate = (OMX_U32)fps << 16;
        def.format.video.nBitrate = (OMX_U32)bitrate;
        if (def.nBufferCountActual < 4) def.nBufferCountActual = 4;
        if (def.nBufferCountActual > MAX_BUF) def.nBufferCountActual = MAX_BUF;
        def.nBufferSize = (OMX_U32)width * height * 3 / 2;
        check("SetParameter(input port)",
              p_SetParameter(comp, OMX_IndexParamPortDefinition, &def));

        OMX_VIDEO_PARAM_PORTFORMATTYPE fmt;
        INIT_STRUCT(fmt);
        fmt.nPortIndex = 0;
        fmt.eCompressionFormat = OMX_VIDEO_CodingUnused;
        fmt.eColorFormat = OMX_COLOR_FormatYUV420SemiPlanar;
        rc = p_SetParameter(comp, OMX_IndexParamVideoPortFormat, &fmt);
        check("SetParameter(input NV12)", rc);
        if (rc != OMX_ErrorNone) {
            fmt.eColorFormat = OMX_COLOR_FormatYUV420Planar;
            check("SetParameter(input I420)",
                  p_SetParameter(comp, OMX_IndexParamVideoPortFormat, &fmt));
        }

        /* ---- output port ---- */
        INIT_STRUCT(def);
        def.nPortIndex = 1;
        rc = p_GetParameter(comp, OMX_IndexParamPortDefinition, &def);
        check("GetParameter(output port)", rc);
        def.format.video.nFrameWidth = width;
        def.format.video.nFrameHeight = height;
        def.format.video.nStride = width;
        def.format.video.nSliceHeight = height;
        def.format.video.xFramerate = (OMX_U32)fps << 16;
        def.format.video.nBitrate = (OMX_U32)bitrate;
        if (def.nBufferCountActual < 2) def.nBufferCountActual = 2;
        if (def.nBufferCountActual > MAX_BUF) def.nBufferCountActual = MAX_BUF;
        check("SetParameter(output port)",
              p_SetParameter(comp, OMX_IndexParamPortDefinition, &def));

        INIT_STRUCT(fmt);
        fmt.nPortIndex = 1;
        fmt.eCompressionFormat = !strcmp(codec, "h264")
            ? OMX_VIDEO_CodingAVC : OMX_VIDEO_CodingHEVC;
        fmt.eColorFormat = OMX_COLOR_FormatUnused;
        check("SetParameter(output format)",
              p_SetParameter(comp, OMX_IndexParamVideoPortFormat, &fmt));

        OMX_VIDEO_PARAM_BITRATETYPE br;
        INIT_STRUCT(br);
        br.nPortIndex = 1;
        br.eControlRate = OMX_Video_ControlRateVariable;
        br.nTargetBitrate = (OMX_U32)bitrate;
        check("SetParameter(bitrate)",
              p_SetParameter(comp, OMX_IndexParamVideoBitrate, &br));

        if (!strcmp(codec, "h264")) {
            OMX_VIDEO_PARAM_AVCTYPE avc;
            INIT_STRUCT(avc);
            avc.nPortIndex = 1;
            avc.eProfile = OMX_VIDEO_AVCProfileBaseline;
            if (width * height > 1280 * 720)
                avc.eLevel = OMX_VIDEO_AVCLevel42;
            else
                avc.eLevel = OMX_VIDEO_AVCLevel31;
            avc.nAllowedPictureTypes = OMX_VIDEO_PictureTypeI |
                                       OMX_VIDEO_PictureTypeP;
            avc.bFrameMBsOnly = OMX_TRUE;
            avc.bEntropyCodingCABAC = OMX_FALSE;
            avc.nRefFrames = 1;
            avc.nPFrames = (OMX_U32)(fps > 1 ? fps - 1 : 1);
            avc.nBFrames = 0;
            check("SetParameter(AVC params)",
                  p_SetParameter(comp, OMX_IndexParamVideoAvc, &avc));
        }

        /* ---- final geometry ---- */
        INIT_STRUCT(def);
        def.nPortIndex = 0;
        rc = p_GetParameter(comp, OMX_IndexParamPortDefinition, &def);
        check("GetParameter(final input)", rc);
        OMX_U32 in_count = def.nBufferCountActual;
        OMX_U32 in_size = def.nBufferSize;
        int in_stride = (int)def.format.video.nStride;
        int in_slice = (int)def.format.video.nSliceHeight;

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
        if (in_size < (OMX_U32)(in_stride * in_slice * 3 / 2))
            in_size = (OMX_U32)(in_stride * in_slice * 3 / 2);

        if (!g_quiet)
            fprintf(stderr,
                    "omx-enc: %s %dx%d @%dfps %ldbps, stride=%d slice=%d, "
                    "%u in / %u out buffers\n",
                    codec, width, height, fps, bitrate,
                    in_stride, in_slice, in_count, out_count);

        OMX_BUFFERHEADERTYPE *in_buf[MAX_BUF], *out_buf[MAX_BUF];
        int n_in = 0, n_out = 0, i;

        for (i = 0; i < (int)in_count; i++) {
            rc = p_AllocateBuffer(comp, &in_buf[i], 0, NULL, in_size);
            if (rc != OMX_ErrorNone) {
                check("AllocateBuffer(input)", rc);
                break;
            }
            n_in++;
        }
        for (i = 0; i < (int)out_count; i++) {
            rc = p_AllocateBuffer(comp, &out_buf[i], 1, NULL, out_size);
            if (rc != OMX_ErrorNone) {
                check("AllocateBuffer(output)", rc);
                break;
            }
            n_out++;
        }
        if (n_in < 2 || n_out < 1) {
            fprintf(stderr, "omx-enc: buffer allocation failed\n");
            return 1;
        }

        check("SendCommand(Idle)",
              p_SendCommand(comp, OMX_CommandStateSet, OMX_StateIdle, NULL));
        if (!wait_flag(&g_idle_done, 15)) {
            fprintf(stderr, "omx-enc: Idle transition failed (error=%d)\n", g_error);
            return 1;
        }
        check("SendCommand(Executing)",
              p_SendCommand(comp, OMX_CommandStateSet, OMX_StateExecuting, NULL));
        if (!wait_flag(&g_exec_done, 15)) {
            fprintf(stderr, "omx-enc: Executing transition failed (error=%d)\n", g_error);
            return 1;
        }

        for (i = 0; i < n_out; i++) {
            out_buf[i]->nFilledLen = 0;
            out_buf[i]->nOffset = 0;
            if (p_FillThisBuffer(comp, out_buf[i]) != OMX_ErrorNone) {
                fprintf(stderr, "omx-enc: initial FillThisBuffer failed\n");
                return 1;
            }
        }

        /* ---- encode loop ---- */
        size_t frame_size = (size_t)width * height * 3 / 2;
        uint8_t *src = malloc(frame_size);
        long frame = 0, submitted = 0;
        int eof = 0;
        if (!src) {
            fprintf(stderr, "omx-enc: out of memory\n");
            return 1;
        }

        while (!g_error && !eof) {
            size_t got;
            if (max_frames >= 0 && frame >= max_frames)
                break;
            got = fread(src, 1, frame_size, fp);
            if (got == 0)
                break;
            if (got < frame_size) {
                /* pad the last partial frame with black */
                memset(src + got, 0, frame_size - got);
                eof = 1;
            }
            drain_outputs(out, 1);
            if (frame >= n_in && !wait_input_done((int)(frame - n_in + 1))) {
                fprintf(stderr, "omx-enc: input stall at frame %ld\n", frame);
                break;
            }
            {
                OMX_BUFFERHEADERTYPE *b = in_buf[frame % n_in];
                copy_frame(b->pBuffer, in_stride, in_slice, width, height,
                           src, pixfmt);
                b->nFilledLen = (OMX_U32)(in_stride * in_slice * 3 / 2);
                b->nOffset = 0;
                b->nFlags = 0;
                b->nTimeStamp = (OMX_U64)frame * (1000000 / (fps > 0 ? fps : 30));
                if (p_EmptyThisBuffer(comp, b) != OMX_ErrorNone) {
                    fprintf(stderr, "omx-enc: EmptyThisBuffer failed at frame %ld\n",
                            frame);
                    break;
                }
            }
            submitted++;
            frame++;
            if (verbose)
                fprintf(stderr, "omx-enc: frame %ld\n", frame);
        }
        free(src);
        if (!g_quiet)
            fprintf(stderr, "omx-enc: submitted %ld frames\n", submitted);

        if (submitted > 0)
            wait_input_done((int)submitted);
        drain_outputs(out, 1);

        /* EOS */
        in_buf[0]->nFilledLen = 0;
        in_buf[0]->nOffset = 0;
        in_buf[0]->nFlags = OMX_BUFFERFLAG_EOS;
        in_buf[0]->nTimeStamp = 0;
        p_EmptyThisBuffer(comp, in_buf[0]);

        {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 30;
            pthread_mutex_lock(&g_lock);
            while (!g_eos && !g_error) {
                if (pthread_cond_timedwait(&g_cond, &g_lock, &ts) != 0)
                    break;
            }
            pthread_mutex_unlock(&g_lock);
        }
        drain_outputs(out, 0);

        if (fp != stdin) fclose(fp);
        if (out != stdout) fclose(out);

        p_SendCommand(comp, OMX_CommandStateSet, OMX_StateIdle, NULL);
        usleep(200000);
        for (i = 0; i < n_out; i++)
            p_FreeBuffer(comp, 1, out_buf[i]);
        for (i = 0; i < n_in; i++)
            p_FreeBuffer(comp, 0, in_buf[i]);

        if (!g_quiet)
            fprintf(stderr, "omx-enc: done (%d fill callbacks, eos=%d)\n",
                    g_fill_count, g_eos);

        /*
         * Skip OMX_FreeHandle()/OMX_Deinit(): unloading the vendor codec
         * aborts inside libhybris's linker.  The kernel reclaims the mappings.
         */
        _exit((g_fill_count > 0 && g_eos && !g_error) ? 0 : 7);
    }
}
