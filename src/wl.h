// Shared Wayland connection: binds every global vctools needs and resolves the
// output geometry that pointer coordinates are expressed in.
#ifndef VC_WL_H
#define VC_WL_H

#include <stdint.h>
#include <wayland-client.h>

#define VC_MAX_TOPLEVELS 128
#define VC_MAX_OUTPUTS 8

struct vc_toplevel {
	struct ext_foreign_toplevel_handle_v1 *handle;
	char title[512];
	char app_id[256];
};

struct vc_output {
	struct wl_output *output;
	struct zxdg_output_v1 *xdg_output;
	char name[64];
	int mode_w, mode_h;       // pixels the compositor scans out
	int logical_w, logical_h; // the coordinate space input events live in
};

struct vc_wl {
	struct wl_display *dpy;
	struct wl_registry *registry;
	struct wl_shm *shm;
	struct wl_seat *seat;

	struct zxdg_output_manager_v1 *xdg_output_mgr;
	struct vc_output outputs[VC_MAX_OUTPUTS];
	int n_outputs;

	struct ext_foreign_toplevel_list_v1 *toplevel_list;
	struct ext_foreign_toplevel_image_capture_source_manager_v1 *window_source_mgr;
	struct ext_output_image_capture_source_manager_v1 *output_source_mgr;
	struct ext_image_copy_capture_manager_v1 *capture_mgr;

	struct zwlr_virtual_pointer_manager_v1 *pointer_mgr;
	struct zwp_virtual_keyboard_manager_v1 *keyboard_mgr;

	struct vc_toplevel toplevels[VC_MAX_TOPLEVELS];
	int n_toplevels;
};

// `display` may be a WAYLAND_DISPLAY name or NULL to use the environment.
int  vc_wl_connect(struct vc_wl *w, const char *display);
void vc_wl_disconnect(struct vc_wl *w);

// Re-enumerates toplevels; safe to call repeatedly while polling.
void vc_wl_refresh_toplevels(struct vc_wl *w);

// Matches a substring against title first, then app_id. NULL/"" picks the only
// toplevel when there is exactly one.
struct vc_toplevel *vc_wl_find_toplevel(struct vc_wl *w, const char *match);

struct vc_output *vc_wl_output(struct vc_wl *w, const char *name); // NULL = first

#endif
