/*
 * EGL_EXT_platform_base / EGL_EXT_platform_x11 smoke test for the
 * vendor-GLES/libhybris path.
 *
 * The wrapper always implemented eglGetPlatformDisplay() and
 * eglCreatePlatformWindowSurface(), but it used to pass the vendor client
 * extension string through unchanged, so eglQueryString(EGL_NO_DISPLAY,
 * EGL_EXTENSIONS) never listed EGL_EXT_platform_base.  Toolkits that gate the
 * platform API on that string then fell back to eglGetDisplay().
 *
 * This probe checks the advertisement and then exercises the advertised path
 * end to end: platform display -> X11 window surface -> GLES2 clear -> swap.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define STEP(message) do { fprintf(stderr, "[platform-smoke] %s\n", message); } while (0)

static int x_error_count;

static int on_x_error(Display *display, XErrorEvent *event)
{
    char text[256];
    XGetErrorText(display, event->error_code, text, sizeof text);
    fprintf(stderr, "[platform-smoke] X error: %s (request %d.%d, serial %lu)\n",
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

/* Whole-token match so "EGL_EXT_platform_base" is not confused with a prefix. */
static int has_extension(const char *extensions, const char *extension)
{
    size_t len = strlen(extension);
    const char *p = extensions;

    while (p && (p = strstr(p, extension)) != NULL) {
        int starts_token = (p == extensions) || (p[-1] == ' ');
        int ends_token = (p[len] == '\0') || (p[len] == ' ');

        if (starts_token && ends_token)
            return 1;
        p += len;
    }
    return 0;
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

    const char *client_extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!client_extensions) {
        fprintf(stderr, "eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS) returned NULL\n");
        return 1;
    }
    printf("client extensions: %s\n", client_extensions);
    if (!has_extension(client_extensions, "EGL_EXT_platform_base")) {
        fprintf(stderr, "EGL_EXT_platform_base is not advertised\n");
        return 1;
    }
    if (!has_extension(client_extensions, "EGL_EXT_platform_x11")) {
        fprintf(stderr, "EGL_EXT_platform_x11 is not advertised\n");
        return 1;
    }
    STEP("client extensions advertise EGL_EXT_platform_base and EGL_EXT_platform_x11");

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display_ext =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress("eglGetPlatformDisplayEXT");
    PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC create_platform_window_surface_ext =
        (PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC) eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");
    PFNEGLGETPLATFORMDISPLAYPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYPROC) eglGetProcAddress("eglGetPlatformDisplay");
    PFNEGLCREATEPLATFORMWINDOWSURFACEPROC create_platform_window_surface =
        (PFNEGLCREATEPLATFORMWINDOWSURFACEPROC) eglGetProcAddress("eglCreatePlatformWindowSurface");

    if (!get_platform_display_ext || !create_platform_window_surface_ext) {
        fprintf(stderr, "EGL_EXT_platform_base entry points are not resolvable\n");
        return 1;
    }
    if (!get_platform_display || !create_platform_window_surface) {
        fprintf(stderr, "EGL 1.5 platform entry points are not resolvable\n");
        return 1;
    }
    STEP("platform entry points resolve through eglGetProcAddress");

    /* EGL_EXT_platform_base requires EXT and core to reach the same display. */
    EGLDisplay display = get_platform_display_ext(EGL_PLATFORM_X11_KHR,
                                                  (void *)xdisplay, NULL);
    if (display == EGL_NO_DISPLAY)
        fail_egl("eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_KHR)");
    STEP("eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_KHR) returned a display");

    EGLint major = 0;
    EGLint minor = 0;
    require_egl(eglInitialize(display, &major, &minor), "eglInitialize");
    STEP("eglInitialize succeeded");
    if (major != 1 || minor < 4) {
        fprintf(stderr, "Unexpected EGL version %d.%d\n", major, minor);
        return 1;
    }

    const char *display_extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!display_extensions) {
        fprintf(stderr, "eglQueryString(display, EGL_EXTENSIONS) returned NULL\n");
        return 1;
    }
    if (!has_extension(display_extensions, "EGL_KHR_surfaceless_context")) {
        fprintf(stderr, "EGL_KHR_surfaceless_context is not advertised\n");
        return 1;
    }
    STEP("display extensions advertise EGL_KHR_surfaceless_context");

    EGLDisplay core_display = get_platform_display(EGL_PLATFORM_X11_KHR,
                                                   (void *)xdisplay, NULL);
    if (core_display != display) {
        fprintf(stderr, "eglGetPlatformDisplay() returned a different display for the same native display\n");
        return 1;
    }
    STEP("eglGetPlatformDisplay() agrees with the EXT variant");

    require_egl(eglBindAPI(EGL_OPENGL_ES_API), "eglBindAPI");

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
    if (config_count < 1) {
        fprintf(stderr, "No EGL config supports an RGBA8 GLES2 X11 window\n");
        return 1;
    }

    EGLint visual_id = 0;
    require_egl(eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID,
                                   &visual_id), "eglGetConfigAttrib(visual)");
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
    const Colormap colormap = XCreateColormap(xdisplay, root,
                                              visual_info[0].visual, AllocNone);
    XSetWindowAttributes window_attrs = {0};
    window_attrs.colormap = colormap;
    window_attrs.event_mask = StructureNotifyMask | ExposureMask;
    /* The EGL visual is depth 32 while the root is depth 24, so the window
     * must not inherit the parent's border or background pixmaps. */
    window_attrs.background_pixmap = None;
    window_attrs.border_pixel = 0;
    const Window window = XCreateWindow(xdisplay, root, 24, 48, 320, 240, 0,
                                        visual_info[0].depth, InputOutput,
                                        visual_info[0].visual,
                                        CWBackPixmap | CWBorderPixel |
                                        CWColormap | CWEventMask,
                                        &window_attrs);
    XFree(visual_info);
    XStoreName(xdisplay, window, "GE8322 EGL platform smoke test");
    XMapWindow(xdisplay, window);
    XSync(xdisplay, False);
    if (x_error_count != 0) {
        fprintf(stderr, "X window setup produced %d error(s)\n", x_error_count);
        return 1;
    }
    STEP("X window created and mapped");

    const EGLSurface surface = create_platform_window_surface_ext(
        display, config, (void *)(uintptr_t)window, NULL);
    if (surface == EGL_NO_SURFACE)
        fail_egl("eglCreatePlatformWindowSurfaceEXT(X11)");
    STEP("eglCreatePlatformWindowSurfaceEXT succeeded");

    const EGLint context_attrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    const EGLContext context = eglCreateContext(display, config,
                                                EGL_NO_CONTEXT, context_attrs);
    if (context == EGL_NO_CONTEXT)
        fail_egl("eglCreateContext");
    require_egl(eglMakeCurrent(display, surface, surface, context),
                "eglMakeCurrent");
    STEP("windowed GLES2 context is current");

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

    glViewport(0, 0, 320, 240);
    glClearColor(0.25f, 0.50f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    GLubyte pixel[4] = {0, 0, 0, 0};
    glReadPixels(160, 120, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    const GLenum gl_error = glGetError();
    printf("center pixel: %u %u %u %u (glError=0x%04x)\n",
           pixel[0], pixel[1], pixel[2], pixel[3], gl_error);
    if (gl_error != GL_NO_ERROR || pixel[0] < 62 || pixel[0] > 66 ||
        pixel[1] < 126 || pixel[1] > 130 || pixel[2] < 190 || pixel[2] > 194 ||
        pixel[3] < 253) {
        fprintf(stderr, "GLES clear/readback did not match expected RGBA values\n");
        return 1;
    }
    STEP("GLES clear/readback through the platform surface is correct");

    if (eglSwapBuffers(display, surface) != EGL_TRUE) {
        const EGLint swap_error = eglGetError();
        if (swap_error != EGL_SUCCESS) {
            fprintf(stderr, "eglSwapBuffers failed: EGL error 0x%04x\n", swap_error);
            return 1;
        }
        STEP("eglSwapBuffers returned EGL_FALSE with EGL_SUCCESS (frame presented)");
    } else {
        STEP("eglSwapBuffers succeeded");
    }

    /* EGL_KHR_surfaceless_context: the platform display must also work
     * without any surface, which is what compositors and headless GL use. */
    require_egl(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context),
                "eglMakeCurrent(surfaceless)");
    const GLubyte *surfaceless_renderer = glGetString(GL_RENDERER);
    if (!surfaceless_renderer) {
        fprintf(stderr, "surfaceless context has no GL_RENDERER\n");
        return 1;
    }
    printf("surfaceless renderer: %s\n", surfaceless_renderer);
    STEP("surfaceless context works on the platform display");

    require_egl(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                               EGL_NO_CONTEXT), "eglMakeCurrent(unbind)");
    require_egl(eglDestroyContext(display, context), "eglDestroyContext");
    require_egl(eglDestroySurface(display, surface), "eglDestroySurface");
    require_egl(eglTerminate(display), "eglTerminate");
    XDestroyWindow(xdisplay, window);
    XFreeColormap(xdisplay, colormap);
    XCloseDisplay(xdisplay);
    puts("egl-platform-smoke: PASS");
    return 0;
}
