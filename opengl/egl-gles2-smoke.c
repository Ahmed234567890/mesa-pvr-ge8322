/*
 * Minimal EGL/GLES2 pbuffer smoke test for the vendor-GLES/libhybris path.
 * This is a diagnostic, not a Mesa test. It should be linked against
 * libhybris EGL/GLES wrappers and run with HYBRIS_EGLPLATFORM=null.
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <stdio.h>
#include <stdlib.h>

static void fail_egl(const char *where)
{
    fprintf(stderr, "%s failed: EGL error 0x%04x\n", where, eglGetError());
    exit(1);
}

static void require_egl(EGLBoolean ok, const char *where)
{
    if (ok != EGL_TRUE)
        fail_egl(where);
}

int main(void)
{
    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    const EGLint pbuffer_attrs[] = {
        EGL_WIDTH, 64,
        EGL_HEIGHT, 64,
        EGL_NONE
    };
    const EGLint context_attrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY)
        fail_egl("eglGetDisplay");

    EGLint major = 0;
    EGLint minor = 0;
    require_egl(eglInitialize(display, &major, &minor), "eglInitialize");
    require_egl(eglBindAPI(EGL_OPENGL_ES_API), "eglBindAPI");

    EGLConfig config;
    EGLint config_count = 0;
    require_egl(eglChooseConfig(display, config_attrs, &config, 1, &config_count),
                "eglChooseConfig");
    if (config_count < 1) {
        fprintf(stderr, "No EGL config supports an RGBA8 GLES2 pbuffer\n");
        return 1;
    }

    EGLSurface surface = eglCreatePbufferSurface(display, config, pbuffer_attrs);
    if (surface == EGL_NO_SURFACE)
        fail_egl("eglCreatePbufferSurface");

    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
                                          context_attrs);
    if (context == EGL_NO_CONTEXT)
        fail_egl("eglCreateContext");
    require_egl(eglMakeCurrent(display, surface, surface, context),
                "eglMakeCurrent");

    const GLubyte *vendor = glGetString(GL_VENDOR);
    const GLubyte *renderer = glGetString(GL_RENDERER);
    const GLubyte *version = glGetString(GL_VERSION);
    if (!vendor || !renderer || !version) {
        fprintf(stderr, "glGetString returned NULL\n");
        return 1;
    }
    printf("EGL version: %d.%d\n", major, minor);
    printf("GL vendor: %s\n", vendor);
    printf("GL renderer: %s\n", renderer);
    printf("GL version: %s\n", version);

    glViewport(0, 0, 64, 64);
    glClearColor(0.25f, 0.50f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();

    GLubyte pixel[4] = {0, 0, 0, 0};
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    GLenum gl_error = glGetError();
    printf("center pixel: %u %u %u %u (glError=0x%04x)\n",
           pixel[0], pixel[1], pixel[2], pixel[3], gl_error);
    if (gl_error != GL_NO_ERROR || pixel[0] < 62 || pixel[0] > 66 ||
        pixel[1] < 126 || pixel[1] > 130 || pixel[2] < 190 || pixel[2] > 194 ||
        pixel[3] < 253) {
        fprintf(stderr, "GLES clear/readback did not match expected RGBA values\n");
        return 1;
    }

    require_egl(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                               EGL_NO_CONTEXT), "eglMakeCurrent(unbind)");
    require_egl(eglDestroyContext(display, context), "eglDestroyContext");
    require_egl(eglDestroySurface(display, surface), "eglDestroySurface");
    require_egl(eglTerminate(display), "eglTerminate");
    puts("egl-gles2-smoke: PASS");
    return 0;
}
