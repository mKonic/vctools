#define _GNU_SOURCE
#include "script.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "box.h"
#include "png.h"
#include "shot.h"
#include "util.h"

int vc_ctx_open(struct vc_ctx *c, const char *box) {
	memset(c, 0, sizeof *c);

	// "host" targets the live session instead of a box — that is what winshot
	// did, and it is still the right tool for grabbing a window you are actually
	// looking at. Input sent to the host is not isolated, by definition.
	if (box && (!strcmp(box, "host") || !strcmp(box, "-"))) box = NULL;
	c->box = box;

	const char *display = box ? vc_box_display(box) : NULL;
	if (box && !display) {
		vc_warn("no box named '%s'", box);
		return -1;
	}
	if (vc_wl_connect(&c->w, display) < 0) return -1;
	if (vc_input_init(&c->in, &c->w, NULL) < 0) return -1;
	c->connected = 1;
	return 0;
}

void vc_ctx_close(struct vc_ctx *c) {
	if (!c->connected) return;
	vc_input_finish(&c->in);
	vc_wl_disconnect(&c->w);
	c->connected = 0;
}

/* --- waits -------------------------------------------------------------- */

int vc_wait_window(struct vc_ctx *c, const char *match, double timeout) {
	for (double waited = 0; waited <= timeout; waited += 0.1) {
		// Reconnecting each round is what makes this see windows that appeared
		// after we connected: the toplevel list only replays on a fresh bind.
		struct vc_wl probe;
		if (vc_wl_connect(&probe, c->box ? vc_box_display(c->box) : NULL) == 0) {
			struct vc_toplevel *t = vc_wl_find_toplevel(&probe, match);
			int found = t != NULL;
			vc_wl_disconnect(&probe);
			if (found) return 0;
		}
		vc_sleep_s(0.1);
	}
	vc_warn("timed out waiting for window '%s'", match ? match : "(any)");
	return -1;
}

int vc_wait_gone(struct vc_ctx *c, const char *match, double timeout) {
	for (double waited = 0; waited <= timeout; waited += 0.1) {
		struct vc_wl probe;
		if (vc_wl_connect(&probe, c->box ? vc_box_display(c->box) : NULL) == 0) {
			struct vc_toplevel *t = vc_wl_find_toplevel(&probe, match);
			int found = t != NULL;
			vc_wl_disconnect(&probe);
			if (!found) return 0;
		}
		vc_sleep_s(0.1);
	}
	vc_warn("timed out waiting for '%s' to close", match ? match : "(any)");
	return -1;
}

// Captures repeatedly until two consecutive frames are near-identical. This is
// what replaces sleeping and hoping the UI finished drawing.
int vc_wait_settle(struct vc_ctx *c, const char *match, double timeout, double threshold) {
	struct vc_frame prev = { 0 }, cur = { 0 };
	int rc = -1;

	for (double waited = 0; waited <= timeout; waited += 0.12) {
		struct vc_frame f = { 0 };
		int ok;
		if (match && *match) {
			vc_wl_refresh_toplevels(&c->w);
			struct vc_toplevel *t = vc_wl_find_toplevel(&c->w, match);
			if (!t) {
				vc_sleep_s(0.12);
				continue;
			}
			ok = vc_shot_window(&c->w, t, &f) == 0;
		} else {
			struct vc_output *o = vc_wl_output(&c->w, NULL);
			ok = o && vc_shot_output(&c->w, o, &f) == 0;
		}
		if (!ok) {
			vc_sleep_s(0.12);
			continue;
		}

		vc_frame_free(&prev);
		prev = cur;
		cur = f;

		if (prev.data && vc_frame_diff(&prev, &cur) <= threshold) {
			rc = 0;
			break;
		}
		vc_sleep_s(0.12);
	}

	vc_frame_free(&prev);
	vc_frame_free(&cur);
	if (rc < 0) vc_warn("timed out waiting for the frame to settle");
	return rc;
}

/* --- ops ---------------------------------------------------------------- */

static int save_frame(struct vc_ctx *c, const char *match, const char *path) {
	struct vc_frame f = { 0 };
	int ok;

	if (match && *match) {
		vc_wl_refresh_toplevels(&c->w);
		struct vc_toplevel *t = vc_wl_find_toplevel(&c->w, match);
		if (!t) {
			vc_warn("no window matching '%s'", match);
			return -1;
		}
		ok = vc_shot_window(&c->w, t, &f) == 0;
	} else {
		struct vc_output *o = vc_wl_output(&c->w, NULL);
		ok = o && vc_shot_output(&c->w, o, &f) == 0;
	}
	if (!ok) return -1;

	int rc;
	size_t len = strlen(path);
	if (len > 5 && !strcasecmp(path + len - 5, ".bgra")) {
		FILE *out = fopen(path, "wb");
		rc = out ? 0 : -1;
		if (out) {
			fwrite(f.data, 1, (size_t)f.stride * f.h, out);
			fclose(out);
		}
	} else {
		rc = vc_png_write(path, f.data, f.w, f.h, f.stride);
	}
	if (rc == 0) printf("%s %dx%d\n", path, f.w, f.h);
	else vc_warn("could not write %s", path);

	vc_frame_free(&f);
	return rc;
}

