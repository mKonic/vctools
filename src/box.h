// A "box" is a headless atrium with its own seat, cursor, keyboard focus, IPC
// socket, D-Bus session and runtime dir. Nothing of it shows on the host, and
// nothing happening inside it can disturb the live session.
#ifndef VC_BOX_H
#define VC_BOX_H

#define VC_MAX_RULES 16

struct vc_box_opts {
	const char *name;
	int width, height;
	double scale;
	int host_input;   // the nested Hyprland's; an atrium box has no host devices
	int decorated;    // the nested Hyprland's; atrium's windows keep their title bars
	int fullscreen;   // fullscreen every window inside the box
	const char *hide; // the nested Hyprland's; an atrium box has no host window
	const char *rules[VC_MAX_RULES]; // extra atrium rules ("app_pattern=foo space=2")
	int n_rules;
};

int vc_box_start(struct vc_box_opts *o);
int vc_box_kill(const char *name);
int vc_box_list(void);
int vc_box_exec(const char *name, char **argv);
int vc_box_env(const char *name);
int vc_box_alive(const char *name);

// Wayland display of a box: its socket's path. NULL if there is no such box.
const char *vc_box_display(const char *name);

#endif
