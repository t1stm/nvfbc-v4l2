#include "nvfbc_v4l2.h"

NvFBC_InitData load_library() {
    Display *dpy = NULL;

    nvfbc_lib = dlopen(NVFBC_LIB_NAME, RTLD_NOW);
    if (nvfbc_lib == NULL) {
        fprintf(stderr, "Unable to open NvFBC library '%s'\n", NVFBC_LIB_NAME);
        exit(EXIT_FAILURE);
    }

    NvFBCCreateInstance_ptr = (PNVFBCCREATEINSTANCE) dlsym(nvfbc_lib, "NvFBCCreateInstance");
    if (NvFBCCreateInstance_ptr == NULL) {
        fprintf(stderr, "Unable to resolve symbol 'NvFBCCreateInstance'\n");
        exit(EXIT_FAILURE);
    }

    memset(&function_list, 0, sizeof(function_list));
    function_list.dwVersion = NVFBC_VERSION;

    NVFBCSTATUS nvfbc_status = NvFBCCreateInstance_ptr(&function_list);
    if (nvfbc_status != NVFBC_SUCCESS) {
        fprintf(stderr, "Unable to create NvFBC instance (status: %d)\n",
                nvfbc_status);
        exit(EXIT_FAILURE);
    }

    dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        fprintf(stderr, "Unable to open X11 display. \n");
        exit(EXIT_FAILURE);
    }

    uint32_t framebufferWidth  = DisplayWidth(dpy, XDefaultScreen(dpy));
    uint32_t framebufferHeight = DisplayHeight(dpy, XDefaultScreen(dpy));

    NvFBC_InitData result = {
            .NvFBCCreateInstance_ptr = NvFBCCreateInstance_ptr,
            .width = framebufferWidth,
            .height = framebufferHeight,
            .X_display = dpy
    };
    return result;
}

NvFBC_SessionData create_session(NvFBC_InitData init_data, void **frame_ptr) {
    NVFBCSTATUS nvfbc_status;

    NVFBC_SESSION_HANDLE fbcHandle;
    NVFBC_CREATE_HANDLE_PARAMS createHandleParams;
    NVFBC_GET_STATUS_PARAMS statusParams;
    NVFBC_CREATE_CAPTURE_SESSION_PARAMS createCaptureParams;
    NVFBC_TOSYS_SETUP_PARAMS setupParams;

    NVFBC_BOX capture_box = {
            .x = init_data.offset_x,
            .y = init_data.offset_y,

            .w = init_data.width,
            .h = init_data.height
    };

    NVFBC_SIZE capture_size = {
            .w = init_data.width,
            .h = init_data.height
    };

    // Create new session handle.
    memset(&createHandleParams, 0, sizeof(createHandleParams));
    createHandleParams.dwVersion = NVFBC_CREATE_HANDLE_PARAMS_VER;

    nvfbc_status = function_list.nvFBCCreateHandle(&fbcHandle, &createHandleParams);
    if (nvfbc_status != NVFBC_SUCCESS) {
        fprintf(stderr, "%s\n", function_list.nvFBCGetLastErrorStr(fbcHandle));
        exit(EXIT_FAILURE);
    }

    // Get display driver state.
    memset(&statusParams, 0, sizeof(statusParams));
    statusParams.dwVersion = NVFBC_GET_STATUS_PARAMS_VER;

    nvfbc_status = function_list.nvFBCGetStatus(fbcHandle, &statusParams);
    if (nvfbc_status != NVFBC_SUCCESS) {
        fprintf(stderr, "%s\n", function_list.nvFBCGetLastErrorStr(fbcHandle));
        exit(EXIT_FAILURE);
    }

    if (statusParams.bCanCreateNow == NVFBC_FALSE) {
        fprintf(stderr, "It is not possible to create a capture session on this system.\n");
        exit(EXIT_FAILURE);
    }

    // Create a capture session that captures to system memory.
    memset(&createCaptureParams, 0, sizeof(createCaptureParams));

    createCaptureParams.dwVersion     = NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER;
    createCaptureParams.eCaptureType  = NVFBC_CAPTURE_TO_SYS;
    createCaptureParams.bWithCursor   = NVFBC_TRUE;
    createCaptureParams.captureBox    = capture_box;
    createCaptureParams.frameSize     = capture_size;
    createCaptureParams.eTrackingType = NVFBC_TRACKING_SCREEN;

    nvfbc_status = function_list.nvFBCCreateCaptureSession(fbcHandle, &createCaptureParams);
    if (nvfbc_status != NVFBC_SUCCESS) {
        fprintf(stderr, "%s\n", function_list.nvFBCGetLastErrorStr(fbcHandle));
        exit(EXIT_FAILURE);
    }

    memset(&setupParams, 0, sizeof(setupParams));

    setupParams.dwVersion     = NVFBC_TOSYS_SETUP_PARAMS_VER;
    setupParams.eBufferFormat = NVFBC_BUFFER_FORMAT_BGRA;
    setupParams.ppBuffer      = frame_ptr;
    setupParams.bWithDiffMap  = NVFBC_FALSE;

    nvfbc_status = function_list.nvFBCToSysSetUp(fbcHandle, &setupParams);
    if (nvfbc_status != NVFBC_SUCCESS) {
        fprintf(stderr, "%s\n", function_list.nvFBCGetLastErrorStr(fbcHandle));
        exit(EXIT_FAILURE);
    }

    NvFBC_SessionData result = {
            .createCaptureParams = createCaptureParams,
            .createHandleParams = createHandleParams,
            .fbcHandle = fbcHandle,
            .statusParams = statusParams
    };
    return result;
}

