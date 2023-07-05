/*
 * obs-nvfbc. OBS Studio source plugin.
 *
 * Copyright (C) 2019 Florian Zwoch <fzwoch@gmail.com>
 *
 * This file is part of obs-nvfbc.
 *
 * obs-nvfbc is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * obs-nvfbc is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with obs-nvfbc. If not, see <http://www.gnu.org/licenses/>.
 */

#define GL_GLEXT_PROTOTYPES
#if _WIN32
#define WGL_WGLEXT_PROTOTYPES 1
#endif

#include <obs/obs-config.h>
#include <obs/obs-module.h>
#include <obs/util/threading.h>
#include <obs/util/platform.h>
#include <obs/graphics/graphics.h>
#if !defined(_WIN32) || !_WIN32
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(27, 0, 0)
#include <obs/obs-nix-platform.h>
#endif
#endif

#include "NvFBC.h"

#include <GL/gl.h>
#include <GL/glext.h>
#if _WIN32
#include <GL/wgl.h>
#include <GL/wglext.h>
#else
#include <GL/glx.h>
#include <GL/glxext.h>
#endif

#if !defined(_WIN32) || !_WIN32
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#endif

#include <string.h>
#include <assert.h>

OBS_DECLARE_MODULE()

#if _WIN64
#define NVFBC_LIB_NAME "NvFBC64.dll"
#elif _WIN32
#define NVFBC_LIB_NAME "NvFBC.dll"
#else
#define NVFBC_LIB_NAME "libnvidia-fbc.so.1"
#endif
static void *nvfbc_lib = NULL;
#if _WIN32
static PFNWGLCOPYIMAGESUBDATANVPROC p_wglCopyImageSubDataNV;
#else
static PFNGLXCOPYIMAGESUBDATANVPROC p_glXCopyImageSubDataNV;
#endif

#if !defined(_WIN32) || !_WIN32
Atom _NET_CURRENT_DESKTOP = None;
Atom _NET_NUMBER_OF_DESKTOPS = None;
Atom _NET_DESKTOP_NAMES = None;
Atom UTF8_STRING = None;
#endif

static NVFBC_API_FUNCTION_LIST nvFBC = {
	.dwVersion = NVFBC_VERSION
};

typedef struct {
	obs_source_t *source;
} data_obs_t;

typedef struct {
	int screen;
	bool show_cursor;
	int fps;
	bool push_model;
	bool direct_capture;
#if !defined(_WIN32) || !_WIN32
	long desktop;
#endif
} data_settings_t;

typedef struct {
	pthread_mutex_t session_mutex;
	NVFBC_SESSION_HANDLE nvfbc_session;
#if _WIN32
	HGLRC nvfbc_ctx;
#else
	GLXContext nvfbc_ctx;
#endif
	bool has_capture_session;
	NVFBC_TOGL_SETUP_PARAMS togl_setup_params;
} data_nvfbc_t;

typedef struct {
	pthread_mutex_t texture_mutex;
	uint32_t width, height;
	gs_texture_t *texture;
} data_texture_t;

#if !defined(_WIN32) || !_WIN32
typedef struct {
	Display *dpy;
	int visible_transition;
} data_x11_t;
#endif

typedef struct {
	data_obs_t obs;
	data_settings_t settings;
	data_nvfbc_t nvfbc;
	data_texture_t tex;
#if !defined(_WIN32) || !_WIN32
	data_x11_t x11;
#endif
} data_t;

static const char* get_name(void *type_data)
{
	return "NvFBC Source";
}

static bool create_nvfbc_session(data_nvfbc_t *data_nvfbc)
{
	if (data_nvfbc->nvfbc_session != -1) {
		return true;
	}

	NVFBC_CREATE_HANDLE_PARAMS params = {
		.dwVersion = NVFBC_CREATE_HANDLE_PARAMS_VER,
		.bExternallyManagedContext = NVFBC_FALSE
	};

	NVFBCSTATUS ret = nvFBC.nvFBCCreateHandle(&data_nvfbc->nvfbc_session, &params);
	if (ret != NVFBC_SUCCESS) {
		blog(LOG_ERROR, "%s", nvFBC.nvFBCGetLastErrorStr(data_nvfbc->nvfbc_session));
		data_nvfbc->nvfbc_session = -1;
		goto create_handle_err;
	}

	data_nvfbc->has_capture_session = false;

#if _WIN32
	data_nvfbc->nvfbc_ctx = wglGetCurrentContext();
#else
	data_nvfbc->nvfbc_ctx = glXGetCurrentContext();
#endif
	if (data_nvfbc->nvfbc_ctx == NULL) {
		blog(LOG_ERROR, "%s", "Could not get NvFBC OpenGL context");
		goto get_ctx_failed;
	}

	return true;

get_ctx_failed:;
	NVFBC_DESTROY_HANDLE_PARAMS destroy_params = {
		.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER
	};
	ret = nvFBC.nvFBCDestroyHandle(data_nvfbc->nvfbc_session, &destroy_params);
	if (ret != NVFBC_SUCCESS) {
		blog(LOG_WARNING, "%s", nvFBC.nvFBCGetLastErrorStr(data_nvfbc->nvfbc_session));
	}
	data_nvfbc->nvfbc_session = -1;
create_handle_err:;
	return false;
}

