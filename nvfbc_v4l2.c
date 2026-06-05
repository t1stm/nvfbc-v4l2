#include "nvfbc_v4l2.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <math.h>
#include <dlfcn.h>
#include <pthread.h>

NVFBC_BACKEND resolve_backend(const NVFBC_BACKEND requested) {
    if (requested != NVFBC_BACKEND_AUTO) {
        return requested;
    }

    const char *session_type = getenv("XDG_SESSION_TYPE");
    if (session_type != NULL && strcmp(session_type, "wayland") == 0) {
        return NVFBC_BACKEND_PIPEWIRE;
    }
    if (getenv("DISPLAY") != NULL) {
        return NVFBC_BACKEND_X11;
    }
    return NVFBC_BACKEND_DIRECT;
}

const char *default_portal_token_path(void) {
    static char path[4096];

    const char *cache = getenv("XDG_CACHE_HOME");
    if (cache != NULL && cache[0] != '\0') {
        snprintf(path, sizeof(path), "%s/nvfbc-v4l2/portal.token", cache);
        return path;
    }

    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        return NULL;
    }
    snprintf(path, sizeof(path), "%s/.cache/nvfbc-v4l2/portal.token", home);
    return path;
}

static bool load_portal_token(const char *path, char *token_out) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }

    const size_t read = fread(token_out, 1, NVFBC_PORTAL_RESTORE_TOKEN_LEN - 1, file);
    fclose(file);
    if (read == 0) {
        return false;
    }

    token_out[read] = '\0';
    char *newline = strchr(token_out, '\n');
    if (newline != NULL) {
        *newline = '\0';
    }
    return true;
}


static void basic_mkdir_p(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);

    for (char *p = tmp + 1; *p != '\0'; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

static void save_portal_token(const char *path, const char *token) {
    basic_mkdir_p(path);

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "Warning: could not write portal restore token to '%s'.\n", path);
        return;
    }

    fputs(token, file);
    fputc('\n', file);
    fclose(file);
    fprintf(stderr, "Saved XDG portal restore token to '%s'.\n", path);
}

NvFBC_InitData load_libraries(const NVFBC_BACKEND backend) {
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

    uint32_t framebuffer_width = 0;
    uint32_t framebuffer_height = 0;

    if (backend == NVFBC_BACKEND_X11) {
        dpy = XOpenDisplay(NULL); // display is only needed on X11
        if (dpy == NULL) {
            fprintf(stderr, "Unable to open X11 display. \n");
            exit(EXIT_FAILURE);
        }

        framebuffer_width = DisplayWidth(dpy, XDefaultScreen(dpy));
        framebuffer_height = DisplayHeight(dpy, XDefaultScreen(dpy));
    }

    const NvFBC_InitData result = {
        .X_display = dpy,
        .width = framebuffer_width,
        .height = framebuffer_height
    };
    return result;
}

static NVFBCSTATUS create_nvfbc_handle(NVFBC_SESSION_HANDLE *handle,
                                       NVFBC_CREATE_HANDLE_PARAMS *params,
                                       const NVFBC_BACKEND backend,
                                       const char *portal_token_path,
                                       const bool force_new_xdg_token) {
    memset(params, 0, sizeof(*params));
    params->dwVersion = NVFBC_CREATE_HANDLE_PARAMS_VER;
    params->eBackend = backend;
    params->bUseEGL = backend != NVFBC_BACKEND_X11 ? NVFBC_TRUE : NVFBC_FALSE;

    if (backend == NVFBC_BACKEND_PIPEWIRE) {
        if (force_new_xdg_token) {
            fprintf(stderr, "Forcing a new portal session (ignoring any saved restore token).\n");
        } else if (portal_token_path != NULL &&
                   load_portal_token(portal_token_path, params->portalRestoreToken)) {
            fprintf(stderr, "Loaded XDG portal restore token from '%s'.\n", portal_token_path);
        }
    }

    NVFBCSTATUS nvfbc_status = function_list.nvFBCCreateHandle(handle, params);
    if (nvfbc_status != NVFBC_SUCCESS) {
        fprintf(stderr, "Failed to create a NvFBC handle normally (%i), trying with interop key. \n",
                nvfbc_status);
        static const uint8_t enable_key[] = {
            0xac, 0x10, 0xc9, 0x2e, 0xa5, 0xe6,
            0x87, 0x4f, 0x8f, 0x4b, 0xf4, 0x61,
            0xf8, 0x56, 0x27, 0xe9
        };
        params->privateData = enable_key;
        params->privateDataSize = 16;

        nvfbc_status = function_list.nvFBCCreateHandle(handle, params);
    }

    return nvfbc_status;
}

