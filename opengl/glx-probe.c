/*
 * glx-probe: minimal desktop-OpenGL (GLX) test for the GL4ES + libhybris path.
 *
 * Unlike glxinfo, this links only against libGL and does not go through any
 * distribution wrapper, so it reports exactly what the GLX library in
 * LD_LIBRARY_PATH provides.
 *
 * It prints the client GLX vendor/version, creates a context on a mapped X11
 * window, prints GL_VENDOR/GL_RENDERER/GL_VERSION, clears to a known colour,
 * reads the pixel back and swaps.
 *
 * Build (armhf, against GL4ES headers):
 *   arm-none-linux-gnueabihf-gcc --sysroot=$SYSROOT -O2 -Wall \
 *       -I$GL4ES/include -o glx-probe glx-probe.c -lX11 -lGL
 */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int attribute_list[] = {
    GLX_RGBA,
    GLX_RED_SIZE, 8,
    GLX_GREEN_SIZE, 8,
    GLX_BLUE_SIZE, 8,
    GLX_DEPTH_SIZE, 24,
    GLX_DOUBLEBUFFER,
    None
};

int main(void)
{
    Display *dpy;
    XVisualInfo *vi;
    Window win;
    XSetWindowAttributes swa;
    GLXContext ctx;
    GLubyte pixel[4];
    int screen;

    dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        fprintf(stderr, "glx-probe: XOpenDisplay failed (DISPLAY=%s)\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return 1;
    }

    printf("glx-probe: client vendor  = %s\n", glXGetClientString(dpy, GLX_VENDOR));
    printf("glx-probe: client version = %s\n", glXGetClientString(dpy, GLX_VERSION));

    if (!glXQueryExtension(dpy, NULL, NULL)) {
        fprintf(stderr, "glx-probe: GLX extension missing\n");
        return 1;
    }

    screen = DefaultScreen(dpy);
    vi = glXChooseVisual(dpy, screen, attribute_list);
    if (vi == NULL) {
        fprintf(stderr, "glx-probe: glXChooseVisual found no RGBA visual\n");
        return 1;
    }
    printf("glx-probe: visual depth  = %d\n", vi->depth);

    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen),
                                   vi->visual, AllocNone);
    swa.background_pixmap = None;
    swa.event_mask = ExposureMask | KeyPressMask;

    win = XCreateWindow(dpy, RootWindow(dpy, vi->screen),
                        0, 0, 320, 240, 0, vi->depth, InputOutput,
                        vi->visual, CWColormap | CWBackPixmap | CWEventMask, &swa);
    XMapWindow(dpy, win);
    XStoreName(dpy, win, "glx-probe");
    XFlush(dpy);

    ctx = glXCreateContext(dpy, vi, NULL, True);
    if (ctx == NULL) {
        fprintf(stderr, "glx-probe: glXCreateContext failed\n");
        return 1;
    }
    if (!glXMakeCurrent(dpy, win, ctx)) {
        fprintf(stderr, "glx-probe: glXMakeCurrent failed\n");
        return 1;
    }

    printf("glx-probe: GL_VENDOR    = %s\n", (const char *)glGetString(GL_VENDOR));
    printf("glx-probe: GL_RENDERER  = %s\n", (const char *)glGetString(GL_RENDERER));
    printf("glx-probe: GL_VERSION   = %s\n", (const char *)glGetString(GL_VERSION));

    glViewport(0, 0, 320, 240);
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glFinish();

    glReadPixels(160, 120, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    printf("glx-probe: center pixel  = %u %u %u %u\n",
           pixel[0], pixel[1], pixel[2], pixel[3]);

    glXSwapBuffers(dpy, win);

    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, ctx);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);

    if (pixel[0] != 64 || pixel[1] != 128 || pixel[2] != 191) {
        fprintf(stderr, "glx-probe: unexpected readback\n");
        return 1;
    }

    puts("glx-probe: PASS");
    return 0;
}