static void destroy_nvfbc_session(data_nvfbc_t *data_nvfbc)
{
	if (data_nvfbc->nvfbc_session == -1) {
		return;
	}

	NVFBC_DESTROY_HANDLE_PARAMS destroy_params = {
		.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER
	};

	NVFBCSTATUS ret = nvFBC.nvFBCDestroyHandle(data_nvfbc->nvfbc_session, &destroy_params);
	if (ret != NVFBC_SUCCESS) {
		blog(LOG_WARNING, "%s", nvFBC.nvFBCGetLastErrorStr(data_nvfbc->nvfbc_session));
	}

	data_nvfbc->has_capture_session = false;
	data_nvfbc->nvfbc_session = -1;
}

static bool enter_nvfbc_context(data_nvfbc_t *data_nvfbc)
{
	if (data_nvfbc->nvfbc_session == -1) {
		return false;
	}

	NVFBC_BIND_CONTEXT_PARAMS bind_context_params = {
		.dwVersion = NVFBC_BIND_CONTEXT_PARAMS_VER
	};

	NVFBCSTATUS ret = nvFBC.nvFBCBindContext(data_nvfbc->nvfbc_session, &bind_context_params);
	if (ret != NVFBC_SUCCESS) {
		blog(LOG_ERROR, "%s", nvFBC.nvFBCGetLastErrorStr(data_nvfbc->nvfbc_session));
		return false;
	}

	return true;
}

static void leave_nvfbc_context(data_nvfbc_t *data_nvfbc)
{
	if (data_nvfbc->nvfbc_session == -1) {
		return;
	}

	NVFBC_RELEASE_CONTEXT_PARAMS release_context_params = {
		.dwVersion = NVFBC_RELEASE_CONTEXT_PARAMS_VER
	};

	NVFBCSTATUS ret = nvFBC.nvFBCReleaseContext(data_nvfbc->nvfbc_session, &release_context_params);
	if (ret != NVFBC_SUCCESS) {
		blog(LOG_WARNING, "%s", nvFBC.nvFBCGetLastErrorStr(data_nvfbc->nvfbc_session));
	}
}

static bool get_nvfbc_status(NVFBC_SESSION_HANDLE session, NVFBC_GET_STATUS_PARAMS *status_params)
{
	bool create_session = session == -1;
	NVFBCSTATUS ret;

	if (create_session) {
		NVFBC_CREATE_HANDLE_PARAMS params = {
			.dwVersion = NVFBC_CREATE_HANDLE_PARAMS_VER,
			.bExternallyManagedContext = NVFBC_FALSE
		};

		ret = nvFBC.nvFBCCreateHandle(&session, &params);
		if (ret != NVFBC_SUCCESS) {
			blog(LOG_ERROR, "%s", nvFBC.nvFBCGetLastErrorStr(session));
			return false;
		}
	}

	ret = nvFBC.nvFBCGetStatus(session, status_params);
	bool ret2 = ret == NVFBC_SUCCESS;
	if (!ret2) {
		blog(LOG_ERROR, "%s", nvFBC.nvFBCGetLastErrorStr(session));
	}

	if (create_session) {
		NVFBC_DESTROY_HANDLE_PARAMS destroy_params = {
			.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER
		};

		ret = nvFBC.nvFBCDestroyHandle(session, &destroy_params);
		if (ret != NVFBC_SUCCESS) {
			blog(LOG_WARNING, "%s", nvFBC.nvFBCGetLastErrorStr(session));
		}
	}

	return ret2;
}

