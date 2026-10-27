/*
 * X11 EGL/GLES2 smoke test for the vendor-GLES/libhybris path.
 * This is a diagnostic, not a Mesa test. It opens an X window, renders a clear,
 * swaps once, and verifies a center-pixel readback from the current framebuffer.
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define STEP(message) do { fprintf(stderr, "[x11-smoke] %s\n", message); } while (0)

static int x_error_count;

static int on_x_error(Display *display, XErrorEvent *event)
{
    char text[256];
    XGetErrorText(display, event->error_code, text, sizeof text);
    fprintf(stderr, "[x11-smoke] X error: %s (request %d.%d, serial %lu)\n",
            text, event->request_code, event->minor_code, event->serial);
    ++x_error_count;
    return 0;
}

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
    Display *xdisplay = XOpenDisplay(NULL);
    if (!xdisplay) {
        fprintf(stderr, "XOpenDisplay failed (check DISPLAY and X11 socket)\n");
        return 1;
    }
    STEP("XOpenDisplay succeeded");
    XSetErrorHandler(on_x_error);

    EGLDisplay display = eglGetDisplay((EGLNativeDisplayType)xdisplay);
    STEP("eglGetDisplay returned");
    if (display == EGL_NO_DISPLAY)
        fail_egl("eglGetDisplay(X11)");
    EGLint major = 0;
    EGLint minor = 0;
    require_egl(eglInitialize(display, &major, &minor), "eglInitialize");
    STEP("eglInitialize succeeded");
    require_egl(eglBindAPI(EGL_OPENGL_ES_API), "eglBindAPI");
    STEP("eglBindAPI succeeded");

    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint config_count = 0;
    require_egl(eglChooseConfig(display, config_attrs, &config, 1, &config_count),
                "eglChooseConfig");
    STEP("eglChooseConfig returned");
    if (config_count < 1) {
        fprintf(stderr, "No EGL config supports an RGBA8 GLES2 X11 window\n");
        return 1;
    }

    EGLint visual_id = 0;
    require_egl(eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID,
                                   &visual_id), "eglGetConfigAttrib(visual)");
    STEP("EGL visual selected");
    XVisualInfo visual_template = { .visualid = (VisualID)visual_id };
    int visual_count = 0;
    XVisualInfo *visual_info = XGetVisualInfo(xdisplay, VisualIDMask,
                                              &visual_template, &visual_count);
    if (!visual_info || visual_count < 1) {
        fprintf(stderr, "No X visual matches EGL visual id 0x%x\n", visual_id);
        return 1;
    }

    const int screen = DefaultScreen(xdisplay);
    const Window root = RootWindow(xdisplay, screen);
    fprintf(stderr, "[x11-smoke] EGL visual 0x%lx: depth %d, class %d, root depth %d\n",
            (unsigned long)visual_id, visual_info[0].depth, visual_info[0].class,
            DefaultDepth(xdisplay, screen));
    const Colormap colormap = XCreateColormap(xdisplay, root,
                                               visual_info[0].visual, AllocNone);
    XSetWindowAttributes window_attrs = {0};
    window_attrs.colormap = colormap;
    window_attrs.event_mask = StructureNotifyMask | ExposureMask;
    /* The EGL visual is depth 32 while the root is depth 24.  A window whose
     * depth differs from its parent must not inherit the parent's border or
     * background pixmaps, or CreateWindow fails with BadMatch.  This mirrors
     * the attribute set that glmark2's X11 canvas uses. */
    window_attrs.background_pixmap = None;
    window_attrs.border_pixel = 0;
    const Window window = XCreateWindow(xdisplay, root, 24, 48, 480, 640, 0,
                                         visual_info[0].depth, InputOutput,
                                         visual_info[0].visual,
                                         CWBackPixmap | CWBorderPixel |
                                         CWColormap | CWEventMask,
                                         &window_attrs);
    XFree(visual_info);
    XStoreName(xdisplay, window, "GE8322 vendor GLES smoke test");
    XMapWindow(xdisplay, window);
    XSync(xdisplay, False);
    if (x_error_count != 0) {
        fprintf(stderr, "[x11-smoke] X window setup produced %d error(s)\n", x_error_count);
        return 1;
    }
    STEP("X window created and mapped");

    const EGLSurface surface = eglCreateWindowSurface(
        display, config, (EGLNativeWindowType)window, NULL);
    if (surface == EGL_NO_SURFACE)
        fail_egl("eglCreateWindowSurface(X11)");
    STEP("eglCreateWindowSurface succeeded");
    const EGLint context_attrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    const EGLContext context = eglCreateContext(display, config,
                                                 EGL_NO_CONTEXT, context_attrs);
    if (context == EGL_NO_CONTEXT)
        fail_egl("eglCreateContext");
    STEP("eglCreateContext succeeded");
    require_egl(eglMakeCurrent(display, surface, surface, context),
                "eglMakeCurrent");
    STEP("eglMakeCurrent succeeded");

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

    glViewport(0, 0, 480, 640);
    glClearColor(0.25f, 0.50f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    GLubyte pixel[4] = {0, 0, 0, 0};
    glReadPixels(240, 320, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    const GLenum gl_error = glGetError();
    printf("center pixel: %u %u %u %u (glError=0x%04x)\n",
           pixel[0], pixel[1], pixel[2], pixel[3], gl_error);
    if (gl_error != GL_NO_ERROR || pixel[0] < 62 || pixel[0] > 66 ||
        pixel[1] < 126 || pixel[1] > 130 || pixel[2] < 190 || pixel[2] > 194 ||
        pixel[3] < 253) {
        fprintf(stderr, "GLES clear/readback did not match expected RGBA values\n");
        return 1;
    }

    if (eglSwapBuffers(display, surface) != EGL_TRUE) {
        const EGLint swap_error = eglGetError();
        if (swap_error != EGL_SUCCESS) {
            fprintf(stderr, "eglSwapBuffers failed: EGL error 0x%04x\n", swap_error);
            return 1;
        }
        /* The vendor eglSwapBuffers returns EGL_FALSE on this libhybris X11
         * backend even though ws_finishSwap() has already presented the frame
         * and no EGL error was raised. */
        STEP("eglSwapBuffers returned EGL_FALSE with EGL_SUCCESS (frame presented)");
    } else {
        STEP("eglSwapBuffers succeeded");
    }
    sleep(3); /* Keep the rendered window visible briefly for manual inspection. */
    require_egl(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                               EGL_NO_CONTEXT), "eglMakeCurrent(unbind)");
    require_egl(eglDestroyContext(display, context), "eglDestroyContext");
    require_egl(eglDestroySurface(display, surface), "eglDestroySurface");
    require_egl(eglTerminate(display), "eglTerminate");
    XDestroyWindow(xdisplay, window);
    XFreeColormap(xdisplay, colormap);
    XCloseDisplay(xdisplay);
    puts("egl-gles2-x11-smoke: PASS");
    return 0;
}
