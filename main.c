#include <stdio.h>
#include <getopt.h>
#include <math.h>
#include <signal.h>

#include "v4l2_wrapper.h"
#include "nvfbc_v4l2.h"
#include "xrandr_wrapper.h"
#include "pixel_fmt_tools.h"

// these are placeholders for non-existent ASCII codes
enum {
    OPT_PORTAL_TOKEN = 1000,
    OPT_NEW_TOKEN    = 1001
};

static bool quit_program = false;

void show_help();

void interrupt_signal() {
    fprintf(stderr,"\nCtrl+C pressed. Exiting.\n");
    quit_program = true;
}

enum Pixel_Format string_to_pixel_fmt(const char* string) {
    return
    strcmp(string, "rgb") == 0 ? RGB_24 :
    strcmp(string, "yuv420") == 0 ? YUV_420 :
    strcmp(string, "rgba") == 0 ? RGBA_444 :
    strcmp(string, "nv12") == 0 ? NV_12 : Pixel_Fmt_None;
}

// Returns the requested backend, or (NVFBC_BACKEND) -1 for an unknown name.
static NVFBC_BACKEND string_to_backend(const char* string) {
    return
    strcmp(string, "auto") == 0 ? NVFBC_BACKEND_AUTO :
    strcmp(string, "x11") == 0 ? NVFBC_BACKEND_X11 :
    strcmp(string, "pipewire") == 0 ? NVFBC_BACKEND_PIPEWIRE :
    strcmp(string, "direct") == 0 ? NVFBC_BACKEND_DIRECT : (NVFBC_BACKEND) -1;
}

static const char* backend_name(const NVFBC_BACKEND backend) {
    switch (backend) {
        case NVFBC_BACKEND_X11:      return "X11";
        case NVFBC_BACKEND_PIPEWIRE: return "PipeWire (Wayland)";
        case NVFBC_BACKEND_DIRECT:   return "Direct (Vulkan)";
        default:                     return "auto";
    }
}

uint32_t get_pixel_buffer_size(const uint32_t width, const uint32_t height, const enum Pixel_Format pixel_fmt) {
    assert(pixel_fmt != Pixel_Fmt_None);

    switch (pixel_fmt) {
        case NV_12:
        case YUV_420:
            return lround(width * height * 1.5);
        case RGB_24:
            return width * height * 3;
        case RGBA_444:
            return width * height * 4;

        default:
            fprintf(stderr, "Invalid pixel format in buffer size calculator.\n");
            exit(EXIT_FAILURE);
    }
}

void yuv420_loop(void** frame_ptr, uint32_t width, uint32_t height, int32_t v4l2_device, uint32_t buffer_size, const NvFBC_SessionData* session_pointer, YUV_420_Data** yuv_data, const Capture_Settings* capture_settings);
void normal_loop(void** frame_ptr, int32_t v4l2_device, uint32_t buffer_size, const NvFBC_SessionData* session_pointer, const Capture_Settings* capture_settings);