static bool create_capture_session(data_nvfbc_t *data_nvfbc, data_settings_t *settings)
{
	if (data_nvfbc->has_capture_session) {
		return false;
	}

	NVFBC_CREATE_CAPTURE_SESSION_PARAMS cap_params = {
		.dwVersion = NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER,
		.eCaptureType = NVFBC_CAPTURE_TO_GL,
		.eTrackingType = settings->screen == -1 ? NVFBC_TRACKING_SCREEN : NVFBC_TRACKING_OUTPUT,
		.dwOutputId = settings->screen,
		.bWithCursor = settings->show_cursor ? NVFBC_TRUE : NVFBC_FALSE,
		.bDisableAutoModesetRecovery = NVFBC_FALSE,
		.bRoundFrameSize = NVFBC_TRUE,
		.dwSamplingRateMs = 1000.0 / settings->fps + 0.5,
		.bPushModel = settings->push_model ? NVFBC_TRUE : NVFBC_FALSE,
		.bAllowDirectCapture = settings->direct_capture ? NVFBC_TRUE : NVFBC_FALSE,
	};

	NVFBCSTATUS ret = nvFBC.nvFBCCreateCaptureSession(data_nvfbc->nvfbc_session, &cap_params);
	if (ret != NVFBC_SUCCESS) {
		blog(LOG_ERROR, "%s", nvFBC.nvFBCGetLastErrorStr(data_nvfbc->nvfbc_session));
		return false;
	}

	data_nvfbc->togl_setup_params = (NVFBC_TOGL_SETUP_PARAMS){
		.dwVersion = NVFBC_TOGL_SETUP_PARAMS_VER,
		.eBufferFormat = NVFBC_BUFFER_FORMAT_RGBA,
		.bWithDiffMap = NVFBC_FALSE,
		.dwDiffMapScalingFactor = 1
	};

	ret = nvFBC.nvFBCToGLSetUp(data_nvfbc->nvfbc_session, &data_nvfbc->togl_setup_params);
	if (ret != NVFBC_SUCCESS) {
		blog(LOG_ERROR, "%s", nvFBC.nvFBCGetLastErrorStr(data_nvfbc->nvfbc_session));
		goto setup_error;
	}

	data_nvfbc->has_capture_session = true;

	return true;

setup_error:;
	NVFBC_DESTROY_CAPTURE_SESSION_PARAMS destroy_cap_params = {
		.dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER,
	};

	ret = nvFBC.nvFBCDestroyCaptureSession(data_nvfbc->nvfbc_session, &destroy_cap_params);
	if (ret != NVFBC_SUCCESS) {
		blog(LOG_WARNING, "%s", nvFBC.nvFBCGetLastErrorStr(data_nvfbc->nvfbc_session));
	}

	return false;
}

static void destroy_capture_session(data_nvfbc_t *data_nvfbc)
{
	if (!data_nvfbc->has_capture_session) {
		return;
	}

	NVFBC_DESTROY_CAPTURE_SESSION_PARAMS destroy_cap_params = {
		.dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER
	};

	NVFBCSTATUS ret = nvFBC.nvFBCDestroyCaptureSession(data_nvfbc->nvfbc_session, &destroy_cap_params);
	if (ret != NVFBC_SUCCESS) {
		blog(LOG_WARNING, "%s", nvFBC.nvFBCGetLastErrorStr(data_nvfbc->nvfbc_session));
	}

	data_nvfbc->has_capture_session = false;
}

static bool switch_to_nvfbc_context(data_nvfbc_t *data_nvfbc)
{
	if (data_nvfbc->nvfbc_session == -1) {
		return false;
	}

	obs_leave_graphics();

	if (!enter_nvfbc_context(data_nvfbc)) {
		obs_enter_graphics();
		return false;
	}

	return true;
}

static void switch_to_obs_context(data_nvfbc_t *data_nvfbc)
{
	leave_nvfbc_context(data_nvfbc);

	obs_enter_graphics();
}

static bool capture_frame(data_nvfbc_t *data_nvfbc, GLuint *out_texture, NVFBC_FRAME_GRAB_INFO *out_info)
{
	if (!data_nvfbc->has_capture_session) {
		return false;
	}

	NVFBC_TOGL_GRAB_FRAME_PARAMS grab_params = {
		.dwVersion = NVFBC_TOGL_GRAB_FRAME_PARAMS_VER,
		.dwFlags = NVFBC_TOGL_GRAB_FLAGS_NOWAIT,
		.pFrameGrabInfo = out_info,
		.dwTimeoutMs = 0
	};

	NVFBCSTATUS ret = nvFBC.nvFBCToGLGrabFrame(data_nvfbc->nvfbc_session, &grab_params);
	if (ret != NVFBC_SUCCESS) {
		blog(LOG_ERROR, "%s", nvFBC.nvFBCGetLastErrorStr(data_nvfbc->nvfbc_session));
		return false;
	}

	*out_texture = data_nvfbc->togl_setup_params.dwTextures[grab_params.dwTextureIndex];

	return true;
}

static bool need_texture_resize(data_texture_t *data_texture, uint32_t width, uint32_t height)
{
	return data_texture->texture == NULL || width != data_texture->width || height != data_texture->height;
}

