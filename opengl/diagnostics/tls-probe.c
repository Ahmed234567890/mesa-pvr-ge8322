/*
 * tls-probe: does the vendor EGL/GLES driver clobber glibc's thread control
 * block?
 *
 * libhybris runs Android libraries on top of glibc.  Android's libc keeps its
 * thread-local slots at fixed offsets from the thread pointer; glibc keeps a
 * different structure there (tcb, dtv, self, stack guard, pointer guard).
 * A vendor driver that writes bionic TLS slots therefore corrupts glibc state.
 *
 * This probe snapshots the first 16 words at the thread pointer, performs a
 * representative EGL/GLES session including eglReleaseThread(), then reports
 * every word that changed.
 *
 * Build (armhf), from a libhybris build tree:
 *   arm-none-linux-gnueabihf-gcc --sysroot=$SYSROOT -O2 -Wall -Wextra \
 *       -I$LIBHYBRIS_BUILD/hybris/include \
 *       -o tls-probe tls-probe.c \
 *       -L$LIBHYBRIS_BUILD/hybris/egl/.libs -L$LIBHYBRIS_BUILD/hybris/glesv2/.libs \
 *       -lEGL -lGLESv2
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NWORDS 16

static uintptr_t initial[NWORDS];
static int have_initial;

static void snapshot(const char *tag)
{
    uintptr_t *tp = (uintptr_t *)__builtin_thread_pointer();
    int i;
    int changed = 0;

    if (!have_initial) {
        memcpy(initial, tp, sizeof(initial));
        have_initial = 1;
        printf("tp=%p tcb=%p dtv=%p\n", (void *)tp, (void *)tp[0],
               (void *)tp[1]);
        for (i = 0; i < NWORDS; i++)
            printf("  tcb[%2d] @%p = 0x%08lx\n", i, (void *)&tp[i],
                   (unsigned long)tp[i]);
        return;
    }

    for (i = 0; i < NWORDS; i++) {
        if (tp[i] != initial[i]) {
            printf("CHANGED [%s] tcb[%d] @%p: 0x%08lx -> 0x%08lx\n", tag, i,
                   (void *)&tp[i], (unsigned long)initial[i],
                   (unsigned long)tp[i]);
            changed = 1;
        }
    }
    if (!changed)
        printf("unchanged [%s]\n", tag);
}

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}

int main(void)
{
    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    const EGLint pbuffer_attrs[] = { EGL_WIDTH, 128, EGL_HEIGHT, 128, EGL_NONE };
    const EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    static const char *vs_src =
        "attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }";
    static const char *fs_src =
        "precision mediump float; void main() { gl_FragColor = vec4(1.0); }";
    EGLDisplay dpy;
    EGLConfig config;
    EGLint count = 0, major = 0, minor = 0;
    EGLSurface surface;
    EGLContext context;
    GLuint tex, fbo, rbo, prog, vbo;
    GLubyte pixel[4];

    snapshot("start");

    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
        fprintf(stderr, "eglInitialize failed\n");
        return 1;
    }
    snapshot("eglInitialize");

    eglBindAPI(EGL_OPENGL_ES_API);
    if (!eglChooseConfig(dpy, config_attrs, &config, 1, &count) || count < 1) {
        fprintf(stderr, "no config\n");
        return 1;
    }
    surface = eglCreatePbufferSurface(dpy, config, pbuffer_attrs);
    context = eglCreateContext(dpy, config, EGL_NO_CONTEXT, context_attrs);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(dpy, surface, surface, context)) {
        fprintf(stderr, "surface/context setup failed\n");
        return 1;
    }
    snapshot("makeCurrent");

    prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, vs_src));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fs_src));
    glLinkProgram(prog);
    glUseProgram(prog);

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, 24, NULL, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glEnableVertexAttribArray(0);

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 128, 128, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA4, 128, 128);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);

    glViewport(0, 0, 128, 128);
    glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    glReadPixels(64, 64, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    snapshot("render");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &rbo);
    glDeleteTextures(1, &tex);
    glDeleteBuffers(1, &vbo);
    glDeleteProgram(prog);
    snapshot("glDelete");

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, context);
    snapshot("destroyContext");
    eglDestroySurface(dpy, surface);
    snapshot("destroySurface");
    eglTerminate(dpy);
    snapshot("terminate");
    eglReleaseThread();
    snapshot("releaseThread");

    puts("tls-probe: done");
    return 0;
}
