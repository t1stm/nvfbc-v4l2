#include <stdio.h>
#include <getopt.h>
#include <signal.h>

#include "v4l2_wrapper.h"
#include "nvfbc_v4l2.h"
#include "xrandr_wrapper.h"

static bool quit_program = false;

void show_help();

void interrupt_signal() {
    printf("\nCtrl+C pressed. Exiting.\n");
    quit_program = true;
}

enum Pixel_Format string_to_pixel_fmt(char* string) {
    return
    strcmp(string, "rgb") == 0 ? RGB_24 :
    strcmp(string, "yuv420") == 0 ? YUV_420 :
    strcmp(string, "rgba") == 0 ? RGBA_444 : Pixel_Fmt_None;
}

uint32_t get_pixel_buffer_size(uint32_t width, uint32_t height, enum Pixel_Format pixel_fmt) {
    assert(pixel_fmt != Pixel_Fmt_None);

    switch (pixel_fmt) {
        case YUV_420:
            return lround((width * height) * 1.5);
        case RGB_24:
            return (width * height) * 3;
        case RGBA_444:
            return (width * height) * 4;

        default:
            fprintf(stderr, "Invalid pixel format in buffer size calculator.\n");
            exit(EXIT_FAILURE);
    }
}

int main(int argc, char *argv[]) {
    bool list = false;
    int32_t opt;
    int32_t output_device = -1;
    Capture_Settings capture_settings = {
            .push_model = true,
            .direct_capture = false,
            .show_cursor = true,
            .fps = 60
    };

    enum Pixel_Format pixel_fmt = RGB_24;

    struct option long_options[] = {
            {"output-device",  required_argument, NULL, 'o'},
            {"screen",         required_argument, NULL, 's'},
            {"pixel_format",   required_argument, NULL, 'p'},
            {"fps",            required_argument, NULL, 'f'},
            {"no-push-model",  no_argument,       NULL, 'n'},
            {"direct-capture", no_argument,       NULL, 'd'},
            {"no-cursor",      no_argument,       NULL, 'c'},
            {"list-screens",   no_argument,       NULL, 'l'},
            {"help",           no_argument,       NULL, 'h'},
            {NULL,             0,                 NULL, 0}
    };

    while ((opt = getopt_long(argc, argv, "o:s:f:ndclhp:", long_options, NULL)) != -1) {
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

            case 'h':
                show_help();
                exit(EXIT_SUCCESS);

            default:
                printf("Invalid argument: -%c\n", optopt);
                printf("To see all available arguments use the -h or --help arguments.\n");
                exit(EXIT_FAILURE);
        }
    }

    if (output_device == -1) {
        printf("Error: Required argument \'output-device\' not specified.\n");
        printf("To see all available arguments use the -h or --help arguments.\n");
        exit(EXIT_FAILURE);
    }

    printf("Loading the NvFBC library.\n");

    byte *frame;
    void **frame_ptr = (void **) &frame;

    NvFBC_InitData nvfbc_data = load_libraries();
    X_Data x_data = get_screens(nvfbc_data.X_display);

    if (list == true) {
        list_screens(x_data);
        exit(EXIT_SUCCESS);
    }

    printf("Output device: /dev/video%u\n", output_device);
    printf("Screen: %i\n", capture_settings.screen);

    if (capture_settings.screen != -1) {
        if (capture_settings.screen >= x_data.count) {
            fprintf(stderr, "Requested screen index is bigger than the display count.\n");
            exit(EXIT_FAILURE);
        }

        X_Screen selected_screen = x_data.screens[capture_settings.screen];

        nvfbc_data.offset_x = selected_screen.offset_x;
        nvfbc_data.offset_y = selected_screen.offset_y;
        nvfbc_data.width = selected_screen.size_w;
        nvfbc_data.height = selected_screen.size_h;

        nvfbc_data.display_id = selected_screen.id;
    }

    NvFBC_SessionData nvfbc_session = create_session(nvfbc_data, capture_settings, frame_ptr, pixel_fmt);
    printf("Opening the V4L2 loopback device.\n");

    int32_t v4l2_device = open_device(output_device);
    set_device_format(v4l2_device, nvfbc_data.width, nvfbc_data.height, pixel_fmt);

    printf("Starting capture. Press CTRL+C to exit. \n");
    uint32_t buffer_size = get_pixel_buffer_size(nvfbc_data.width, nvfbc_data.height, pixel_fmt);

    NvFBC_SessionData* session_pointer = &nvfbc_session;
    signal(SIGINT, interrupt_signal);
    for (;;) {
        if (quit_program == true) break;
        capture_frame(session_pointer);
        write_frame(v4l2_device, frame_ptr, buffer_size);
    }

    destroy_session(nvfbc_session);
    return EXIT_SUCCESS;
}

void show_help() {
    printf("Usage: nvfbc-v4l2 [options]\n");
    printf("Options:\n");
    printf("  -o, --output-device <device>  REQUIRED: Sets the V4L2 output device number.\n");
    printf("  -s, --screen <screen>         Sets the requested X screen.\n");
    printf("  -p, --pixel_format <pixel_fmt>         Sets the wanted pixel format.\n");
    printf("  -f, --fps <fps>               Sets the frames per second.\n");
    printf("  -n, --no-push-model           Disables push model.\n");
    printf("  -d, --direct-capture          Enables direct capture (warning: causes cursor issues when a screen is selected)\n");
    printf("  -c, --no-cursor               Hides the cursor.\n");
    printf("  -l, --list-screens            Lists available screens.\n");
    printf("  -h, --help                    Shows this help message.\n");
}