static bool resize_texture(data_texture_t *data_texture, uint32_t width, uint32_t height)
{
	if (data_texture->texture != NULL) {
		gs_texture_destroy(data_texture->texture);
	}

	data_texture->texture = gs_texture_create(width, height, GS_RGBA, 1, NULL, GS_DYNAMIC);
	if (data_texture->texture == NULL) {
		return false;
	}

	/* HACK: OBS's graphics api doesn't support creating a texture with internal format GL_RGBA8,
		which is required by *CopyImageSubDataNV because the NvFBC texture has that format. */
	GLuint new_tex;
	glGenTextures(1, &new_tex);
	glBindTexture(GL_TEXTURE_2D, new_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);
	GLuint old_tex = *(GLuint*)gs_texture_get_obj(data_texture->texture);
	*(GLuint*)gs_texture_get_obj(data_texture->texture) = new_tex;
	glDeleteTextures(1, &old_tex);

	data_texture->width = width;
	data_texture->height = height;

	return true;
}

#if !defined(_WIN32) || !_WIN32
static long get_current_desktop(Display *dpy)
{
	Atom type;
	int format;
	unsigned long count, remaining;
	unsigned char *data;
	int status = XGetWindowProperty(dpy, DefaultRootWindow(dpy), _NET_CURRENT_DESKTOP, 0, 1, False, XA_CARDINAL, &type, &format, &count, &remaining, &data);
	if (status != Success) {
		return -1;
	}
	if (type != XA_CARDINAL || format != 32 || count != 1 || remaining != 0) {
		XFree(data);
		return -1;
	}
	long ret = *(long*)data;
	XFree(data);
	return ret;
}

static bool is_desktop_visible(data_t *data)
{
	if (data->settings.desktop == -1) {
		return true;
	}

	long current_desktop = get_current_desktop(data->x11.dpy);
	return current_desktop >= 0 && current_desktop == data->settings.desktop;
}
#endif

