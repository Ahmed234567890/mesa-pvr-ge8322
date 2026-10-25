// SPDX-License-Identifier: MIT
/*
 * libahb-wrapper - AHardwareBuffer entry points for a glibc Vulkan process.
 *
 * The Arm window-system-integration layer looks up
 * AHardwareBuffer_sendHandleToUnixSocket and AHardwareBuffer_release with
 * dlsym(RTLD_DEFAULT, ...).  Those symbols live in bionic's libandroid.so,
 * which cannot be linked directly into a glibc process.  This shim resolves
 * them through libhybris' Android linker and exports thin forwarding
 * wrappers under the bionic names.
 *
 * Only the four entry points used by the bridge are provided.  The ABI is
 * plain C and does not depend on the layout of AHardwareBuffer or
 * AHardwareBuffer_Desc, so opaque pointers are sufficient here.
 */

#include <dlfcn/dlfcn.h>

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* libhybris' dlfcn header declares the entry points but not the RTLD flags. */
#ifndef RTLD_LAZY
#define RTLD_LAZY 1
#endif

typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct AHardwareBuffer_Desc AHardwareBuffer_Desc;
typedef struct native_handle native_handle_t;

typedef int (*ahb_allocate_fn)(const AHardwareBuffer_Desc *desc, AHardwareBuffer **buffer);
typedef void (*ahb_release_fn)(AHardwareBuffer *buffer);
typedef int (*ahb_send_handle_fn)(const AHardwareBuffer *buffer, int socket);
typedef const native_handle_t *(*ahb_get_handle_fn)(const AHardwareBuffer *buffer);

struct ahb_api
{
   ahb_allocate_fn allocate;
   ahb_release_fn release;
   ahb_send_handle_fn send_handle;
   ahb_get_handle_fn get_handle;
};

static struct ahb_api api;
static pthread_once_t api_once = PTHREAD_ONCE_INIT;
static int api_ready;

#define AHB_EXPORT __attribute__((visibility("default")))

static int debug_enabled(void)
{
   const char *value = getenv("PVR_DEBUG");
   return value != NULL && value[0] == '1';
}

static void load_api(void)
{
   void *android = hybris_dlopen("libandroid.so", RTLD_LAZY);
   if (android == NULL)
   {
      if (debug_enabled())
      {
         fprintf(stderr, "ahb-wrapper: cannot open libandroid.so: %s\n", hybris_dlerror());
      }
      return;
   }

   api.allocate = (ahb_allocate_fn)hybris_dlsym(android, "AHardwareBuffer_allocate");
   api.release = (ahb_release_fn)hybris_dlsym(android, "AHardwareBuffer_release");
   api.send_handle = (ahb_send_handle_fn)hybris_dlsym(android, "AHardwareBuffer_sendHandleToUnixSocket");
   api.get_handle = (ahb_get_handle_fn)hybris_dlsym(android, "AHardwareBuffer_getNativeHandle");

   /* The WSI layer requires exactly these two.  allocate/getNativeHandle are
    * resolved opportunistically and fail cleanly if a vendor libandroid.so
    * does not export them. */
   if (api.release == NULL || api.send_handle == NULL)
   {
      if (debug_enabled())
      {
         fprintf(stderr,
                 "ahb-wrapper: libandroid.so is missing %s%s%s\n",
                 api.release == NULL ? "AHardwareBuffer_release" : "",
                 (api.release == NULL && api.send_handle == NULL) ? ", " : "",
                 api.send_handle == NULL ? "AHardwareBuffer_sendHandleToUnixSocket" : "");
      }
      return;
   }

   api_ready = 1;
   if (debug_enabled())
   {
      fprintf(stderr, "ahb-wrapper: AHardwareBuffer entry points resolved\n");
   }
}

static int ensure_api(void)
{
   pthread_once(&api_once, load_api);
   return api_ready;
}

AHB_EXPORT void AHardwareBuffer_release(AHardwareBuffer *buffer)
{
   if (ensure_api())
   {
      api.release(buffer);
   }
}

AHB_EXPORT int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer *buffer, int socket)
{
   if (!ensure_api())
   {
      return -ENODEV;
   }
   return api.send_handle(buffer, socket);
}

AHB_EXPORT int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *desc, AHardwareBuffer **buffer)
{
   if (!ensure_api() || api.allocate == NULL)
   {
      return -ENODEV;
   }
   return api.allocate(desc, buffer);
}

AHB_EXPORT const native_handle_t *AHardwareBuffer_getNativeHandle(const AHardwareBuffer *buffer)
{
   if (!ensure_api() || api.get_handle == NULL)
   {
      return NULL;
   }
   return api.get_handle(buffer);
}
