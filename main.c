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

int main(int argc, char* argv[]) {
    bool list = false;
    int32_t opt;
    int32_t output_device = -1;
    CaptureSettings capture_settings = {
            .push_model = true,
            .direct_capture = false,
            .show_cursor = true,
            .fps = 60
    };

    struct option long_options[] = {
            {"output-device", required_argument, NULL, 'o'},
            {"screen", required_argument, NULL, 's'},
            {"fps", required_argument, NULL, 'f'},
            {"no-push-model", no_argument, NULL, 'p'},
            {"direct-capture", no_argument, NULL, 'd'},
            {"no-cursor", no_argument, NULL, 'c'},
            {"list-screens", no_argument, NULL, 'l'},
            {"help", no_argument, NULL, 'h'},
            {NULL, 0, NULL, 0}
    };

    while ((opt = getopt_long(argc, argv, "o:s:f:pdc:l:h", long_options, NULL)) != -1) {
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

    byte* frame;
    void** frame_ptr = (void**) &frame;

    NvFBC_InitData nvfbc_data = load_library();
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

    NvFBC_SessionData nvfbc_session = create_session(nvfbc_data, capture_settings, frame_ptr);
    printf("Opening the V4L2 loopback device.\n");

    int32_t v4l2_device = open_device(output_device);
    set_device_format(v4l2_device, nvfbc_data.width, nvfbc_data.height);

    printf("Starting capture. Press CTRL+C to exit. \n");
    uint32_t buffer_size = (nvfbc_data.width * nvfbc_data.height) * 4 /* Bytes per pixel */;

    signal(SIGINT, interrupt_signal);
    for (;;) {
        if (quit_program == true) break;
        capture_frame(&nvfbc_session);
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
    printf("  -f, --fps <fps>               Sets the frames per second.\n");
    printf("  -p, --no-push-model           Disables push model.\n");
    printf("  -d, --direct-capture          Enables direct capture (warning: causes cursor issues when a screen is selected)\n");
    printf("  -c, --no-cursor               Hides the cursor.\n");
    printf("  -l, --list-screens            Lists available screens.\n");
    printf("  -h, --help                    Shows this help message.\n");
}