static bool update_texture(data_t *data)
{
	int error = pthread_mutex_lock(&data->nvfbc.session_mutex);
	if (error != 0) {
		blog(LOG_ERROR, "Mutex lock error: %s", strerror(error));
		goto nvfbc_lock_err;
	}

	if (!data->nvfbc.has_capture_session) {
		goto no_capture_session;
	}

#if !defined(_WIN32) || !_WIN32
	/* Check desktop here to avoid capturing the desktop if not necessary. */
	if (!is_desktop_visible(data)) {
		data->x11.visible_transition = (data->settings.fps + 5) / -6;
		goto desktop_hidden;
	}
#endif

	if (!switch_to_nvfbc_context(&data->nvfbc)) {
		goto enter_ctx_failed;
	}

	GLuint nvfbc_tex;
	NVFBC_FRAME_GRAB_INFO info;

	if (!capture_frame(&data->nvfbc, &nvfbc_tex, &info)) {
		goto capture_frame_err;
	}

	switch_to_obs_context(&data->nvfbc);

#if !defined(_WIN32) || !_WIN32
	/* Wait a few frames to make sure the old desktop is never visible. */
	if (data->x11.visible_transition + 1 < 0) {
		if (info.bIsNewFrame == NVFBC_TRUE) {
			++data->x11.visible_transition;
		}
		goto desktop_hidden;
	}
	if (data->x11.visible_transition + 1 == 0) {
		if (info.bIsNewFrame == NVFBC_FALSE) {
			goto desktop_hidden;
		}
		++data->x11.visible_transition;
	}

	/* Need to check again to avoid a "race condition". */
	if (!is_desktop_visible(data)) {
		data->x11.visible_transition = (data->settings.fps + 5) / -6;
		goto desktop_hidden;
	}
#endif

	error = pthread_mutex_lock(&data->tex.texture_mutex);
	if (error != 0) {
		blog(LOG_ERROR, "Mutex lock error: %s", strerror(error));
		goto tex_lock_err;
	}

	if (need_texture_resize(&data->tex, info.dwWidth, info.dwHeight)) {
		if (!resize_texture(&data->tex, info.dwWidth, info.dwHeight)) {
			goto tex_create_failed;
		}
	} else if (!info.bIsNewFrame) {
		goto omit_tex_copy;
	}

#if _WIN32
	p_wglCopyImageSubDataNV(
#else
	p_glXCopyImageSubDataNV(
		data->x11.dpy,
#endif
		data->nvfbc.nvfbc_ctx, nvfbc_tex, data->nvfbc.togl_setup_params.dwTexTarget, 0, 0, 0, 0,
		NULL, *(GLuint*)gs_texture_get_obj(data->tex.texture), GL_TEXTURE_2D, 0, 0, 0, 0,
		info.dwWidth, info.dwHeight, 1
	);

	error = pthread_mutex_unlock(&data->nvfbc.session_mutex);
	assert(error == 0);

	GLenum glerr = glGetError();
	if (glerr != GL_NO_ERROR) {
#if _WIN32
		blog(LOG_ERROR, "wglCopyImageSubDataNV GL error: %x", glerr);
#else
		blog(LOG_ERROR, "glXCopyImageSubDataNV GL error: %x", glerr);
#endif
		goto tex_copy_failed;
	}

	error = pthread_mutex_unlock(&data->tex.texture_mutex);
	assert(error == 0);

	return true;

omit_tex_copy:;
	error = pthread_mutex_unlock(&data->nvfbc.session_mutex);
	assert(error == 0);
	error = pthread_mutex_unlock(&data->tex.texture_mutex);
	assert(error == 0);
	return true;

#if !defined(_WIN32) || !_WIN32
desktop_hidden:;
	error = pthread_mutex_unlock(&data->nvfbc.session_mutex);
	assert(error == 0);
	return true;
#endif

tex_copy_failed:;
tex_create_failed:;
	error = pthread_mutex_unlock(&data->tex.texture_mutex);
	assert(error == 0);
tex_lock_err:;
	goto enter_ctx_failed;
capture_frame_err:;
	switch_to_obs_context(&data->nvfbc);
enter_ctx_failed:;
no_capture_session:;
	error = pthread_mutex_unlock(&data->nvfbc.session_mutex);
	assert(error == 0);
nvfbc_lock_err:;
	return false;
}

static void copy_settings(data_settings_t *settings, obs_data_t *obs_settings)
{
	settings->screen = obs_data_get_int(obs_settings, "screen");
	settings->show_cursor = obs_data_get_bool(obs_settings, "show_cursor");
	settings->fps = obs_data_get_int(obs_settings, "fps");
	settings->push_model = obs_data_get_bool(obs_settings, "push_model");
	settings->direct_capture = obs_data_get_bool(obs_settings, "direct_capture");
#if !defined(_WIN32) || !_WIN32
	settings->desktop = obs_data_get_int(obs_settings, "desktop");
#endif
}

static void* create(obs_data_t *settings, obs_source_t *source)
{
	obs_enter_graphics();
	if (gs_get_device_type() != GS_DEVICE_OPENGL) {
		blog(LOG_ERROR, "%s", "This plugin requires an OpenGL context");
		goto not_opengl_err;
	}

	Display *dpy = glXGetCurrentDisplay();
	obs_leave_graphics();

	data_t *data = bzalloc(sizeof(data_t));
	if (data == NULL) {
		blog(LOG_ERROR, "%s", "Out of memory");
		goto alloc_err;
	}

	data->obs.source = source;
	copy_settings(&data->settings, settings);
	data->nvfbc.nvfbc_session = -1;
#if !defined(_WIN32) || !_WIN32
	data->x11.dpy = dpy;
#endif

	int error = pthread_mutex_init(&data->nvfbc.session_mutex, NULL);
	if (error != 0) {
		blog(LOG_ERROR, "Mutex initialization error: %s", strerror(error));
		goto sess_mutex_err;
	}

	error = pthread_mutex_init(&data->tex.texture_mutex, NULL);
	if (error != 0) {
		blog(LOG_ERROR, "Mutex initialization error: %s", strerror(error));
		goto tex_mutex_err;
	}

	if (!create_nvfbc_session(&data->nvfbc)) {
		goto nvfbc_err;
	}
	leave_nvfbc_context(&data->nvfbc);

	return data;

nvfbc_err:;
	pthread_mutex_destroy(&data->tex.texture_mutex);
tex_mutex_err:;
	pthread_mutex_destroy(&data->nvfbc.session_mutex);
sess_mutex_err:;
	bfree(data);
alloc_err:;
#if !defined(_WIN32) || !_WIN32
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(27, 0, 0)
not_glx_err:;
#endif
#endif
not_opengl_err:;
	return NULL;
}

static void destroy(void *p)
{
	data_t *data = p;

	pthread_mutex_lock(&data->tex.texture_mutex);

	if (data->tex.texture != NULL) {
		gs_texture_destroy(data->tex.texture);
	}

	pthread_mutex_lock(&data->nvfbc.session_mutex);

	if (data->nvfbc.nvfbc_session != -1) {
		while (!enter_nvfbc_context(&data->nvfbc));
		destroy_capture_session(&data->nvfbc);
		destroy_nvfbc_session(&data->nvfbc);
	}

	pthread_mutex_destroy(&data->tex.texture_mutex);
	pthread_mutex_destroy(&data->nvfbc.session_mutex);

	bfree(data);
}

static void render(void *p, gs_effect_t *effect)
{
	data_t *data = p;

	if (!update_texture(data)) {
		goto tex_upd_err;
	}

	if (!data->tex.texture) {
		goto no_texture;
	}

	effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);
	gs_blend_state_push();
	gs_reset_blend_state();

	int error = pthread_mutex_lock(&data->tex.texture_mutex);
	if (error != 0) {
		blog(LOG_ERROR, "Mutex lock error: %s", strerror(error));
		goto tex_lock_err;
	}

	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	if (image == NULL) {
		blog(LOG_ERROR, "Effect image parameter not found");
		goto no_image;
	}
	gs_effect_set_texture(image, data->tex.texture);

	while (gs_effect_loop(effect, "Draw")) {
		gs_draw_sprite(data->tex.texture, 0, 0, 0);
	}

	error = pthread_mutex_unlock(&data->tex.texture_mutex);
	assert(error == 0);

	gs_blend_state_pop();

	return;

no_image:;
	error = pthread_mutex_unlock(&data->tex.texture_mutex);
	assert(error == 0);
tex_lock_err:;
	gs_blend_state_pop();
no_texture:;
tex_upd_err:;
	return;
}