void destroy_session(NvFBC_SessionData session_data) {
    NVFBCSTATUS nvfbc_status;
    NVFBC_DESTROY_CAPTURE_SESSION_PARAMS destroyCaptureParams;
    NVFBC_DESTROY_HANDLE_PARAMS destroyHandleParams;

    // Destroy capture session.
    memset(&destroyCaptureParams, 0, sizeof(destroyCaptureParams));
    destroyCaptureParams.dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER;

    nvfbc_status = function_list.nvFBCDestroyCaptureSession(session_data.fbcHandle, &destroyCaptureParams);
    if (nvfbc_status != NVFBC_SUCCESS) {
        fprintf(stderr, "%s\n", function_list.nvFBCGetLastErrorStr(session_data.fbcHandle));
        exit(EXIT_FAILURE);
    }

    // Destroy session handle.
    memset(&destroyHandleParams, 0, sizeof(destroyHandleParams));
    destroyHandleParams.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER;

    nvfbc_status = function_list.nvFBCDestroyHandle(session_data.fbcHandle, &destroyHandleParams);
    if (nvfbc_status != NVFBC_SUCCESS) {
        fprintf(stderr, "%s\n", function_list.nvFBCGetLastErrorStr(session_data.fbcHandle));
        exit(EXIT_FAILURE);
    }
}

void capture_frame(NvFBC_SessionData session_data) {
    NVFBCSTATUS nvfbc_status;
    NVFBC_TOSYS_GRAB_FRAME_PARAMS grabParams;
    NVFBC_FRAME_GRAB_INFO frameInfo;

    memset(&grabParams, 0, sizeof(grabParams));
    memset(&frameInfo, 0, sizeof(frameInfo));

    grabParams.dwVersion = NVFBC_TOSYS_GRAB_FRAME_PARAMS_VER;

    // Block until screen or mouse update.
    grabParams.dwFlags = NVFBC_TOSYS_GRAB_FLAGS_NOFLAGS;

    // Frame info.
    grabParams.pFrameGrabInfo = &frameInfo;

    // Captures a frame.
    nvfbc_status = function_list.nvFBCToSysGrabFrame(session_data.fbcHandle, &grabParams);
    if (nvfbc_status != NVFBC_SUCCESS) {
        fprintf(stderr, "Capturing frame failed: '%s'\n", function_list.nvFBCGetLastErrorStr(session_data.fbcHandle));
        exit(EXIT_FAILURE);
    }
}