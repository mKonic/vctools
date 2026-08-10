#define _GNU_SOURCE
#include "wl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "util.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

/* --- toplevels --- */

static void tl_closed(void *data, struct ext_foreign_toplevel_handle_v1 *h) {}
static void tl_done(void *data, struct ext_foreign_toplevel_handle_v1 *h) {}
static void tl_identifier(void *data, struct ext_foreign_toplevel_handle_v1 *h, const char *id) {}

static void tl_title(void *data, struct ext_foreign_toplevel_handle_v1 *h, const char *title) {
	struct vc_toplevel *t = data;
	snprintf(t->title, sizeof t->title, "%s", title);
}

static void tl_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *h, const char *app_id) {
	struct vc_toplevel *t = data;
	snprintf(t->app_id, sizeof t->app_id, "%s", app_id);
}

static const struct ext_foreign_toplevel_handle_v1_listener tl_listener = {
	.closed = tl_closed,
	.done = tl_done,
	.title = tl_title,
	.app_id = tl_app_id,
	.identifier = tl_identifier,
};

static void list_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *list,
                          struct ext_foreign_toplevel_handle_v1 *handle) {
	struct vc_wl *w = data;
	if (w->n_toplevels >= VC_MAX_TOPLEVELS) return;
	struct vc_toplevel *t = &w->toplevels[w->n_toplevels++];
	memset(t, 0, sizeof *t);
	t->handle = handle;
	ext_foreign_toplevel_handle_v1_add_listener(handle, &tl_listener, t);
}

static void list_finished(void *data, struct ext_foreign_toplevel_list_v1 *list) {}

static const struct ext_foreign_toplevel_list_v1_listener list_listener = {
	.toplevel = list_toplevel,
	.finished = list_finished,
};

/* --- outputs --- */

static void out_geometry(void *data, struct wl_output *o, int32_t x, int32_t y, int32_t pw,
                         int32_t ph, int32_t sub, const char *make, const char *model, int32_t tr) {}
static void out_done(void *data, struct wl_output *o) {}
static void out_scale(void *data, struct wl_output *o, int32_t s) {}
static void out_description(void *data, struct wl_output *o, const char *d) {}

static void out_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h,
                     int32_t refresh) {
	struct vc_output *out = data;
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		out->mode_w = w;
		out->mode_h = h;
	}
}

static void out_name(void *data, struct wl_output *o, const char *name) {
	struct vc_output *out = data;
	snprintf(out->name, sizeof out->name, "%s", name);
}

static const struct wl_output_listener output_listener = {
	.geometry = out_geometry,
	.mode = out_mode,
	.done = out_done,
	.scale = out_scale,
	.name = out_name,
	.description = out_description,
};

static void xdg_out_logical_position(void *data, struct zxdg_output_v1 *o, int32_t x, int32_t y) {}
static void xdg_out_done(void *data, struct zxdg_output_v1 *o) {}
static void xdg_out_name(void *data, struct zxdg_output_v1 *o, const char *n) {}
static void xdg_out_description(void *data, struct zxdg_output_v1 *o, const char *d) {}

static void xdg_out_logical_size(void *data, struct zxdg_output_v1 *o, int32_t w, int32_t h) {
	struct vc_output *out = data;
	out->logical_w = w;
	out->logical_h = h;
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
	.logical_position = xdg_out_logical_position,
	.logical_size = xdg_out_logical_size,
	.done = xdg_out_done,
	.name = xdg_out_name,
	.description = xdg_out_description,
};

/* --- registry --- */

