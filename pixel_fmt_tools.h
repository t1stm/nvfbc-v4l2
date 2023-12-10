#ifndef NVFBC_V4L2_PIXEL_FMT_TOOLS_H
#define NVFBC_V4L2_PIXEL_FMT_TOOLS_H

#include <stdint.h>

typedef struct {
    uint8_t* u_plane;
    uint8_t* y_plane;
} YUV_420_Data;

static void inplace_nv12_to_yuv420p(void* frame, const uint32_t width, const uint32_t height, YUV_420_Data* yuv_data) {
    const uint32_t frame_size = width * height;
    const uint32_t chroma_size = frame_size / 4;

    unsigned char *uv_plane = frame + frame_size;

    if (yuv_data->u_plane == NULL) {
        yuv_data->u_plane = malloc(chroma_size);
    }

    if (yuv_data->y_plane == NULL) {
        yuv_data->y_plane = malloc(chroma_size);
    }

    for (int i = 0; i < chroma_size * 2; i += 2) {
        yuv_data->u_plane[i / 2] = uv_plane[i];
        yuv_data->y_plane[i / 2] = uv_plane[i + 1];
    }

    memcpy(uv_plane, yuv_data->u_plane, chroma_size);
    memcpy(uv_plane + chroma_size, yuv_data->y_plane, chroma_size);
}

#endif
