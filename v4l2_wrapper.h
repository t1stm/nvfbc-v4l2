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
#include <assert.h>

#include "defines.h"

typedef struct {
    uint32_t major;
    uint32_t minor;
    uint32_t bugfix;
} V4L2_Version;

const V4L2_Version NEW_STANDARD_VERSION = {
        .major = 0,
        .minor = 13,
        .bugfix = 2
};

const char *V4L2_LOOPBACK_MODULE_VERSION_LOCATION = "/sys/module/v4l2loopback/version";

int32_t open_device(const int32_t output_device) {
    // /dev/video###
    char device_location[13];
    sprintf(device_location, "/dev/video%u", output_device);
    const int32_t file_descriptor = open((const char *) device_location, O_RDWR);
    if (file_descriptor == -1) {
        fprintf(stderr, "Unable to open v4l2loopback device: '/dev/video%u'\n", output_device);
        exit(EXIT_FAILURE);
    }

    return file_descriptor;
}

static uint32_t get_v4l2_pixel_fmt(const enum Pixel_Format pixel_fmt) {
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

bool read_v4l2_version(V4L2_Version *version) {
    int fd = open(V4L2_LOOPBACK_MODULE_VERSION_LOCATION, O_RDONLY);
    if (fd < 0)
        return false;

    char buffer[32];
    ssize_t bytes_read = read(fd, buffer, sizeof (buffer) - 1);
    close(fd);

    if (bytes_read <= 0)
        return false;

    uint32_t major, minor, bugfix;
    int parsed = sscanf(buffer, "%u.%u.%u",
                        &major,
                        &minor,
                        &bugfix);

    if (parsed != 3)
        return false;

    version->major = major;
    version->minor = minor;
    version->bugfix = bugfix;

    return true;
}

bool is_v4l2_version_new(V4L2_Version *version) {
    V4L2_Version v = *version;
    V4L2_Version standard = NEW_STANDARD_VERSION;

    return
            standard.major >= v.major &&
            (standard.minor == v.minor && standard.bugfix < v.bugfix) ||
            (standard.minor < v.minor);
}

void set_device_format(int32_t file_descriptor, uint32_t width, uint32_t height, enum Pixel_Format pixel_fmt, uint32_t framerate) {
    assert(file_descriptor >= 0);

    struct v4l2_capability capability;
    struct v4l2_format format;
    struct v4l2_streamparm parm;

    V4L2_Version current_version = {};
    if (!read_v4l2_version(&current_version)){
        fprintf(stderr, "Failed to read the v4l2loopback device's version number.\n");
        goto fail_and_log;
    }

    if (ioctl(file_descriptor, VIDIOC_QUERYCAP, &capability) < 0) {
        fprintf(stderr, "Failed to query the v4l2loopback device.\n");
        goto fail_and_log;
    }

    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

    if (ioctl(file_descriptor, VIDIOC_G_FMT, &format) < 0) {
        // I have no idea what this does. help...
        fprintf(stderr, "Failed to get the format of the v4l2loopback device.\n");
        goto fail_and_log;
    }

    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    parm.parm.output.capability = V4L2_CAP_TIMEPERFRAME;
    parm.parm.output.timeperframe.numerator = 1;
    parm.parm.output.timeperframe.denominator = framerate;

    if (ioctl(file_descriptor, VIDIOC_S_PARM, &parm) < 0) {
        fprintf(stderr, "Failed to set the framerate of the v4l2loopback device.\n");
        goto fail_and_log;
    }

    format.fmt.pix.width = width;
    format.fmt.pix.height = height;
    format.fmt.pix.pixelformat = get_v4l2_pixel_fmt(pixel_fmt);

    if (ioctl(file_descriptor, VIDIOC_S_FMT, &format) < 0) {
        fprintf(stderr, "Failed to set the format of the v4l2loopback device.\n");
        goto fail_and_log;
    }

    if (!is_v4l2_version_new(&current_version) && ioctl(file_descriptor, VIDIOC_STREAMON, &parm) < 0) {
        fprintf(stderr, "Failed to start streaming.\n");
        goto fail_and_log;
    }

    return;

    fail_and_log:

    perror("Error");
    close(file_descriptor);
    exit(EXIT_FAILURE);
}

void write_frame(const int32_t file_descriptor, void **frame, const uint32_t size) {
    write(file_descriptor, *frame, size);
    fsync(file_descriptor);
}

#endif