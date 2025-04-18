#include "nvfbc_v4l2.h"
#include <string.h>

NvFBC_InitData load_libraries() {
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

    const NVFBCSTATUS nvfbc_status = NvFBCCreateInstance_ptr(&function_list);
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

    const uint32_t framebuffer_width = DisplayWidth(dpy, XDefaultScreen(dpy));
    const uint32_t framebuffer_height = DisplayHeight(dpy, XDefaultScreen(dpy));

    const NvFBC_InitData result = {
            .X_display = dpy,

            // Replaced if screen is specified.
            .width = framebuffer_width,
            .height = framebuffer_height
    };
    return result;
}

NvFBC_SessionData create_session(NvFBC_InitData init_data, Capture_Settings capture_settings, void **frame_ptr,
                                 enum Pixel_Format pixel_fmt) {
    NVFBCSTATUS nvfbc_status;

    NVFBC_SESSION_HANDLE fbc_handle;
    NVFBC_CREATE_HANDLE_PARAMS create_handle_params;
    NVFBC_GET_STATUS_PARAMS status_params;
    NVFBC_CREATE_CAPTURE_SESSION_PARAMS create_capture_params;
    NVFBC_TOSYS_SETUP_PARAMS setup_params;

    NVFBC_BOX capture_box = {
            .x = 0,
            .y = 0,

            .w = init_data.width,
            .h = init_data.height
    };

    NVFBC_SIZE capture_size = {
            .w = init_data.width,
            .h = init_data.height
    };

    // Create new session handle.
    memset(&create_handle_params, 0, sizeof(create_handle_params));
    create_handle_params.dwVersion = NVFBC_CREATE_HANDLE_PARAMS_VER;

    nvfbc_status = function_list.nvFBCCreateHandle(&fbc_handle, &create_handle_params);
    if (nvfbc_status != NVFBC_SUCCESS) {
        fprintf(stderr, "Failed to create a NvFBC handle normally (%i), trying with interop key. \n",
                nvfbc_status);
        const uint8_t enable_key[] = {0xac, 0x10, 0xc9, 0x2e, 0xa5, 0xe6,
                                      0x87, 0x4f,0x8f,0x4b, 0xf4, 0x61,
                                      0xf8,0x56, 0x27, 0xe9};
        create_handle_params.privateData = enable_key;
        create_handle_params.privateDataSize = 16;

        nvfbc_status = function_list.nvFBCCreateHandle(&fbc_handle, &create_handle_params);
    }

    if (nvfbc_status != NVFBC_SUCCESS) {
        fprintf(stderr, "Failed to create a NvFBC handle with the following error type: '%i', and error message '%s'. "
                        "Exiting. \n", nvfbc_status, function_list.nvFBCGetLastErrorStr(fbc_handle));
        exit(EXIT_FAILURE);
    }

    // Get display driver state.
    memset(&status_params, 0, sizeof(status_params));
    status_params.dwVersion = NVFBC_GET_STATUS_PARAMS_VER;

    nvfbc_status = function_list.nvFBCGetStatus(fbc_handle, &status_params);
    if (nvfbc_status != NVFBC_SUCCESS) {
        fprintf(stderr, "NvFBC failed to get the display driver's state with the following error: '%s'. Exiting. \n",
                function_list.nvFBCGetLastErrorStr(fbc_handle));
        exit(EXIT_FAILURE);
    }

    if (status_params.bCanCreateNow == NVFBC_FALSE) {
        fprintf(stderr, "Unable to create a capture session on this system currently. Exiting. \n");
        exit(EXIT_FAILURE);
    }

    // Create a capture session that captures to system memory.
    memset(&create_capture_params, 0, sizeof(create_capture_params));

    create_capture_params.dwVersion = NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER;
    create_capture_params.eCaptureType = NVFBC_CAPTURE_TO_SYS;
    create_capture_params.captureBox = capture_box;
    create_capture_params.frameSize = capture_size;

    create_capture_params.bRoundFrameSize = NVFBC_FALSE;
    create_capture_params.dwOutputId = init_data.display_id;
    create_capture_params.eTrackingType = capture_settings.screen == -1 ? NVFBC_TRACKING_SCREEN : NVFBC_TRACKING_OUTPUT;
    create_capture_params.bWithCursor = capture_settings.show_cursor ? NVFBC_TRUE : NVFBC_FALSE;
    create_capture_params.bPushModel = capture_settings.push_model ? NVFBC_TRUE : NVFBC_FALSE;
    create_capture_params.bAllowDirectCapture = capture_settings.direct_capture ? NVFBC_TRUE : NVFBC_FALSE;
    create_capture_params.dwSamplingRateMs = (int) lround(1000.0 / capture_settings.fps);

    nvfbc_status = function_list.nvFBCCreateCaptureSession(fbc_handle, &create_capture_params);
    if (nvfbc_status != NVFBC_SUCCESS) {
        fprintf(stderr, "%s\n", function_list.nvFBCGetLastErrorStr(fbc_handle));
        exit(EXIT_FAILURE);
    }

    memset(&setup_params, 0, sizeof(setup_params));

    setup_params.dwVersion = NVFBC_TOSYS_SETUP_PARAMS_VER;
    setup_params.eBufferFormat = get_nvfbc_pixel_format(pixel_fmt);
    setup_params.ppBuffer = frame_ptr;
    setup_params.bWithDiffMap = NVFBC_FALSE;

    nvfbc_status = function_list.nvFBCToSysSetUp(fbc_handle, &setup_params);
    if (nvfbc_status != NVFBC_SUCCESS) {
        fprintf(stderr, "%s\n", function_list.nvFBCGetLastErrorStr(fbc_handle));
        exit(EXIT_FAILURE);
    }

    NvFBC_SessionData result = {
            .create_capture_params = create_capture_params,
            .create_handle_params = create_handle_params,
            .fbc_handle = fbc_handle,
            .status_params = status_params
    };
    return result;
}

