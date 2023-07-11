#ifndef V4L2_WRAPPER_H
#define V4L2_WRAPPER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <linux/videodev2.h>
#include <linux/v4l2-common.h>
#include <linux/v4l2-controls.h>

#include "defines.h"

int32_t open_device(int32_t output_device) {
    // /dev/video###
    char device_location[13];
    sprintf(device_location, "/dev/video%u", output_device);
    int32_t file_descriptor = open((const char *) device_location, O_RDWR);
    if (file_descriptor == -1) {
        fprintf(stderr, "Unable to open v4l2loopback device: '/dev/video%u'\n", output_device);
        exit(EXIT_FAILURE);
    }

    return file_descriptor;
}

static uint32_t get_v4l2_pixel_fmt(enum Pixel_Format pixel_fmt) {
    switch (pixel_fmt) {
        case NV_12:
            return V4L2_PIX_FMT_NV12;
        case YUV_420:
            return V4L2_PIX_FMT_YUV420;
        case RGB_24:
            return V4L2_PIX_FMT_RGB24;
        case RGBA_444:
            return V4L2_PIX_FMT_RGBA444;

        default:
            exit(EXIT_FAILURE);
    }
}

void set_device_format(int32_t file_descriptor, uint32_t width, uint32_t height, enum Pixel_Format pixel_fmt) {
    struct v4l2_format format;
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

    if (ioctl(file_descriptor, VIDIOC_G_FMT, &format) < 0) {
        // I have no idea what this does. help...
        fprintf(stderr, "Failed to get the format of the v4l2loopback device.\n");
        exit(EXIT_FAILURE);
    }

    format.fmt.pix.width = width;
    format.fmt.pix.height = height;
    format.fmt.pix.pixelformat = get_v4l2_pixel_fmt(pixel_fmt);
    format.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(file_descriptor, VIDIOC_S_FMT, &format) >= 0) return;

    fprintf(stderr, "Failed to set the format of the v4l2loopback device.\n");
    perror("Error");
    fprintf(stderr, "Width: %u, Height: %u\n", width, height);
    close(file_descriptor);
}

void write_frame(int32_t file_descriptor, void **frame, uint32_t size) {
    write(file_descriptor, *frame, size);
}

#endif