static void registry_global(void *data, struct wl_registry *r, uint32_t name, const char *iface,
                            uint32_t version) {
	struct vc_wl *w = data;

	if (!strcmp(iface, wl_shm_interface.name))
		w->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, wl_seat_interface.name) && !w->seat)
		w->seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	else if (!strcmp(iface, wl_output_interface.name)) {
		if (w->n_outputs >= VC_MAX_OUTPUTS) return;
		struct vc_output *out = &w->outputs[w->n_outputs++];
		memset(out, 0, sizeof *out);
		out->output = wl_registry_bind(r, name, &wl_output_interface, version < 4 ? version : 4);
		wl_output_add_listener(out->output, &output_listener, out);
	} else if (!strcmp(iface, zxdg_output_manager_v1_interface.name))
		w->xdg_output_mgr = wl_registry_bind(r, name, &zxdg_output_manager_v1_interface,
		                                     version < 3 ? version : 3);
	else if (!strcmp(iface, ext_foreign_toplevel_list_v1_interface.name))
		w->toplevel_list = wl_registry_bind(r, name, &ext_foreign_toplevel_list_v1_interface, 1);
	else if (!strcmp(iface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name))
		w->window_source_mgr = wl_registry_bind(
			r, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
	else if (!strcmp(iface, ext_output_image_capture_source_manager_v1_interface.name))
		w->output_source_mgr = wl_registry_bind(
			r, name, &ext_output_image_capture_source_manager_v1_interface, 1);
	else if (!strcmp(iface, ext_image_copy_capture_manager_v1_interface.name))
		w->capture_mgr = wl_registry_bind(r, name, &ext_image_copy_capture_manager_v1_interface, 1);
	else if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name))
		w->pointer_mgr = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface,
		                                  version < 2 ? version : 2);
	else if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name))
		w->keyboard_mgr = wl_registry_bind(r, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
}

static void registry_global_remove(void *data, struct wl_registry *r, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

int vc_wl_connect(struct vc_wl *w, const char *display) {
	memset(w, 0, sizeof *w);

	w->dpy = wl_display_connect(display);
	if (!w->dpy) {
		vc_warn("cannot connect to wayland display '%s'", display ? display : getenv("WAYLAND_DISPLAY"));
		return -1;
	}

	w->registry = wl_display_get_registry(w->dpy);
	wl_registry_add_listener(w->registry, &registry_listener, w);
	wl_display_roundtrip(w->dpy); // globals
	wl_display_roundtrip(w->dpy); // wl_output events

	// xdg-output reports the logical size, which is the space pointer motion and
	// window geometry actually use; wl_output's mode is raw pixels and differs
	// from it on any scaled output.
	if (w->xdg_output_mgr) {
		for (int i = 0; i < w->n_outputs; i++) {
			w->outputs[i].xdg_output = zxdg_output_manager_v1_get_xdg_output(
				w->xdg_output_mgr, w->outputs[i].output);
			zxdg_output_v1_add_listener(w->outputs[i].xdg_output, &xdg_output_listener,
			                            &w->outputs[i]);
		}
		wl_display_roundtrip(w->dpy);
	}
	for (int i = 0; i < w->n_outputs; i++) {
		if (!w->outputs[i].logical_w) { // no xdg-output: fall back to the raw mode
			w->outputs[i].logical_w = w->outputs[i].mode_w;
			w->outputs[i].logical_h = w->outputs[i].mode_h;
		}
	}

	if (w->toplevel_list) {
		ext_foreign_toplevel_list_v1_add_listener(w->toplevel_list, &list_listener, w);
		vc_wl_refresh_toplevels(w);
	}
	return 0;
}

void vc_wl_refresh_toplevels(struct vc_wl *w) {
	// One roundtrip announces handles, the second delivers title/app_id.
	wl_display_roundtrip(w->dpy);
	wl_display_roundtrip(w->dpy);
}

struct vc_toplevel *vc_wl_find_toplevel(struct vc_wl *w, const char *match) {
	if (!match || !*match)
		return w->n_toplevels == 1 ? &w->toplevels[0] : NULL;
	for (int i = 0; i < w->n_toplevels; i++)
		if (strstr(w->toplevels[i].title, match)) return &w->toplevels[i];
	for (int i = 0; i < w->n_toplevels; i++)
		if (strstr(w->toplevels[i].app_id, match)) return &w->toplevels[i];
	return NULL;
}

struct vc_output *vc_wl_output(struct vc_wl *w, const char *name) {
	if (!w->n_outputs) return NULL;
	if (!name || !*name) return &w->outputs[0];
	for (int i = 0; i < w->n_outputs; i++)
		if (!strcmp(w->outputs[i].name, name)) return &w->outputs[i];
	return NULL;
}

void vc_wl_disconnect(struct vc_wl *w) {
	if (w->dpy) {
		wl_display_roundtrip(w->dpy);
		wl_display_disconnect(w->dpy);
		w->dpy = NULL;
	}
}