int main(const int argc, char* const argv[]) {
    bool list = false;
    bool stdout_output = false;
    int32_t opt;
    int32_t output_device = -1;
    Capture_Settings capture_settings = {
            .push_model = true,
            .direct_capture = false,
            .show_cursor = true,
            .fps = 60,
            .backend = NVFBC_BACKEND_AUTO,
            .vulkan_pid = 0,
            .vulkan_target_index = 0,
            .xdg_portal_token_path = NULL,
            .issue_new_xdg_token = false
    };

    enum Pixel_Format pixel_fmt = RGB_24;

    const struct option long_options[] = {
            {"output-device",  required_argument, NULL, 'o'},
            {"screen",         required_argument, NULL, 's'},
            {"pixel-format",   required_argument, NULL, 'p'},
            {"fps",            required_argument, NULL, 'f'},
            {"backend",        required_argument, NULL, 'b'},
            {"pid",            required_argument, NULL, 'P'},
            {"target",         required_argument, NULL, 't'},
            {"portal-token",   required_argument, NULL, OPT_PORTAL_TOKEN},
            {"new-token",      no_argument,       NULL, OPT_NEW_TOKEN},
            {"stdout",         no_argument,       NULL, 'r'},
            {"no-push-model",  no_argument,       NULL, 'n'},
            {"direct-capture", no_argument,       NULL, 'd'},
            {"no-cursor",      no_argument,       NULL, 'c'},
            {"list-screens",   no_argument,       NULL, 'l'},
            {"help",           no_argument,       NULL, 'h'},
            {NULL,             0,                 NULL, 0}
    };

    // shortopts explained
    // after each .val you can notice a ':'
    // this means that the argument is required.
    while ((opt = getopt_long(argc, argv, "o:s:p:f:b:P:t:rndclh", long_options, NULL)) != -1) {
        int32_t temporary;
        switch (opt) {
            case 0:
                break;
            case 'o':
                temporary = atoi(optarg);
                output_device = temporary > -1 && temporary < 256 ? temporary : -1;
                break;
            case 's':
                capture_settings.screen = atoi(optarg);
                break;
            case 'f':
                capture_settings.fps = atoi(optarg);
                break;
            case 'p':
                pixel_fmt = string_to_pixel_fmt(optarg);
                if (pixel_fmt != Pixel_Fmt_None) break;

                fprintf(stderr, "Invalid pixel format specified: --pixel_format = %s\n", optarg);
                exit(EXIT_FAILURE);

            case 'b':
                capture_settings.backend = string_to_backend(optarg);
                if (capture_settings.backend != -1) break;

                fprintf(stderr, "Invalid backend specified: --backend = %s "
                                "(allowed: auto, x11, pipewire, direct)\n", optarg);
                exit(EXIT_FAILURE);

            case 'P':
                temporary = atoi(optarg);
                if (temporary <= 0) {
                    fprintf(stderr, "Invalid pid specified: --pid = %s\n", optarg);
                    exit(EXIT_FAILURE);
                }
                capture_settings.vulkan_pid = (uint32_t) temporary;
                break;

            case 't':
                capture_settings.vulkan_target_index = (uint32_t) atoi(optarg);
                break;

            case OPT_PORTAL_TOKEN:
                capture_settings.xdg_portal_token_path = optarg;
                break;

            case OPT_NEW_TOKEN:
                capture_settings.issue_new_xdg_token = true;
                break;

            case 'n':
                capture_settings.push_model = false;
                break;
            case 'd':
                fprintf(stderr, "WARNING: There is a bug in NvFBC that causes the cursor "
                                "to appear as a black box when direct capture is enabled in this program and a screen is specified.\n");
                capture_settings.direct_capture = true;
                break;
            case 'c':
                capture_settings.show_cursor = false;
                break;

            case 'l':
                list = true;
                break;

            case 'r':
                stdout_output = true;
                break;

            case 'h':
                show_help();
                exit(EXIT_SUCCESS);

            default:
                fprintf(stderr,"Invalid argument: -%c\n", optopt);
                fprintf(stderr,"To see all available arguments use the -h or --help arguments.\n");
                exit(EXIT_FAILURE);
        }
    }

    // A target process implies the direct backend.
    if (capture_settings.vulkan_pid != 0 && capture_settings.backend != NVFBC_BACKEND_DIRECT) {
        if (capture_settings.backend != NVFBC_BACKEND_AUTO) {
            fprintf(stderr, "Note: --pid implies the direct backend; overriding the requested backend.\n");
        }
        capture_settings.backend = NVFBC_BACKEND_DIRECT;
    }

    // An output device is only required when actually capturing to a v4l2 device.
    if (!list && !stdout_output && output_device == -1) {
        fprintf(stderr,"Error: Required argument \'output-device\' not specified.\n");
        fprintf(stderr,"To see all available arguments use the -h or --help arguments.\n");
        exit(EXIT_FAILURE);
    }

    fprintf(stderr,"Loading the NvFBC library.\n");

    byte *frame;
    void **frame_ptr = (void **) &frame;

    const NVFBC_BACKEND backend = resolve_backend(capture_settings.backend);
    capture_settings.backend = backend;
    fprintf(stderr,"Backend: %s\n", backend_name(backend));

    // PipeWire reuses a saved portal token (unless overridden) to skip the dialog.
    if (capture_settings.xdg_portal_token_path == NULL) {
        capture_settings.xdg_portal_token_path = default_portal_token_path();
    }

    NvFBC_InitData nvfbc_data = load_libraries(backend);

    // Per-backend display querying and --list-screens handling.
    X_Data x_data = { .screens = NULL, .count = 0 };

    if (backend == NVFBC_BACKEND_X11) {
        x_data = get_screens(nvfbc_data.X_display);
        if (list) {
            list_screens(x_data);
            exit(EXIT_SUCCESS);
        }
    } else if (backend == NVFBC_BACKEND_PIPEWIRE) {
        if (list) {
            fprintf(stderr, "On the PipeWire (Wayland) backend the screen is chosen through the\n"
                            "compositor's screen-picker dialog when capture starts, so there is\n"
                            "nothing to list here.\n");
            exit(EXIT_SUCCESS);
        }
    } else { // NVFBC_BACKEND_DIRECT
        if (capture_settings.vulkan_pid == 0) {
            fprintf(stderr, "The direct backend requires a target process. Pass --pid <pid>.\n");
            exit(EXIT_FAILURE);
        }
        if (list) {
            list_direct_targets(capture_settings.vulkan_pid);
            exit(EXIT_SUCCESS);
        }
    }

    if (stdout_output) {
        fprintf(stderr,"Output device: stdout\n");
    }
    else fprintf(stderr,"Output device: /dev/video%u\n", output_device);

    // X11: optionally track a specific RandR output. (screen == -1 captures the
    // whole framebuffer.)
    if (backend == NVFBC_BACKEND_X11 && capture_settings.screen != -1) {
        fprintf(stderr,"Screen: %i\n", capture_settings.screen);

        if (capture_settings.screen >= x_data.count) {
            fprintf(stderr, "Requested screen index is bigger than the display count.\n");
            exit(EXIT_FAILURE);
        }

        const X_Screen selected_screen = x_data.screens[capture_settings.screen];

        nvfbc_data.offset_x = selected_screen.offset_x;
        nvfbc_data.offset_y = selected_screen.offset_y;
        nvfbc_data.width = selected_screen.size_w;
        nvfbc_data.height = selected_screen.size_h;

        nvfbc_data.display_id = selected_screen.id;
    }

    const NvFBC_SessionData nvfbc_session = create_session(nvfbc_data, capture_settings, frame_ptr, pixel_fmt);
    const NvFBC_SessionData* session_pointer = &nvfbc_session;

    fprintf(stderr,"Starting capture. Press CTRL+C to exit. \n");

    // Grab a first frame to learn the real capture dimensions. The PipeWire and
    // direct backends cannot report the frame size before a session exists, so we
    // size the v4l2 device from the actual frame instead of a pre-queried screen.
    NVFBC_FRAME_GRAB_INFO frame_info;
    const uint32_t timeout = (uint32_t) lround(1000.0 / capture_settings.fps);
    capture_frame(session_pointer, timeout, &frame_info);

    const uint32_t width = frame_info.dwWidth;
    const uint32_t height = frame_info.dwHeight;
    fprintf(stderr,"Capture size: %ux%u\n", width, height);

    int32_t file_descriptor;
    if (stdout_output) file_descriptor = fileno(stdout);
    else {
        fprintf(stderr,"Opening the V4L2 loopback device.\n");
        file_descriptor = open_device(output_device);
        set_device_format(file_descriptor, width, height, pixel_fmt, capture_settings.fps);
    }

    const uint32_t buffer_size = get_pixel_buffer_size(width, height, pixel_fmt);

    signal(SIGINT, interrupt_signal);

    YUV_420_Data* yuv_data = NULL;
    if (pixel_fmt == YUV_420) {
        yuv_data = malloc(sizeof(YUV_420_Data));
        memset(yuv_data, 0, sizeof(YUV_420_Data));
        yuv420_loop(frame_ptr, width, height, file_descriptor, buffer_size, session_pointer, &yuv_data, &capture_settings);
    }
    else normal_loop(frame_ptr, file_descriptor, buffer_size, session_pointer, &capture_settings);

    destroy_session(nvfbc_session);
    if (pixel_fmt == YUV_420) {
        free(yuv_data->u_plane);
        free(yuv_data->y_plane);
        free(yuv_data);
    }
    return EXIT_SUCCESS;
}

