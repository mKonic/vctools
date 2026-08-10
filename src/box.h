// A "box" is a nested Hyprland instance with its own seat, cursor, keyboard
// focus and IPC socket. It renders into a window that vc parks out of sight on
// the host, so everything happening inside it is invisible to — and cannot
// disturb — the live session.
#ifndef VC_BOX_H
#define VC_BOX_H

#define VC_MAX_RULES 16

struct vc_box_opts {
	const char *name;
	int width, height;
	double scale;
	int host_input;   // leave the host-forwarded keyboard/pointer devices enabled
	int decorated;    // keep borders and gaps instead of a window that fills the output
	int fullscreen;   // fullscreen every window inside the box
	const char *hide; // host workspace to park the box window on
	const char *rules[VC_MAX_RULES]; // extra hl.window_rule lines for inside the box
	int n_rules;
};

int vc_box_start(struct vc_box_opts *o);
int vc_box_kill(const char *name);
int vc_box_list(void);
int vc_box_exec(const char *name, char **argv);
int vc_box_env(const char *name);
int vc_box_alive(const char *name);

// Wayland display name for a box, e.g. "vc-ladle". NULL if there is no such box.
const char *vc_box_display(const char *name);
const char *vc_box_signature(const char *name);

#endif