int vc_op(struct vc_ctx *c, int argc, char **argv) {
	const char *op = argv[0];
	int left = argc - 1; // arguments available after the op name

#define WANT(n)                                                                                    \
	do {                                                                                           \
		if (left < (n)) {                                                                          \
			vc_warn("%s needs %d argument(s)", op, (n));                                           \
			return -1;                                                                             \
		}                                                                                          \
	} while (0)

	if (!strcmp(op, "move")) {
		WANT(2);
		return vc_input_move(&c->in, atof(argv[1]), atof(argv[2])) < 0 ? -1 : 3;
	}
	if (!strcmp(op, "moverel")) {
		WANT(2);
		return vc_input_move_rel(&c->in, atof(argv[1]), atof(argv[2])) < 0 ? -1 : 3;
	}
	if (!strcmp(op, "click")) {
		const char *btn = left >= 1 ? argv[1] : "left";
		int consumed = left >= 1 ? 2 : 1;
		int count = 1;
		if (left >= 2 && isdigit((unsigned char)argv[2][0])) {
			count = atoi(argv[2]);
			consumed = 3;
		}
		return vc_input_click(&c->in, btn, count) < 0 ? -1 : consumed;
	}
	if (!strcmp(op, "down") || !strcmp(op, "up")) {
		const char *btn = left >= 1 ? argv[1] : "left";
		int pressed = !strcmp(op, "down");
		return vc_input_button(&c->in, btn, pressed) < 0 ? -1 : (left >= 1 ? 2 : 1);
	}
	if (!strcmp(op, "drag")) {
		WANT(4);
		int steps = left >= 5 ? atoi(argv[5]) : 20;
		int rc = vc_input_drag(&c->in, atof(argv[1]), atof(argv[2]), atof(argv[3]), atof(argv[4]),
		                       steps);
		return rc < 0 ? -1 : (left >= 5 ? 6 : 5);
	}
	if (!strcmp(op, "scroll")) {
		WANT(1);
		double dx = 0, dy = atof(argv[1]);
		int consumed = 2;
		if (left >= 2 && (isdigit((unsigned char)argv[2][0]) || argv[2][0] == '-')) {
			dx = dy;
			dy = atof(argv[2]);
			consumed = 3;
		}
		return vc_input_scroll(&c->in, dx, dy) < 0 ? -1 : consumed;
	}
	if (!strcmp(op, "type")) {
		WANT(1);
		return vc_input_type(&c->in, argv[1]) < 0 ? -1 : 2;
	}
	if (!strcmp(op, "key")) {
		WANT(1);
		return vc_input_key(&c->in, argv[1]) < 0 ? -1 : 2;
	}
	if (!strcmp(op, "hold")) {
		WANT(1);
		return vc_input_key_hold(&c->in, argv[1], 1) < 0 ? -1 : 2;
	}
	if (!strcmp(op, "release")) {
		WANT(1);
		return vc_input_key_hold(&c->in, argv[1], 0) < 0 ? -1 : 2;
	}
	if (!strcmp(op, "sleep")) {
		WANT(1);
		vc_sleep_s(atof(argv[1]));
		return 2;
	}
	if (!strcmp(op, "shot")) {
		WANT(1);
		// `shot out.png` captures the screen; `shot Title out.png` one window.
		if (left >= 2) return save_frame(c, argv[1], argv[2]) < 0 ? -1 : 3;
		return save_frame(c, NULL, argv[1]) < 0 ? -1 : 2;
	}
	if (!strcmp(op, "wait")) {
		WANT(1);
		if (!strcmp(argv[1], "settle")) {
			double timeout = left >= 2 ? atof(argv[2]) : 5.0;
			return vc_wait_settle(c, NULL, timeout, 0.35) < 0 ? -1 : (left >= 2 ? 3 : 2);
		}
		if (!strcmp(argv[1], "window")) {
			WANT(2);
			double timeout = left >= 3 ? atof(argv[3]) : 10.0;
			return vc_wait_window(c, argv[2], timeout) < 0 ? -1 : (left >= 3 ? 4 : 3);
		}
		if (!strcmp(argv[1], "gone")) {
			WANT(2);
			double timeout = left >= 3 ? atof(argv[3]) : 10.0;
			return vc_wait_gone(c, argv[2], timeout) < 0 ? -1 : (left >= 3 ? 4 : 3);
		}
		vc_warn("unknown wait '%s'", argv[1]);
		return -1;
	}
	if (!strcmp(op, "delay")) {
		WANT(1);
		c->in.delay_ms = atoi(argv[1]);
		return 2;
	}

	vc_warn("unknown op '%s'", op);
	return -1;
#undef WANT
}

int vc_ops(struct vc_ctx *c, int argc, char **argv) {
	int i = 0;
	while (i < argc) {
		int used = vc_op(c, argc - i, argv + i);
		if (used < 0) return -1;
		i += used;
	}
	return 0;
}

// Splits a script line into tokens, honouring "double quotes" so text with
// spaces survives.
static int tokenize(char *line, char **out, int max) {
	int n = 0;
	char *p = line;
	while (*p && n < max) {
		while (*p == ' ' || *p == '\t') p++;
		if (!*p || *p == '\n') break;
		if (*p == '#') break; // comment to end of line

		char *start;
		if (*p == '"' || *p == '\'') {
			char quote = *p++;
			start = p;
			while (*p && *p != quote) p++;
		} else {
			start = p;
			while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
		}
		if (*p) *p++ = 0;
		out[n++] = start;
	}
	return n;
}

int vc_run_script(struct vc_ctx *c, FILE *f) {
	char line[2048];
	int lineno = 0, failures = 0;

	while (fgets(line, sizeof line, f)) {
		lineno++;
		char *tokens[32];
		int n = tokenize(line, tokens, 32);
		if (!n) continue;

		vc_log("line %d: %s", lineno, tokens[0]);
		if (vc_ops(c, n, tokens) < 0) {
			vc_warn("script line %d failed: %s", lineno, tokens[0]);
			failures++;
		}
	}
	c->failures = failures;
	return failures ? -1 : 0;
}
