#define _GNU_SOURCE
#include "box.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "input.h"
#include "script.h"
#include "util.h"
#include "wl.h"

const char *vc_box_display(const char *name) {
	return vc_state_get(name, "display");
}

int vc_box_alive(const char *name) {
	int pid = vc_state_get_int(name, "pid", 0);
	return pid > 0 && kill(pid, 0) == 0;
}

/* --- the box's own corner of the machine ------------------------------- */

// A box is a headless atrium beside the user's session: no window on the host
// to hide, no host seat mirrored in. What keeps it from touching that session
// is where it lives: its own runtime dir (its Wayland and IPC sockets, and no
// way to the user's systemd or PipeWire), its own D-Bus session (or its shell
// would take the notifications, portals and media keys of the live one), its
// own settings, state and cache, and ATRIUM_BOX, which leaves the phone, the
// screens (DDC) and the session's services to the user's own atrium.
static void box_paths(const char *name, char *run, size_t n) {
	snprintf(run, n, "%s/run", vc_box_dir(name));
}

// atriumctl against the box, its output captured.
static const char *box_ctl(const char *name, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static const char *box_ctl(const char *name, const char *fmt, ...) {
	char args[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(args, sizeof args, fmt, ap);
	va_end(ap);
	char run[600];
	box_paths(name, run, sizeof run);
	const char *ipc = vc_state_get(name, "ipc");
	return vc_capture("XDG_RUNTIME_DIR='%s' atriumctl -s '%s' %s 2>&1", run, ipc ? ipc : "", args);
}

// A line of the box's log after `marker`, up to the end of the line.
static const char *log_after(const char *logpath, const char *marker) {
	return vc_capture("sed -n 's|.*%s\\(.*\\)$|\\1|p' '%s' 2>/dev/null | head -1", marker, logpath);
}

/* --- lifecycle ---------------------------------------------------------- */

static int wait_for_display(const char *display, double timeout_s) {
	struct vc_wl w;
	for (double waited = 0; waited < timeout_s; waited += 0.15) {
		if (vc_wl_connect(&w, display) == 0) {
			vc_wl_disconnect(&w);
			return 0;
		}
		vc_sleep_s(0.15);
	}
	return -1;
}

// Reports the size the box's output actually ended up with.
static int probe_size(const char *display, int *w_out, int *h_out) {
	struct vc_wl w;
	if (vc_wl_connect(&w, display) < 0) return -1;
	struct vc_output *o = vc_wl_output(&w, NULL);
	int rc = -1;
	if (o) {
		*w_out = o->logical_w;
		*h_out = o->logical_h;
		rc = 0;
	}
	vc_wl_disconnect(&w);
	return rc;
}

int vc_box_start(struct vc_box_opts *o) {
	if (vc_box_alive(o->name)) {
		vc_warn("box '%s' is already running", o->name);
		return 1;
	}
	if (o->hide || o->host_input || o->decorated)
		vc_log("--hide, --host-input and --decorated were the nested Hyprland's; "
		       "an atrium box has no host window");

	const char *dir = vc_box_dir(o->name);
	char run[600], logpath[600], registry[600], path[700];
	box_paths(o->name, run, sizeof run);
	snprintf(logpath, sizeof logpath, "%s/box.log", dir);
	snprintf(registry, sizeof registry, "%s/registry.db", dir);
	// A fresh box each time: settings, windows remembered and state included.
	vc_run("rm -rf '%s'", dir);
	static const char *const subdirs[] = { "run", "config", "state/atrium", "cache", "desk" };
	for (size_t i = 0; i < sizeof subdirs / sizeof *subdirs; i++) {
		snprintf(path, sizeof path, "%s/%s/", dir, subdirs[i]);
		if (vc_mkdirs(path) < 0) vc_die("cannot create %s", path);
	}
	chmod(run, 0700);
	// No first-run welcome over the app under test.
	snprintf(path, sizeof path, "%s/state/atrium/welcomed", dir);
	close(open(path, O_WRONLY | O_CREAT, 0600));

	pid_t pid = fork();
	if (pid < 0) vc_die("fork: %s", strerror(errno));

	if (pid == 0) {
		int log = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (log >= 0) {
			dup2(log, STDOUT_FILENO);
			dup2(log, STDERR_FILENO);
		}
		setsid();

		// Nothing of the host's display: atrium would open as a window in it.
		unsetenv("WAYLAND_DISPLAY");
		unsetenv("WAYLAND_SOCKET");
		unsetenv("DISPLAY");
		unsetenv("HYPRLAND_INSTANCE_SIGNATURE");
		unsetenv("DBUS_SESSION_BUS_ADDRESS");
		unsetenv("ATRIUM_SOCKET");
		setenv("WLR_BACKENDS", "headless", 1);
		setenv("ATRIUM_BOX", "1", 1);
		setenv("ATRIUM_NO_WRAPPER", "1", 1);
		setenv("ATRIUM_NO_DDC", "1", 1);
		setenv("XDG_RUNTIME_DIR", run, 1);
		snprintf(path, sizeof path, "%s/config", dir);
		setenv("XDG_CONFIG_HOME", path, 1);
		snprintf(path, sizeof path, "%s/state", dir);
		setenv("XDG_STATE_HOME", path, 1);
		snprintf(path, sizeof path, "%s/cache", dir);
		setenv("XDG_CACHE_HOME", path, 1);
		snprintf(path, sizeof path, "%s/desk", dir);
		setenv("ATRIUM_DESKTOP_DIR", path, 1);
		// A box stuck in a busy loop at RT priority starves the host (Bluetooth
		// audio stalled); a box runs as a plain process.
		const struct rlimit no_rt = {0, 0};
		setrlimit(RLIMIT_RTPRIO, &no_rt);

		execlp("dbus-run-session", "dbus-run-session", "--", "atrium", "-c", registry, (char *)NULL);
		_exit(127);
	}

	vc_state_set(o->name, "pid", vc_capture("echo %d", pid));
	vc_state_set(o->name, "log", logpath);

	// It says where it listens once it does.
	const char *ipc = NULL, *socket = NULL;
	for (double waited = 0; waited < 15.0 && !(ipc && *ipc && socket && *socket); waited += 0.15) {
		if (!vc_box_alive(o->name)) break;
		vc_sleep_s(0.15);
		ipc = log_after(logpath, "ipc: listening on ");
		if (ipc && *ipc) vc_state_set(o->name, "ipc", ipc);
		socket = log_after(logpath, "running on WAYLAND_DISPLAY=");
	}
	if (!socket || !*socket) {
		vc_warn("box '%s' did not come up; see %s", o->name, logpath);
		kill(-pid, SIGTERM);
		return 1;
	}
	// The box's own runtime dir isn't the clients': they find it by path.
	char display[700];
	snprintf(display, sizeof display, "%s/%s", run, socket);
	vc_state_set(o->name, "display", display);
	if (wait_for_display(display, 10.0) < 0) {
		vc_warn("box '%s' did not come up; see %s", o->name, logpath);
		kill(-pid, SIGTERM);
		return 1;
	}

	// Its one screen at the size asked for.
	const char *out = box_ctl(o->name, "-j outputs | sed -n 's/.*\"name\": *\"\\([^\"]*\\)\".*/\\1/p' | head -1");
	char output[128];
	snprintf(output, sizeof output, "%s", out && *out ? out : "HEADLESS-1");
	vc_state_set(o->name, "output", output);
	box_ctl(o->name, "output '%s' width=%d height=%d refresh=60000 scale=%g", output, o->width, o->height,
	        o->scale);
	// The default secret space for chat apps starts Discord when shown.
	box_ctl(o->name, "app discord launch=");
	if (o->fullscreen)
		box_ctl(o->name, "rule add 'app_pattern=.*' fullscreen=true");
	for (int i = 0; i < o->n_rules; i++) box_ctl(o->name, "rule add %s", o->rules[i]);

	int got_w = 0, got_h = 0;
	for (int i = 0; i < 20 && (got_w != o->width || got_h != o->height); i++) {
		vc_sleep_s(0.1);
		probe_size(display, &got_w, &got_h);
	}
	char size[64];
	snprintf(size, sizeof size, "%dx%d", got_w, got_h);
	vc_state_set(o->name, "size", size);

	// The pointer in the middle, where a fresh session has it (the resize
	// above pulled it into a corner), and a keyboard on the seat, as a real
	// one has from the start.
	struct vc_ctx c;
	if (vc_ctx_open(&c, o->name) == 0) {
		vc_input_keyboard(&c.in);
		vc_input_move(&c.in, got_w / 2.0, got_h / 2.0);
		vc_ctx_close(&c);
	}

	// Xwayland inside the box has its own DISPLAY; `vc box exec` must use that
	// one, never the host's, or X11 apps would open in the live session.
	for (int i = 0; i < 30; i++) {
		const char *xdisp = log_after(logpath, "Starting Xwayland on ");
		if (xdisp && *xdisp) {
			vc_state_set(o->name, "xdisplay", xdisp);
			break;
		}
		vc_sleep_s(0.1);
	}

	printf("box %s up: WAYLAND_DISPLAY=%s %dx%d\n", o->name, display, got_w, got_h);
	if (got_w != o->width || got_h != o->height)
		vc_warn("requested %dx%d but got %dx%d", o->width, o->height, got_w, got_h);
	return 0;
}

int vc_box_kill(const char *name) {
	int pid = vc_state_get_int(name, "pid", 0);
	if (pid <= 0) {
		vc_warn("no box named '%s'", name);
		return 1;
	}
	// The whole session: dbus-run-session, atrium, its shell and its bus.
	if (kill(-pid, SIGTERM) == 0) {
		for (int i = 0; i < 40 && kill(pid, 0) == 0; i++) vc_sleep_s(0.1);
		if (kill(pid, 0) == 0) kill(-pid, SIGKILL);
	}
	char statefile[600];
	snprintf(statefile, sizeof statefile, "%s/box", vc_box_dir(name));
	unlink(statefile);
	printf("box %s stopped\n", name);
	return 0;
}

int vc_box_list(void) {
	char root[512];
	snprintf(root, sizeof root, "%s/vctools", vc_runtime_dir());
	DIR *d = opendir(root);
	if (!d) return 0;

	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		// vc_state_get's answers share one buffer: each copied before the next.
		const char *disp = vc_box_display(e->d_name);
		if (!disp) continue;
		char dispbuf[512], sizebuf[64];
		snprintf(dispbuf, sizeof dispbuf, "%s", disp);
		const char *size = vc_state_get(e->d_name, "size");
		snprintf(sizebuf, sizeof sizebuf, "%s", size ? size : "?");
		const int alive = vc_box_alive(e->d_name);
		printf("%-16s %-10s %-8s %s\n", e->d_name, sizebuf, alive ? "running" : "dead", dispbuf);
	}
	closedir(d);
	return 0;
}

// Sets up the environment so a child process joins the box instead of the host.
static void apply_box_env(const char *name) {
	const char *disp = vc_box_display(name);
	if (!disp) vc_die("no box named '%s'", name);
	setenv("WAYLAND_DISPLAY", disp, 1);

	unsetenv("HYPRLAND_INSTANCE_SIGNATURE");

	const char *xdisp = vc_state_get(name, "xdisplay");
	if (xdisp && *xdisp)
		setenv("DISPLAY", xdisp, 1);
	else
		unsetenv("DISPLAY"); // never leak the host's X server into the box
}

int vc_box_exec(const char *name, char **argv) {
	if (!vc_box_alive(name)) vc_die("box '%s' is not running", name);
	apply_box_env(name);
	execvp(argv[0], argv);
	vc_die("exec %s: %s", argv[0], strerror(errno));
	return 127;
}

int vc_box_env(const char *name) {
	const char *disp = vc_box_display(name);
	if (!disp) vc_die("no box named '%s'", name);
	printf("export WAYLAND_DISPLAY=%s\n", disp);
	printf("export ATRIUM_SOCKET=%s\n", vc_state_get(name, "ipc") ? vc_state_get(name, "ipc") : "");
	const char *xdisp = vc_state_get(name, "xdisplay");
	if (xdisp && *xdisp) printf("export DISPLAY=%s\n", xdisp);
	else printf("unset DISPLAY\n");
	return 0;
}