void destroy_session(const NvFBC_SessionData session_data) {
    NVFBC_DESTROY_CAPTURE_SESSION_PARAMS destroy_capture_params;
    NVFBC_DESTROY_HANDLE_PARAMS destroy_handle_params;

    // Destroy capture session.
    memset(&destroy_capture_params, 0, sizeof(destroy_capture_params));
    destroy_capture_params.dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER;

    NVFBCSTATUS nvfbc_status = function_list.nvFBCDestroyCaptureSession(
            session_data.fbc_handle, &destroy_capture_params);
    if (nvfbc_status != NVFBC_SUCCESS) {
        fprintf(stderr, "%s\n", function_list.nvFBCGetLastErrorStr(session_data.fbc_handle));
        exit(EXIT_FAILURE);
    }

    // Destroy session handle.
    memset(&destroy_handle_params, 0, sizeof(destroy_handle_params));
    destroy_handle_params.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER;

    nvfbc_status = function_list.nvFBCDestroyHandle(session_data.fbc_handle, &destroy_handle_params);
    if (nvfbc_status != NVFBC_SUCCESS) {
        fprintf(stderr, "%s\n", function_list.nvFBCGetLastErrorStr(session_data.fbc_handle));
        exit(EXIT_FAILURE);
    }
}

void capture_frame(const NvFBC_SessionData *session_data, u_int32_t timeout_ms) {
    NVFBC_TOSYS_GRAB_FRAME_PARAMS grab_params;
    NVFBC_FRAME_GRAB_INFO frame_info;

    memset(&grab_params, 0, sizeof(grab_params));
    memset(&frame_info, 0, sizeof(frame_info));

    grab_params.dwVersion = NVFBC_TOSYS_GRAB_FRAME_PARAMS_VER;

    // Block until screen or mouse update.
    grab_params.dwFlags = NVFBC_TOSYS_GRAB_FLAGS_NOFLAGS;

    // Sets the timeout to the current capture framerate.
    grab_params.dwTimeoutMs = timeout_ms;

    // Frame info.
    grab_params.pFrameGrabInfo = &frame_info;

    // Captures a frame.
    const NVFBCSTATUS nvfbc_status = function_list.nvFBCToSysGrabFrame(session_data->fbc_handle, &grab_params);
    if (nvfbc_status != NVFBC_SUCCESS) {
        fprintf(stderr, "Capturing frame failed: '%s'\n", function_list.nvFBCGetLastErrorStr(session_data->fbc_handle));
        exit(EXIT_FAILURE);
    }
}