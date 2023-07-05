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
    int32_t file_descriptor = open((const char *) device_location, O_WRONLY);
    if (file_descriptor == -1) {
        fprintf(stderr, "Unable to open v4l2_loopback device: '/dev/video%u'\n", output_device);
        exit(EXIT_FAILURE);
    }

    return file_descriptor;
}

void set_device_format(int32_t file_descriptor, uint32_t width, uint32_t height) {
    struct v4l2_format format;
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    format.fmt.pix.width = width;
    format.fmt.pix.height = height;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGBA444;
    format.fmt.pix.field = V4L2_FIELD_NONE;
    format.fmt.pix.priv = 0;

    if (ioctl(file_descriptor, VIDIOC_S_FMT, &format) != -1) return;

    fprintf(stderr, "Failed to set format of the v4l2_loopback device.\n");
    close(file_descriptor);
}

#endif