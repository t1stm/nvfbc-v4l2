#ifndef XRANDR_WRAPPER_H
#define XRANDR_WRAPPER_H

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

typedef struct {
    uint32_t index;

    uint32_t offset_x;
    uint32_t offset_y;

    uint32_t size_w;
    uint32_t size_h;

    uint64_t id;
} X_Screen;

typedef struct {
    X_Screen *screens;
    size_t count;
} X_Data;

inline X_Data get_screens(Display *x_display) {
    int event_base, error_base;

    if (!XRRQueryExtension(x_display, &event_base, &error_base)) {
        fprintf(stderr, "The Xrandr extension is not available.\n");
        exit(EXIT_FAILURE);
    }

    XRRScreenResources* screen_resources = XRRGetScreenResources(x_display, DefaultRootWindow(x_display));
    if (!screen_resources) {
        fprintf(stderr, "Unable to get Xrandr screen resources.\n");
        exit(EXIT_FAILURE);
    }

    const int32_t num_monitors = screen_resources->ncrtc;

    uint32_t valid_monitors = 0;

    // Pass to check valid displays.
    for (int i = 0; i < num_monitors; i++) {
        XRRCrtcInfo *crtc_info = XRRGetCrtcInfo(x_display, screen_resources, screen_resources->crtcs[i]);
        if (!crtc_info || crtc_info->width == 0 && crtc_info->height == 0) {
            continue;
        }

        XRRFreeCrtcInfo(crtc_info);
        ++valid_monitors;
    }

    X_Screen *screens = malloc(sizeof(Screen) * valid_monitors);
    uint32_t current_monitor = 0;

    // Pass to add displays.
    for (int i = 0; i < num_monitors; i++) {
        XRRCrtcInfo *crtc_info = XRRGetCrtcInfo(x_display, screen_resources, screen_resources->crtcs[i]);
        if (!crtc_info || crtc_info->width == 0 && crtc_info->height == 0) {
            continue;
        }

        screens[current_monitor].index = current_monitor;
        screens[current_monitor].offset_x = crtc_info->x;
        screens[current_monitor].offset_y = crtc_info->y;

        screens[current_monitor].size_w = crtc_info->width;
        screens[current_monitor].size_h = crtc_info->height;

        screens[current_monitor].id = *crtc_info->outputs;

        XRRFreeCrtcInfo(crtc_info);
        ++current_monitor;
    }

    XRRFreeScreenResources(screen_resources);

    const X_Data data = {
            .screens = screens,
            .count = valid_monitors
    };

    return data;
}

inline void list_screens(const X_Data x_data) {
    for (int i = 0; i < x_data.count; ++i) {
        const X_Screen screen = x_data.screens[i];

        fprintf(stderr,"Monitor %u:\n", i);
        fprintf(stderr,"  Offset X: %u\n", screen.offset_x);
        fprintf(stderr,"  Offset Y: %u\n", screen.offset_y);
        fprintf(stderr,"  Width: %u\n", screen.size_w);
        fprintf(stderr,"  Height: %u\n", screen.size_h);
        fprintf(stderr,"  Output ID: %lu\n", screen.id);
    }
}

#endif
