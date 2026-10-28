/*
 * opencl-probe: load the vendor PowerVR OpenCL library from a glibc process
 * through libhybris and run a real kernel.
 *
 * The vendor driver is an Android/bionic library, so it cannot be linked the
 * normal way. hybris_dlopen() maps it with the bionic linker and resolves the
 * symbols for us. Everything below is plain OpenCL 1.2 API.
 *
 * Prints the platform and device strings, builds a kernel from source,
 * executes it, and verifies the result.
 *
 * Build (armhf):
 *   arm-none-linux-gnueabihf-gcc --sysroot=$SYSROOT -O2 -Wall -Wextra \
 *       -I$LIBHYBRIS_BUILD/hybris/include -I/usr/include \
 *       -o opencl-probe opencl-probe.c \
 *       -L$LIBHYBRIS_BUILD/hybris/common/.libs -lhybris-common -ldl
 */
#include <hybris/common/dlfcn.h>

#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

#include <dlfcn.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr)                                                          \
    do {                                                                     \
        cl_int _err = (expr);                                                \
        if (_err != CL_SUCCESS) {                                            \
            fprintf(stderr, "opencl-probe: %s failed: %d\n", #expr, _err);   \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static const char *kernel_source =
    "__kernel void add_arrays(__global const float *a,\n"
    "                         __global const float *b,\n"
    "                         __global float *c)\n"
    "{\n"
    "    size_t i = get_global_id(0);\n"
    "    c[i] = a[i] + b[i];\n"
    "}\n";

struct cl_api {
    cl_int (*clGetPlatformIDs)(cl_uint, cl_platform_id *, cl_uint *);
    cl_int (*clGetPlatformInfo)(cl_platform_id, cl_platform_info, size_t,
                                void *, size_t *);
    cl_int (*clGetDeviceIDs)(cl_platform_id, cl_device_type, cl_uint,
                             cl_device_id *, cl_uint *);
    cl_int (*clGetDeviceInfo)(cl_device_id, cl_device_info, size_t, void *,
                              size_t *);
    cl_context (*clCreateContext)(const cl_context_properties *, cl_uint,
                                  const cl_device_id *, void *, void *,
                                  cl_int *);
    cl_command_queue (*clCreateCommandQueue)(cl_context, cl_device_id,
                                             cl_command_queue_properties,
                                             cl_int *);
    cl_program (*clCreateProgramWithSource)(cl_context, cl_uint,
                                            const char **, const size_t *,
                                            cl_int *);
    cl_int (*clBuildProgram)(cl_program, cl_uint, const cl_device_id *,
                             const char *, void *, void *);
    cl_kernel (*clCreateKernel)(cl_program, const char *, cl_int *);
    cl_mem (*clCreateBuffer)(cl_context, cl_mem_flags, size_t, void *,
                             cl_int *);
    cl_int (*clSetKernelArg)(cl_kernel, cl_uint, size_t, const void *);
    cl_int (*clEnqueueNDRangeKernel)(cl_command_queue, cl_kernel, cl_uint,
                                     const size_t *, const size_t *,
                                     const size_t *, cl_uint,
                                     const cl_event *, cl_event *);
    cl_int (*clEnqueueReadBuffer)(cl_command_queue, cl_mem, cl_bool, size_t,
                                  size_t, void *, cl_uint, const cl_event *,
                                  cl_event *);
    cl_int (*clFinish)(cl_command_queue);
    cl_int (*clReleaseMemObject)(cl_mem);
    cl_int (*clReleaseKernel)(cl_kernel);
    cl_int (*clReleaseProgram)(cl_program);
    cl_int (*clReleaseCommandQueue)(cl_command_queue);
    cl_int (*clReleaseContext)(cl_context);
};

static int load_api(void *handle, struct cl_api *api)
{
    struct {
        const char *name;
        void **slot;
    } symbols[] = {
        { "clGetPlatformIDs", (void **)&api->clGetPlatformIDs },
        { "clGetPlatformInfo", (void **)&api->clGetPlatformInfo },
        { "clGetDeviceIDs", (void **)&api->clGetDeviceIDs },
        { "clGetDeviceInfo", (void **)&api->clGetDeviceInfo },
        { "clCreateContext", (void **)&api->clCreateContext },
        { "clCreateCommandQueue", (void **)&api->clCreateCommandQueue },
        { "clCreateProgramWithSource", (void **)&api->clCreateProgramWithSource },
        { "clBuildProgram", (void **)&api->clBuildProgram },
        { "clCreateKernel", (void **)&api->clCreateKernel },
        { "clCreateBuffer", (void **)&api->clCreateBuffer },
        { "clSetKernelArg", (void **)&api->clSetKernelArg },
        { "clEnqueueNDRangeKernel", (void **)&api->clEnqueueNDRangeKernel },
        { "clEnqueueReadBuffer", (void **)&api->clEnqueueReadBuffer },
        { "clFinish", (void **)&api->clFinish },
        { "clReleaseMemObject", (void **)&api->clReleaseMemObject },
        { "clReleaseKernel", (void **)&api->clReleaseKernel },
        { "clReleaseProgram", (void **)&api->clReleaseProgram },
        { "clReleaseCommandQueue", (void **)&api->clReleaseCommandQueue },
        { "clReleaseContext", (void **)&api->clReleaseContext },
    };
    size_t i;

    for (i = 0; i < sizeof(symbols) / sizeof(symbols[0]); i++) {
        *symbols[i].slot = hybris_dlsym(handle, symbols[i].name);
        if (*symbols[i].slot == NULL) {
            fprintf(stderr, "opencl-probe: missing symbol %s\n", symbols[i].name);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *library = argc > 1 ? argv[1] : "/vendor/lib/libPVROCL.so";
    struct cl_api api;
    void *handle;
    cl_platform_id platform = NULL;
    cl_device_id device = NULL;
    cl_uint count = 0;
    char text[512];
    cl_int err;
    cl_context context;
    cl_command_queue queue;
    cl_program program;
    cl_kernel kernel;
    cl_mem buf_a, buf_b, buf_c;
    const float a[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    const float b[4] = { 10.0f, 20.0f, 30.0f, 40.0f };
    float c[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    size_t global = 4;
    int i;

    handle = hybris_dlopen(library, RTLD_NOW);
    if (handle == NULL) {
        fprintf(stderr, "opencl-probe: hybris_dlopen(%s): %s\n", library,
                hybris_dlerror());
        return 1;
    }
    if (load_api(handle, &api) != 0)
        return 1;

    if (api.clGetPlatformIDs(1, &platform, &count) != CL_SUCCESS || count == 0) {
        fprintf(stderr, "opencl-probe: no OpenCL platform\n");
        return 1;
    }

    api.clGetPlatformInfo(platform, CL_PLATFORM_NAME, sizeof(text), text, NULL);
    printf("opencl-probe: platform name    = %s\n", text);
    api.clGetPlatformInfo(platform, CL_PLATFORM_VENDOR, sizeof(text), text, NULL);
    printf("opencl-probe: platform vendor  = %s\n", text);
    api.clGetPlatformInfo(platform, CL_PLATFORM_VERSION, sizeof(text), text, NULL);
    printf("opencl-probe: platform version = %s\n", text);

    if (api.clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, &count)
            != CL_SUCCESS || count == 0) {
        fprintf(stderr, "opencl-probe: no OpenCL device\n");
        return 1;
    }
    api.clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof(text), text, NULL);
    printf("opencl-probe: device name      = %s\n", text);
    api.clGetDeviceInfo(device, CL_DEVICE_VERSION, sizeof(text), text, NULL);
    printf("opencl-probe: device version   = %s\n", text);
    api.clGetDeviceInfo(device, CL_DRIVER_VERSION, sizeof(text), text, NULL);
    printf("opencl-probe: driver version   = %s\n", text);

    {
        cl_uint units = 0;
        cl_ulong global_mem = 0;
        size_t size = 0;

        api.clGetDeviceInfo(device, CL_DEVICE_MAX_COMPUTE_UNITS,
                            sizeof(units), &units, NULL);
        api.clGetDeviceInfo(device, CL_DEVICE_GLOBAL_MEM_SIZE,
                            sizeof(global_mem), &global_mem, NULL);
        printf("opencl-probe: compute units    = %u\n", units);
        printf("opencl-probe: global memory    = %llu MiB\n",
               (unsigned long long)(global_mem / (1024 * 1024)));

        size = 0;
        if (api.clGetDeviceInfo(device, CL_DEVICE_OPENCL_C_VERSION, 0, NULL,
                                &size) == CL_SUCCESS && size > 0 && size < sizeof(text)) {
            api.clGetDeviceInfo(device, CL_DEVICE_OPENCL_C_VERSION, size, text, NULL);
            printf("opencl-probe: OpenCL C        = %s\n", text);
        }

        size = 0;
        if (api.clGetDeviceInfo(device, CL_DEVICE_EXTENSIONS, 0, NULL, &size)
                == CL_SUCCESS && size > 0) {
            char *exts = malloc(size);
            if (exts != NULL
                    && api.clGetDeviceInfo(device, CL_DEVICE_EXTENSIONS, size,
                                           exts, NULL) == CL_SUCCESS) {
                printf("opencl-probe: extensions      = %s\n", exts);
            }
            free(exts);
        }
    }

    context = api.clCreateContext(NULL, 1, &device, NULL, NULL, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "opencl-probe: clCreateContext failed: %d\n", err);
        return 1;
    }
    queue = api.clCreateCommandQueue(context, device, 0, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "opencl-probe: clCreateCommandQueue failed: %d\n", err);
        return 1;
    }

    program = api.clCreateProgramWithSource(context, 1, &kernel_source, NULL, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "opencl-probe: clCreateProgramWithSource failed: %d\n", err);
        return 1;
    }
    err = api.clBuildProgram(program, 1, &device, "", NULL, NULL);
    if (err != CL_SUCCESS) {
        char log[4096] = { 0 };
        size_t log_size = 0;
        /* clGetProgramBuildInfo is optional here; keep the error code primary. */
        (void)log;
        (void)log_size;
        fprintf(stderr, "opencl-probe: clBuildProgram failed: %d\n", err);
        return 1;
    }

    kernel = api.clCreateKernel(program, "add_arrays", &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "opencl-probe: clCreateKernel failed: %d\n", err);
        return 1;
    }

    buf_a = api.clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                               sizeof(a), (void *)a, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "opencl-probe: clCreateBuffer(a) failed: %d\n", err);
        return 1;
    }
    buf_b = api.clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                               sizeof(b), (void *)b, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "opencl-probe: clCreateBuffer(b) failed: %d\n", err);
        return 1;
    }
    buf_c = api.clCreateBuffer(context, CL_MEM_WRITE_ONLY, sizeof(c), NULL, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "opencl-probe: clCreateBuffer(c) failed: %d\n", err);
        return 1;
    }

    CHECK(api.clSetKernelArg(kernel, 0, sizeof(cl_mem), &buf_a));
    CHECK(api.clSetKernelArg(kernel, 1, sizeof(cl_mem), &buf_b));
    CHECK(api.clSetKernelArg(kernel, 2, sizeof(cl_mem), &buf_c));
    CHECK(api.clEnqueueNDRangeKernel(queue, kernel, 1, NULL, &global, NULL, 0,
                                     NULL, NULL));
    CHECK(api.clEnqueueReadBuffer(queue, buf_c, CL_TRUE, 0, sizeof(c), c, 0,
                                  NULL, NULL));
    CHECK(api.clFinish(queue));

    printf("opencl-probe: result          = %.0f %.0f %.0f %.0f\n",
           c[0], c[1], c[2], c[3]);

    for (i = 0; i < 4; i++) {
        if (c[i] != a[i] + b[i]) {
            fprintf(stderr, "opencl-probe: wrong result at %d\n", i);
            return 1;
        }
    }

    api.clReleaseMemObject(buf_a);
    api.clReleaseMemObject(buf_b);
    api.clReleaseMemObject(buf_c);
    api.clReleaseKernel(kernel);
    api.clReleaseProgram(program);
    api.clReleaseCommandQueue(queue);
    api.clReleaseContext(context);
    hybris_dlclose(handle);

    puts("opencl-probe: PASS");
    return 0;
}