uint32_t get_width(void *p)
{
	data_t *data = p;

	return data->tex.width;
}

uint32_t get_height(void *p)
{
	data_t *data = p;

	return data->tex.height;
}

static void get_defaults(obs_data_t *settings)
{
	NVFBC_GET_STATUS_PARAMS status_params = {
		.dwVersion = NVFBC_GET_STATUS_PARAMS_VER
	};

	if (get_nvfbc_status(-1, &status_params)) {
		obs_data_set_default_int(settings, "screen", status_params.outputs[0].dwId);
	}

	obs_data_set_default_int(settings, "fps", 60);
	obs_data_set_default_bool(settings, "show_cursor", true);
	obs_data_set_default_bool(settings, "push_model", true);
	obs_data_set_default_bool(settings, "direct_capture", false);
#if !defined(_WIN32) || !_WIN32
	obs_data_set_default_int(settings, "desktop", -1);
#endif
}

#if !defined(_WIN32) || !_WIN32
static long get_desktop_count(Display *dpy)
{
	Atom type;
	int format;
	unsigned long count, remaining;
	unsigned char *data;
	int status = XGetWindowProperty(dpy, DefaultRootWindow(dpy), _NET_NUMBER_OF_DESKTOPS, 0, 1, False, XA_CARDINAL, &type, &format, &count, &remaining, &data);
	if (status != Success) {
		return -1;
	}
	if (type != XA_CARDINAL || format != 32 || count != 1 || remaining != 0) {
		XFree(data);
		return -1;
	}
	long ret = *(long*)data;
	XFree(data);
	return ret;
}

static unsigned long get_desktop_names(Display *dpy, char **names)
{
	if (_NET_DESKTOP_NAMES == None || UTF8_STRING == None) {
		return 0;
	}
	Atom type;
	int format;
	unsigned long count, remaining;
	unsigned char *data;
	int status = XGetWindowProperty(dpy, DefaultRootWindow(dpy), _NET_DESKTOP_NAMES, 0, (long)((unsigned long)-1 >> 1), False, UTF8_STRING, &type, &format, &count, &remaining, &data);
	if (status != Success) {
		return 0;
	}
	if (type != UTF8_STRING || format != 8 || remaining != 0) {
		XFree(data);
		return 0;
	}
	*names = (char *)data;
	return count;
}

static void free_desktop_names(char *names)
{
	if (names) {
		XFree(names);
	}
}
#endif