void list_direct_targets(const uint32_t pid) {
    NVFBC_SESSION_HANDLE fbc_handle;
    NVFBC_CREATE_HANDLE_PARAMS create_handle_params;

    NVFBCSTATUS nvfbc_status = create_nvfbc_handle(&fbc_handle, &create_handle_params,
                                                   NVFBC_BACKEND_DIRECT, NULL, false);
    if (nvfbc_status != NVFBC_SUCCESS) {
        fprintf(stderr, "Failed to create a NvFBC handle for the direct backend: '%s'.\n",
                function_list.nvFBCGetLastErrorStr(fbc_handle));
        exit(EXIT_FAILURE);
    }

    NVFBC_GET_STATUS_PARAMS status_params = {0};
    status_params.dwVersion = NVFBC_GET_STATUS_PARAMS_VER;
    status_params.dwPid = pid;
    status_params.dwDbusTimeoutMs = 1000;

    nvfbc_status = function_list.nvFBCGetStatus(fbc_handle, &status_params);
    if (nvfbc_status != NVFBC_SUCCESS) {
        fprintf(stderr, "Failed to query direct capture targets for pid %u: '%s'.\n",
                pid, function_list.nvFBCGetLastErrorStr(fbc_handle));
        exit(EXIT_FAILURE);
    }

    if (status_params.dwCaptureTargetCount == 0) {
        fprintf(stderr, "No capture targets found for pid %u. Is it a running Vulkan application?\n", pid);
    } else {
        fprintf(stderr, "Capture targets for pid %u:\n", pid);
        for (uint32_t i = 0; i < status_params.dwCaptureTargetCount; ++i) {
            fprintf(stderr, "  Target %u: %ux%u\n", i,
                    status_params.captureTargetSizes[i].w,
                    status_params.captureTargetSizes[i].h);
        }
    }

    NVFBC_DESTROY_HANDLE_PARAMS destroy_handle_params = {0};
    destroy_handle_params.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER;
    function_list.nvFBCDestroyHandle(fbc_handle, &destroy_handle_params);
}