void yuv420_loop(void** frame_ptr, const uint32_t width, const uint32_t height, const int32_t v4l2_device,
              const uint32_t buffer_size, const NvFBC_SessionData* session_pointer, YUV_420_Data** yuv_data, const Capture_Settings* capture_settings) {
    const uint32_t timeout = (uint32_t) lround(1000.0 / capture_settings->fps);
    NVFBC_FRAME_GRAB_INFO frame_info;

    while (quit_program != true) {
        capture_frame(session_pointer, timeout, &frame_info);
        inplace_nv12_to_yuv420p(*frame_ptr, width, height, *yuv_data);
        const uint32_t write_size = frame_info.dwByteSize != 0 ? frame_info.dwByteSize : buffer_size;
        write_frame(v4l2_device, frame_ptr, write_size);
    }
}

void normal_loop(void** frame_ptr, const int32_t v4l2_device, const uint32_t buffer_size, const NvFBC_SessionData* session_pointer, const Capture_Settings* capture_settings) {
    const uint32_t timeout = (uint32_t) lround(1000.0 / capture_settings->fps);
    NVFBC_FRAME_GRAB_INFO frame_info;

    while (quit_program != true) {
        capture_frame(session_pointer, timeout, &frame_info);
        const uint32_t write_size = frame_info.dwByteSize != 0 ? frame_info.dwByteSize : buffer_size;
        write_frame(v4l2_device, frame_ptr, write_size);
    }
}

