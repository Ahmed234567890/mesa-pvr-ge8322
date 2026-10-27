/*
 * ABI shim for the SPRD vendor H.264/H.265 encoders.
 *
 * The vendor blob (built against an older Android) references:
 *   android::GraphicBufferMapper::lock(handle, usage, Rect, void**, int*, int*)
 *   android::GraphicBufferMapper::unlock(handle)
 * Android 14's libui.so only exports the 4-argument lock() and the
 * 2-argument unlock(handle, unique_fd*).  Bionic would bind these lazily and
 * the calls are normally never made for OMX-allocated buffers; libhybris
 * resolves eagerly, so the missing symbols abort the load.
 *
 * This library is built freestanding (no libc dependency) so the Android
 * linker can load it.  The caller passes the real libui entry points in with
 * gbm_shim_set_real().
 */
#include <cstdint>

struct native_handle;
namespace android { class Rect; }

extern "C" {
/*
 * Android 14 libs are built with CFI.  Their indirect calls go through
 * __cfi_slowpath, which needs the linker-populated CFI shadow.  libhybris's
 * older linker never creates it, so the first check dereferences NULL.
 * These no-ops keep the checks from crashing; the calls themselves are
 * normal C++ virtual/dispatch calls.
 */
void __cfi_slowpath(unsigned long long, void *) {}
void __cfi_slowpath_diag(unsigned long long, void *, void *) {}
void __cfi_init(void *) {}

int gbm_shim_stats[4] = { 0, 0, 0, 0 };

/*
 * Synchronous message delivery.
 *
 * SprdSimpleOMXComponent::sendCommand/emptyThisBuffer/fillThisBuffer build an
 * AMessage and call AMessage::post(), which queues it on the component's
 * ALooper.  That path is unusable here: the vendor blob's AHandlerReflector
 * was built against an older AHandler layout, so Android 14's
 * AHandler::deliverMessage() writes past the end of the 56-byte object and
 * also mismanages the RefBase control block, freeing the mutex libhybris
 * allocated for mLock.
 *
 * The caller patches the AMessage::post GOT slot in libstagefrighthw.so to
 * point here instead (see omx_shim_set_handler()).  We hand the message
 * straight to the handler's onMessageReceived(), which is exactly what the
 * looper would have done.
 */
static void *g_handler;

void omx_shim_set_handler(void *handler)
{
    g_handler = handler;
}

int amessage_post_sync(void *self)
{
    void *msg = self;
    void **vt;
    void (*onmsg)(void *, const void *);

    if (!g_handler)
        return -38; /* -ENOSYS */
    vt = *(void ***)g_handler;
    onmsg = (void (*)(void *, const void *))vt[6];
    if (!onmsg)
        return -38;
    onmsg(g_handler, &msg);
    return 0;
}
/* stats[0] = lock calls, stats[1] = unlock calls,
 * stats[2] = last lock rc, stats[3] = real pointers installed */
}

namespace android {

class GraphicBufferMapper {
public:
    int lock(const native_handle *handle, uint32_t usage, const Rect &bounds,
             void **vaddr, int32_t *outBytesPerPixel,
             int32_t *outBytesPerStride);
    int unlock(const native_handle *handle);
};

typedef int (*lock4_fn)(const native_handle *, uint32_t, const android::Rect &,
                        void **);
typedef int (*unlock2_fn)(const native_handle *, void *);

static lock4_fn g_lock4;
static unlock2_fn g_unlock2;

int GraphicBufferMapper::lock(const native_handle *handle, uint32_t usage,
                              const Rect &bounds, void **vaddr,
                              int32_t *outBytesPerPixel,
                              int32_t *outBytesPerStride)
{
    gbm_shim_stats[0]++;
    if (!g_lock4)
        return -22; /* -EINVAL */
    void *v = 0;
    int rc = g_lock4(handle, usage, bounds, &v);
    gbm_shim_stats[2] = rc;
    if (rc == 0) {
        *vaddr = v;
        /* The caller expects pixel/stride byte counts; zero means unknown. */
        if (outBytesPerPixel) *outBytesPerPixel = 0;
        if (outBytesPerStride) *outBytesPerStride = 0;
    }
    return rc;
}

int GraphicBufferMapper::unlock(const native_handle *handle)
{
    gbm_shim_stats[1]++;
    if (!g_unlock2)
        return -22;
    return g_unlock2(handle, 0);
}

} /* namespace android */

extern "C" void gbm_shim_set_real(void *lock4, void *unlock2)
{
    android::g_lock4 = (android::lock4_fn)lock4;
    android::g_unlock2 = (android::unlock2_fn)unlock2;
    gbm_shim_stats[3] = (lock4 && unlock2) ? 1 : 0;
}
