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

#include "NvFBC.h"
#include "defines.h"

typedef struct {
    int32_t screen;
    bool show_cursor;
    int32_t fps;
    bool push_model;
    bool direct_capture;
} CaptureSettings;

typedef struct {
    PNVFBCCREATEINSTANCE NvFBCCreateInstance_ptr;
    Display* X_display;

    uint32_t width;
    uint32_t height;
    uint32_t offset_x;
    uint32_t offset_y;
} NvFBC_InitData;

typedef struct {
    NVFBC_SESSION_HANDLE fbcHandle;
    NVFBC_CREATE_HANDLE_PARAMS createHandleParams;
    NVFBC_GET_STATUS_PARAMS statusParams;
    NVFBC_CREATE_CAPTURE_SESSION_PARAMS createCaptureParams;
} NvFBC_SessionData;

static void *nvfbc_lib = NULL;
static PNVFBCCREATEINSTANCE NvFBCCreateInstance_ptr = NULL;
static NVFBC_API_FUNCTION_LIST function_list;

NvFBC_InitData load_library();
NvFBC_SessionData create_session(NvFBC_InitData init_data, CaptureSettings capture_settings, void **frame_ptr);
void capture_frame(NvFBC_SessionData *session_data);
void destroy_session(NvFBC_SessionData session_data);

#endif