void show_help() {
    fprintf(stderr, "Usage: nvfbc-v4l2 [options]\n");
    fprintf(stderr,"Options:\n");
    fprintf(stderr,"  -o, --output-device <device>  REQUIRED (unless --stdout/--list-screens): V4L2 output device number.\n");
    fprintf(stderr,"  -b, --backend <backend>       Capture backend: 'auto' (Default), 'x11', 'pipewire', 'direct'.\n");
    fprintf(stderr,"  -s, --screen <screen>         X11 backend: requested RandR output (-1 = whole framebuffer).\n");
    fprintf(stderr,"  -P, --pid <pid>               Direct backend: pid of the Vulkan application to capture (implies --backend direct).\n");
    fprintf(stderr,"  -t, --target <index>          Direct backend: index of the capture target (Default: 0).\n");
    fprintf(stderr,"      --portal-token <path>     PipeWire backend: XDG portal restore token file (Default: ~/.cache/nvfbc-v4l2/portal.token).\n");
    fprintf(stderr,"      --new-token               PipeWire backend: force a fresh portal session (shows the picker again) and replace the saved token.\n");
    fprintf(stderr,"  -p, --pixel_format <pix_fmt>  Sets the wanted pixel format. Allowed values: 'rgb' (Default), 'yuv420', 'rgba', 'nv12'\n");
    fprintf(stderr,"  -f, --fps <fps>               Sets the frames per second.\n");
    fprintf(stderr,"  -r, --stdout                  Write the raw video data to stdout instead of a v4l2 device.\n");
    fprintf(stderr,"  -n, --no-push-model           Disables push model.\n");
    fprintf(stderr,"  -d, --direct-capture          Enables direct capture. (warning: causes cursor issues when a screen is selected)\n");
    fprintf(stderr,"  -c, --no-cursor               Hides the cursor.\n");
    fprintf(stderr,"  -l, --list-screens            Lists available screens (or, with --pid, direct capture targets).\n");
    fprintf(stderr,"  -h, --help                    Shows this help message.\n");
}