static obs_properties_t* get_properties(void *p)
{
	data_t *data = p;

	obs_properties_t *props = obs_properties_create();
	if (props == NULL) {
		goto props_create_err;
	}

	NVFBC_GET_STATUS_PARAMS status_params = {
		.dwVersion = NVFBC_GET_STATUS_PARAMS_VER
	};
	bool status_valid = false;

	int error = pthread_mutex_lock(&data->nvfbc.session_mutex);
	if (error != 0) {
		goto create_context;
	}

	if (!enter_nvfbc_context(&data->nvfbc)) {
		goto enter_ctx_failed;
	}
	status_valid = get_nvfbc_status(data->nvfbc.nvfbc_session, &status_params);
	leave_nvfbc_context(&data->nvfbc);

	error = pthread_mutex_unlock(&data->nvfbc.session_mutex);
	assert(error == 0);

	goto screen_list;

enter_ctx_failed:;
	error = pthread_mutex_unlock(&data->nvfbc.session_mutex);
	assert(error == 0);
create_context:;
	status_valid = get_nvfbc_status(-1, &status_params);

screen_list:;
	obs_property_t *prop = obs_properties_add_list(props, "screen", "Screen", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	if (prop == NULL) {
		goto screen_lst_alloc_err;
	}

	if (status_valid) {
		obs_property_list_add_int(prop, "Entire Desktop", -1);
		for (int i = 0; i < status_params.dwOutputNum; i++) {
			obs_property_list_add_int(prop, status_params.outputs[i].name, status_params.outputs[i].dwId);
		}
	}

	obs_properties_add_int(props, "fps", "FPS", 1, 120, 1);
	obs_properties_add_bool(props, "show_cursor", "Cursor");
	obs_properties_add_bool(props, "push_model", "Use Push Model");
	obs_properties_add_bool(props, "direct_capture", "Use Direct Capture");

#if !defined(_WIN32) || !_WIN32
	prop = obs_properties_add_list(props, "desktop", "Desktop", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	if (prop == NULL) {
		goto screen_lst_alloc_err;
	}

	obs_property_list_add_int(prop, "All Desktops", -1);
	Display *dpy = data->x11.dpy;
	long desktop_count;
	if (_NET_CURRENT_DESKTOP != None && _NET_NUMBER_OF_DESKTOPS != None
			&& (desktop_count = get_desktop_count(dpy)) > 0 && get_current_desktop(dpy) >= 0) {
		char *desktop_names = NULL;
		long desktop_names_len = get_desktop_names(dpy, &desktop_names);
		char *p = desktop_names;

		for (int i = 0; i < desktop_count; i++) {
			size_t remaining_len;
			if (p) {
				remaining_len = desktop_names_len - (p - desktop_names);
				if (!remaining_len) {
					p = NULL;
				}
			}

			char text[64];
			if (p && *p) {
				snprintf(text, sizeof(text), "Desktop %i (%.*s)", i + 1, (int)remaining_len, p);
			} else {
				snprintf(text, sizeof(text), "Desktop %i", i + 1);
			}
			obs_property_list_add_int(prop, text, i);

			if (p) {
				p = memchr(p, '\0', remaining_len) + 1;
			}
		}

		free_desktop_names(desktop_names);
	} else {
		obs_property_set_enabled(prop, false);
	}
#endif

	return props;

screen_lst_alloc_err:;
	obs_properties_destroy(props);
props_create_err:;
	return NULL;
}

static void show(void *p)
{
	data_t *data = p;

	int error = pthread_mutex_lock(&data->nvfbc.session_mutex);
	if (error != 0) {
		blog(LOG_ERROR, "Mutex lock error: %s", strerror(error));
		return;
	}

	if (enter_nvfbc_context(&data->nvfbc)) {
		create_capture_session(&data->nvfbc, &data->settings);
		leave_nvfbc_context(&data->nvfbc);
	}

	error = pthread_mutex_unlock(&data->nvfbc.session_mutex);
	assert(error == 0);
}

static void hide(void *p)
{
	data_t *data = p;

	int error = pthread_mutex_lock(&data->nvfbc.session_mutex);
	if (error != 0) {
		blog(LOG_ERROR, "Mutex lock error: %s", strerror(error));
		return;
	}

	if (enter_nvfbc_context(&data->nvfbc)) {
		destroy_capture_session(&data->nvfbc);
		leave_nvfbc_context(&data->nvfbc);
	}

	error = pthread_mutex_unlock(&data->nvfbc.session_mutex);
	assert(error == 0);
}

static void update(void *p, obs_data_t *settings)
{
	data_t *data = p;

	int error = pthread_mutex_lock(&data->nvfbc.session_mutex);
	if (error != 0) {
		blog(LOG_ERROR, "Mutex lock error: %s", strerror(error));
		return;
	}

	if (enter_nvfbc_context(&data->nvfbc)) {
		destroy_capture_session(&data->nvfbc);
		copy_settings(&data->settings, settings);
		create_capture_session(&data->nvfbc, &data->settings);
		leave_nvfbc_context(&data->nvfbc);
	}

	error = pthread_mutex_unlock(&data->nvfbc.session_mutex);
	assert(error == 0);
}

struct obs_source_info nvfbc_source = {
	.id = "nvfbc-source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.get_name = get_name,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE,

	.create = create,
	.destroy = destroy,
	.video_render = render,
	.get_width = get_width,
	.get_height = get_height,

	.get_defaults = get_defaults,
	.get_properties = get_properties,
	.show = show,
	.hide = hide,
	.update = update,
};

static bool check_ext_in_string(const char *str, const char *name)
{
	for (const char *space; (space = strchr(str, ' ')); str = space + 1) {
		if (!strncmp(str, name, space - str)) {
			return true;
		}
	}
	return !strcmp(str, name);
}

static bool check_platform_ext_available(const char *name)
{
	const char *extensions;

#if _WIN32
	PFNWGLGETEXTENSIONSSTRINGARBPROC p_wglGetExtensionsStringARB = (PFNWGLGETEXTENSIONSSTRINGARBPROC)wglGetProcAddress("wglGetExtensionsStringARB");
	if (!p_wglGetExtensionsStringARB) {
		blog(LOG_ERROR, "wglGetExtensionsStringARB function not available");
		return false;
	}

	HDC hDC = wglGetCurrentDC();
	if (!hDC) {
		blog(LOG_ERROR, "Cound not get DC from current OpenGL context");
		return false;
	}
	extensions = p_wglGetExtensionsStringARB ? p_wglGetExtensionsStringARB(hDC) : NULL;
#else
	Display *dpy = glXGetCurrentDisplay();
	if (!dpy) {
		blog(LOG_ERROR, "Cound not get X11 Display from current OpenGL context");
		return false;
	}
	extensions = glXQueryExtensionsString(dpy, DefaultScreen(dpy));
#endif

	if (!extensions) {
		blog(LOG_ERROR, "Cound not get OpenGL platform extensions string");
		return false;
	}
	return check_ext_in_string(extensions, name);
}

static bool check_fallback_ext_available(const char *name)
{
	GLint ext_count = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &ext_count);
	if (!ext_count) {
		const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
		if (!extensions) {
			blog(LOG_ERROR, "Cound not get OpenGL extensions string");
			return false;
		}
		return check_ext_in_string(extensions, name);
	}
	for (GLint i = 0; i < ext_count; ++i) {
		const GLubyte *ext = glGetStringi(GL_EXTENSIONS, i);
		if (!ext) {
			blog(LOG_ERROR, "Cound not get OpenGL extension #%i", i);
		}
		else if (!strcmp((const char *)ext, name)) {
			return true;
		}
	}
	return false;
}

static bool check_ext_available(const char *name)
{
	return check_platform_ext_available(name)
		|| check_fallback_ext_available(name);
}

bool obs_module_load(void)
{
	if (obs_get_version() >> 24 >= 28) {
		blog(LOG_ERROR, "NvFBC is defunct on OBS version >= 28. Abort.");
		return false;
	}

	PNVFBCCREATEINSTANCE p_NvFBCCreateInstance = NULL;

	nvfbc_lib = os_dlopen(NVFBC_LIB_NAME);
	if (nvfbc_lib == NULL) {
		blog(LOG_ERROR, "%s", "Unable to load NvFBC library");
		goto error;
	}

	p_NvFBCCreateInstance = (PNVFBCCREATEINSTANCE)os_dlsym(nvfbc_lib, "NvFBCCreateInstance");
	if (p_NvFBCCreateInstance == NULL) {
		blog(LOG_ERROR, "%s", "Unable to find NvFBCCreateInstance symbol in NvFBC library");
		goto error;
	}

	NVFBCSTATUS ret = p_NvFBCCreateInstance(&nvFBC);
	if (ret != NVFBC_SUCCESS) {
		blog(LOG_ERROR, "%s", "Unable to create NvFBC instance");
		goto error;
	}

	obs_enter_graphics();

#if !defined(_WIN32) || !_WIN32
	_NET_CURRENT_DESKTOP = XInternAtom(glXGetCurrentDisplay(), "_NET_CURRENT_DESKTOP", False);
	_NET_NUMBER_OF_DESKTOPS = XInternAtom(glXGetCurrentDisplay(), "_NET_NUMBER_OF_DESKTOPS", False);
	_NET_DESKTOP_NAMES = XInternAtom(glXGetCurrentDisplay(), "_NET_DESKTOP_NAMES", False);
	UTF8_STRING = XInternAtom(glXGetCurrentDisplay(), "UTF8_STRING", False);
#endif

#if _WIN32
	if (!check_ext_available("WGL_NV_copy_image")) {
		blog(LOG_ERROR, "%s", "OpenGL extension WGL_NV_copy_image not supported");
		goto ext_error;
	}
	p_wglCopyImageSubDataNV = (PFNWGLCOPYIMAGESUBDATANVPROC)wglGetProcAddress("wglCopyImageSubDataNV");
	if (p_wglCopyImageSubDataNV == NULL) {
		blog(LOG_ERROR, "%s", "Failed getting address of wglCopyImageSubDataNV function");
		goto ext_error;
	}
#else
	if (!check_ext_available("GLX_NV_copy_image")) {
		blog(LOG_ERROR, "%s", "OpenGL extension GLX_NV_copy_image not supported");
		goto ext_error;
	}
	p_glXCopyImageSubDataNV = (PFNGLXCOPYIMAGESUBDATANVPROC)glXGetProcAddress((const GLubyte*)"glXCopyImageSubDataNV");
	if (p_glXCopyImageSubDataNV == NULL) {
		blog(LOG_ERROR, "%s", "Failed getting address of glXCopyImageSubDataNV function");
		goto ext_error;
	}
#endif

	obs_leave_graphics();

	obs_register_source(&nvfbc_source);

	return true;

ext_error:;
	obs_leave_graphics();
error:;
	if (nvfbc_lib != NULL) {
		os_dlclose(nvfbc_lib);
		nvfbc_lib = NULL;
	}
	return false;
}

void obs_module_unload(void)
{
	if (nvfbc_lib != NULL) {
		os_dlclose(nvfbc_lib);
		nvfbc_lib = NULL;
	}
}