NvFBC_SessionData create_session(NvFBC_InitData init_data, Capture_Settings capture_settings, void **frame_ptr,
                                 enum Pixel_Format pixel_fmt) {
    NVFBCSTATUS nvfbc_status;
    const NVFBC_BACKEND backend = capture_settings.backend;

    NVFBC_SESSION_HANDLE fbc_handle;
    NVFBC_CREATE_HANDLE_PARAMS create_handle_params;
    NVFBC_GET_STATUS_PARAMS status_params;
    NVFBC_CREATE_CAPTURE_SESSION_PARAMS create_capture_params;
    NVFBC_TOSYS_SETUP_PARAMS setup_params;

    // Create new session handle.
    nvfbc_status = create_nvfbc_handle(&fbc_handle, &create_handle_params, backend,
                                       capture_settings.xdg_portal_token_path, capture_settings.issue_new_xdg_token);
    if (nvfbc_status != NVFBC_SUCCESS) {
        fprintf(stderr, "Failed to create a NvFBC handle with the following error type: '%i', and error message '%s'. "
                "Exiting. \n", nvfbc_status, function_list.nvFBCGetLastErrorStr(fbc_handle));
        exit(EXIT_FAILURE);
    }

    // Get display driver state.
    memset(&status_params, 0, sizeof(status_params));
    status_params.dwVersion = NVFBC_GET_STATUS_PARAMS_VER;
    if (backend == NVFBC_BACKEND_DIRECT) {
        status_params.dwPid = capture_settings.vulkan_pid;
        status_params.dwDbusTimeoutMs = 1000;
    }

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

    if (backend == NVFBC_BACKEND_DIRECT) {
        if (status_params.dwCaptureTargetCount == 0) {
            fprintf(stderr, "No capture targets found for pid %u. Is it a running Vulkan application?\n",
                    capture_settings.vulkan_pid);
            exit(EXIT_FAILURE);
        }
        if (capture_settings.vulkan_target_index >= status_params.dwCaptureTargetCount) {
            fprintf(stderr, "Requested capture target %u is out of range (found %u targets).\n",
                    capture_settings.vulkan_target_index, status_params.dwCaptureTargetCount);
            exit(EXIT_FAILURE);
        }
    }

    // Create a capture session that captures to system memory.
    memset(&create_capture_params, 0, sizeof(create_capture_params));
    create_capture_params.dwVersion = NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER;
    create_capture_params.eCaptureType = NVFBC_CAPTURE_TO_SYS;
    create_capture_params.bRoundFrameSize = NVFBC_FALSE;
    create_capture_params.bWithCursor = capture_settings.show_cursor ? NVFBC_TRUE : NVFBC_FALSE;
    create_capture_params.dwSamplingRateMs = (int) lround(1000.0 / capture_settings.fps);

    switch (backend) {
        case NVFBC_BACKEND_AUTO:
            fprintf(stderr, "Auto backend should already be decided, but has reached create_capture_params. \n");
            exit(EXIT_FAILURE);
        case NVFBC_BACKEND_X11:
            const NVFBC_BOX capture_box = {
                .x = 0,
                .y = 0,
                .w = init_data.width,
                .h = init_data.height
            };
            const NVFBC_SIZE capture_size = {
                .w = init_data.width,
                .h = init_data.height
            };

            create_capture_params.captureBox = capture_box;
            create_capture_params.frameSize = capture_size;
            create_capture_params.dwOutputId = init_data.display_id;
            create_capture_params.eTrackingType =
                    capture_settings.screen == -1 ? NVFBC_TRACKING_SCREEN : NVFBC_TRACKING_OUTPUT;
            create_capture_params.bPushModel = capture_settings.push_model ? NVFBC_TRUE : NVFBC_FALSE;
            create_capture_params.bAllowDirectCapture = capture_settings.direct_capture ? NVFBC_TRUE : NVFBC_FALSE;
            break;
        case NVFBC_BACKEND_PIPEWIRE:
            create_capture_params.eTrackingType = NVFBC_TRACKING_DEFAULT;
            create_capture_params.bPushModel = NVFBC_FALSE;
            break;
        case NVFBC_BACKEND_DIRECT:
            create_capture_params.eTrackingType = NVFBC_TRACKING_DEFAULT;
            create_capture_params.bPushModel = NVFBC_TRUE;
            create_capture_params.bWithCursor = NVFBC_FALSE;
            create_capture_params.dwPid = capture_settings.vulkan_pid;
            create_capture_params.dwCaptureTarget = capture_settings.vulkan_target_index;
            create_capture_params.dwDbusTimeoutMs = 1000;
            break;
    }

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

    if (backend == NVFBC_BACKEND_PIPEWIRE && capture_settings.xdg_portal_token_path != NULL) {
        NVFBC_GET_STATUS_PARAMS token_status = {0};
        token_status.dwVersion = NVFBC_GET_STATUS_PARAMS_VER;

        if (function_list.nvFBCGetStatus(fbc_handle, &token_status) == NVFBC_SUCCESS &&
            token_status.portalRestoreToken[0] != '\0') {
            save_portal_token(capture_settings.xdg_portal_token_path, token_status.portalRestoreToken);
        }
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

void capture_frame(const NvFBC_SessionData *session_data, const uint32_t timeout_ms, NVFBC_FRAME_GRAB_INFO *frame_info) {
    NVFBC_TOSYS_GRAB_FRAME_PARAMS grab_params = {0};

    memset(frame_info, 0, sizeof(*frame_info));

    grab_params.dwVersion = NVFBC_TOSYS_GRAB_FRAME_PARAMS_VER;

    // Block until screen or mouse update.
    grab_params.dwFlags = NVFBC_TOSYS_GRAB_FLAGS_NOFLAGS;

    // Sets the timeout to the current capture framerate.
    grab_params.dwTimeoutMs = timeout_ms;

    // Frame info, including the real dimensions and byte size of the grabbed frame.
    grab_params.pFrameGrabInfo = frame_info;

    // Captures a frame.
    const NVFBCSTATUS nvfbc_status = function_list.nvFBCToSysGrabFrame(session_data->fbc_handle, &grab_params);
    if (nvfbc_status != NVFBC_SUCCESS) {
        fprintf(stderr, "Capturing frame failed: '%s'\n", function_list.nvFBCGetLastErrorStr(session_data->fbc_handle));
        exit(EXIT_FAILURE);
    }
}