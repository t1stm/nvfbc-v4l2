#include <stdio.h>
#include <getopt.h>

#include "v4l2_wrapper.h"
#include "nvfbc_v4l2.h"

int main(int argc, char* argv[]) {
    int32_t opt;
    int32_t output_device = -1;
    CaptureSettings capture_settings;

    printf("Initializing settings.\n");

    struct option long_options[] = {
            {"output-device", required_argument, NULL, 'o'},
            {"screen", required_argument, NULL, 's'},
            {"fps", optional_argument, NULL, 'f'},
            {"push-model", no_argument, NULL, 'p'},
            {"direct-capture", no_argument, NULL, 'd'},
            {NULL, 0, NULL, 0}
    };

    while ((opt = getopt_long(argc, argv, "d:s:", long_options, NULL)) != -1) {
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
                capture_settings.push_model = true;
                break;
            case 'd':
                capture_settings.direct_capture = true;
                break;
            default:
                printf("Invalid option: -%c", optopt);
                exit(EXIT_FAILURE);
        }
    }

    printf("Loading the NvFBC library.\n");

    byte* frame;
    void** frame_ptr = (void**) &frame;

    NvFBC_InitData nvfbc_data = load_library();
    NvFBC_SessionData nvfbc_session = create_session(nvfbc_data, frame_ptr);

    printf("Opening the V4L2 loopback device.\n");

    int32_t v4l2_device = open_device(output_device);
    set_device_format(v4l2_device, nvfbc_data.width, nvfbc_data.height);

    for (int i = 0; i < 128; i++) {
        capture_frame(nvfbc_session);
    }

    destroy_session(nvfbc_session);
    return EXIT_SUCCESS;
}