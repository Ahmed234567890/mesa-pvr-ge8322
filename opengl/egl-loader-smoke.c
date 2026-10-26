/* Diagnose EGL/GLES libraries that an application loads with dlopen(). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>

int main(void)
{
    const char *names[] = { "libEGL.so", "libEGL.so.1", "libGLESv2.so",
                            "libGLESv2.so.2" };
    for (unsigned int i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        dlerror();
        void *handle = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        const char *error = dlerror();
        if (!handle) {
            fprintf(stderr, "dlopen(%s): %s\n", names[i],
                    error ? error : "unknown error");
            continue;
        }

        printf("dlopen(%s): OK\n", names[i]);
        const char *symbols[] = { "eglGetDisplay", "eglInitialize",
                                  "eglCreatePbufferSurface", "glGetString" };
        for (unsigned int j = 0; j < sizeof(symbols) / sizeof(symbols[0]); ++j) {
            dlerror();
            void *symbol = dlsym(handle, symbols[j]);
            error = dlerror();
            if (error)
                printf("  dlsym(%s): %s\n", symbols[j], error);
            else
                printf("  dlsym(%s): %p\n", symbols[j], symbol);
        }
        dlclose(handle);
    }
    return 0;
}
