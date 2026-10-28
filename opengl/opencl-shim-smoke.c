/*
 * opencl-shim-smoke: use OpenCL the way an application does.
 *
 * Links against libOpenCL.so.1 and calls the API directly.  With the staged
 * shim first in LD_LIBRARY_PATH that is the libhybris forwarding library, so
 * this proves that unmodified OpenCL applications reach the vendor driver.
 *
 * Build (armhf):
 *   arm-none-linux-gnueabihf-gcc --sysroot=$SYSROOT -O2 -Wall -Wextra \
 *       -o opencl-shim-smoke opencl-shim-smoke.c -lOpenCL
 */
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

#include <stdio.h>

static const char *source =
    "__kernel void scale(__global const float *in, __global float *out,\n"
    "                    const float factor)\n"
    "{\n"
    "    size_t i = get_global_id(0);\n"
    "    out[i] = in[i] * factor;\n"
    "}\n";

#define CHECK(expr)                                                          \
    do {                                                                     \
        cl_int _err = (expr);                                                \
        if (_err != CL_SUCCESS) {                                            \
            fprintf(stderr, "opencl-shim-smoke: %s failed: %d\n", #expr,     \
                    _err);                                                   \
            return 1;                                                        \
        }                                                                    \
    } while (0)

int main(void)
{
    cl_platform_id platform;
    cl_device_id device;
    cl_uint count = 0;
    char name[256];
    cl_int err;
    cl_context context;
    cl_command_queue queue;
    cl_program program;
    cl_kernel kernel;
    cl_mem in_buf, out_buf;
    const float input[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    float output[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const float factor = 2.5f;
    size_t global = 4;
    int i;

    CHECK(clGetPlatformIDs(1, &platform, &count));
    if (count == 0) {
        fprintf(stderr, "opencl-shim-smoke: no platform\n");
        return 1;
    }
    clGetPlatformInfo(platform, CL_PLATFORM_NAME, sizeof(name), name, NULL);
    printf("opencl-shim-smoke: platform   = %s\n", name);

    CHECK(clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, &count));
    clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof(name), name, NULL);
    printf("opencl-shim-smoke: device     = %s\n", name);

    context = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
    CHECK(err);
    queue = clCreateCommandQueue(context, device, 0, &err);
    CHECK(err);
    program = clCreateProgramWithSource(context, 1, &source, NULL, &err);
    CHECK(err);
    CHECK(clBuildProgram(program, 1, &device, "", NULL, NULL));
    kernel = clCreateKernel(program, "scale", &err);
    CHECK(err);

    in_buf = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                            sizeof(input), (void *)input, &err);
    CHECK(err);
    out_buf = clCreateBuffer(context, CL_MEM_WRITE_ONLY, sizeof(output), NULL, &err);
    CHECK(err);

    CHECK(clSetKernelArg(kernel, 0, sizeof(cl_mem), &in_buf));
    CHECK(clSetKernelArg(kernel, 1, sizeof(cl_mem), &out_buf));
    CHECK(clSetKernelArg(kernel, 2, sizeof(cl_float), &factor));
    CHECK(clEnqueueNDRangeKernel(queue, kernel, 1, NULL, &global, NULL, 0, NULL,
                                 NULL));
    CHECK(clEnqueueReadBuffer(queue, out_buf, CL_TRUE, 0, sizeof(output), output,
                              0, NULL, NULL));
    CHECK(clFinish(queue));

    printf("opencl-shim-smoke: result     = %.2f %.2f %.2f %.2f\n",
           output[0], output[1], output[2], output[3]);
    for (i = 0; i < 4; i++) {
        if (output[i] != input[i] * factor) {
            fprintf(stderr, "opencl-shim-smoke: wrong result at %d\n", i);
            return 1;
        }
    }

    clReleaseMemObject(in_buf);
    clReleaseMemObject(out_buf);
    clReleaseKernel(kernel);
    clReleaseProgram(program);
    clReleaseCommandQueue(queue);
    clReleaseContext(context);

    puts("opencl-shim-smoke: PASS");
    return 0;
}
