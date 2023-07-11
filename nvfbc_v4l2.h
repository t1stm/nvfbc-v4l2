#ifndef NVFBC_V4L2
#define NVFBC_V4L2

#include <stdint.h>
#include <pthread.h>
#include <string.h>
#include <assert.h>
#include <malloc.h>
#include <math.h>
#include <dlfcn.h>
#include <stdlib.h>

#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glext.h>
#include <GL/glxext.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>

#include "NvFBC.h"
#include "defines.h"

typedef struct {
    int32_t screen;
    bool show_cursor;
    int32_t fps;
    bool push_model;
    bool direct_capture;
} Capture_Settings;

typedef struct {
    Display *X_display;

    uint32_t fr_width;
    uint32_t fr_height;

    uint32_t width;
    uint32_t height;
    uint32_t offset_x;
    uint32_t offset_y;

    uint64_t display_id;
} NvFBC_InitData;

typedef struct {
    NVFBC_SESSION_HANDLE fbc_handle;
    NVFBC_CREATE_HANDLE_PARAMS create_handle_params;
    NVFBC_GET_STATUS_PARAMS status_params;
    NVFBC_CREATE_CAPTURE_SESSION_PARAMS create_capture_params;
} NvFBC_SessionData;

static void *nvfbc_lib = NULL;
static PNVFBCCREATEINSTANCE NvFBCCreateInstance_ptr = NULL;
static NVFBC_API_FUNCTION_LIST function_list;

static enum _NVFBC_BUFFER_FORMAT get_nvfbc_pixel_format(enum Pixel_Format pixel_fmt) {
    switch (pixel_fmt) {
        case NV_12:
        case YUV_420:
            return NVFBC_BUFFER_FORMAT_NV12;
        case RGB_24:
            return NVFBC_BUFFER_FORMAT_RGB;
        case RGBA_444:
            return NVFBC_BUFFER_FORMAT_BGRA;

        default:
            exit(EXIT_FAILURE);
    }
}

NvFBC_InitData load_libraries();

NvFBC_SessionData
create_session(NvFBC_InitData init_data, Capture_Settings capture_settings, void **frame_ptr, enum Pixel_Format pixel_fmt);

void capture_frame(NvFBC_SessionData *session_data);

void destroy_session(NvFBC_SessionData session_data);

#endif