#ifndef NVFBC_V4L2_PIXEL_FMT_TOOLS_H
#define NVFBC_V4L2_PIXEL_FMT_TOOLS_H

#include <stdint.h>
#include <stddef.h>
#include <malloc.h>

typedef struct {
    uint8_t* u_plane;
    uint8_t* y_plane;
} YUV_420_Data;

static void inplace_nv12_to_yuv420p(void* frame, uint32_t width, uint32_t height, YUV_420_Data* yuv_data) {
    uint32_t frameSize = width * height;
    uint32_t chromaSize = frameSize / 4;

    unsigned char *uv_plane = frame + frameSize;

    if (yuv_data->u_plane == NULL) {
        yuv_data->u_plane = malloc(chromaSize);
    }

    if (yuv_data->y_plane == NULL) {
        yuv_data->y_plane = malloc(chromaSize);
    }

    for (int i = 0; i < chromaSize * 2; i += 2) {
        yuv_data->u_plane[i / 2] = uv_plane[i];
        yuv_data->y_plane[i / 2] = uv_plane[i + 1];
    }

    memcpy(uv_plane, yuv_data->u_plane, chromaSize);
    memcpy(uv_plane + chromaSize, yuv_data->y_plane, chromaSize);
}

#endif
