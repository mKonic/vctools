// vc — virtual control tools.
//
// Runs a GUI under test inside a headless atrium that has its own seat, then
// drives it with synthetic input and captures it, without ever touching the
// cursor, focus or workspace of the live session.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "box.h"
#include "script.h"
#include "util.h"
#include "wl.h"

static int usage(void) {
	fputs(
		"usage: vc [-v] <command>\n"
		"\n"
		"  box start <name> [--size WxH] [--scale S] [--fullscreen]\n"
		"                   [--rule 'app_pattern=RE FIELD=VALUE...']\n"
		"  box kill <name> | box list | box env <name>\n"
		"  box exec <name> -- <cmd> [args...]   run a program inside the box\n"
		"\n"
		"  ls <name>                            list windows in the box\n"
		"  in <name> <op> [op...]               send input, then exit\n"
		"  run <name> <script|->                run a script of ops\n"
		"  shot <name> [match] <out.png>        capture a window, or the screen\n"
		"  wait <name> window <match> [secs]\n"
		"  wait <name> settle [secs]\n"
		"\n"
		"ops: move X Y | moverel DX DY | click [btn] [n] | down/up [btn]\n"
		"     drag X0 Y0 X1 Y1 [steps] | scroll [DX] DY\n"
		"     type TEXT | key SPEC | hold SPEC | release SPEC\n"
		"     sleep S | delay MS | shot [match] PATH | wait ...\n"
		"\n"
		"Coordinates are logical pixels within the box's output, origin top-left.\n",
		stderr);
	return 2;
}

static int cmd_box(int argc, char **argv) {
	if (argc < 1) return usage();
	const char *sub = argv[0];

	if (!strcmp(sub, "list")) return vc_box_list();

	if (argc < 2) return usage();
	const char *name = argv[1];

	if (!strcmp(sub, "kill")) {
		if (!strcmp(name, "--all")) {
			vc_box_list();
			return 0;
		}
		return vc_box_kill(name);
	}
	if (!strcmp(sub, "env")) return vc_box_env(name);

	if (!strcmp(sub, "exec")) {
		int i = 2;
		if (i < argc && !strcmp(argv[i], "--")) i++;
		if (i >= argc) return usage();
		return vc_box_exec(name, argv + i);
	}

	if (!strcmp(sub, "start")) {
		struct vc_box_opts o = {
			.name = name,
			.width = 1920,
			.height = 1080,
			.scale = 1.0,
			.hide = NULL,
		};
		for (int i = 2; i < argc; i++) {
			if (!strcmp(argv[i], "--size") && i + 1 < argc) {
				if (sscanf(argv[++i], "%dx%d", &o.width, &o.height) != 2)
					vc_die("--size wants WxH, e.g. 1920x1080");
			} else if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
				o.scale = atof(argv[++i]);
			} else if (!strcmp(argv[i], "--hide") && i + 1 < argc) {
				o.hide = argv[++i];
			} else if (!strcmp(argv[i], "--host-input")) {
				o.host_input = 1;
			} else if (!strcmp(argv[i], "--decorated")) {
				o.decorated = 1;
			} else if (!strcmp(argv[i], "--fullscreen")) {
				o.fullscreen = 1;
			} else if (!strcmp(argv[i], "--rule") && i + 1 < argc) {
				if (o.n_rules < VC_MAX_RULES) o.rules[o.n_rules++] = argv[++i];
			} else {
				vc_die("unknown option '%s'", argv[i]);
			}
		}
		return vc_box_start(&o);
	}

	return usage();
}

static int cmd_ls(const char *box) {
	struct vc_ctx c;
	if (vc_ctx_open(&c, box) < 0) return 1;

	vc_wl_refresh_toplevels(&c.w);
	for (int i = 0; i < c.w.n_toplevels; i++)
		printf("[%d] %-28s %s\n", i, c.w.toplevels[i].app_id, c.w.toplevels[i].title);
	if (!c.w.n_toplevels) printf("(no windows)\n");

	vc_ctx_close(&c);
	return 0;
}

int main(int argc, char **argv) {
	int i = 1;
	while (i < argc && argv[i][0] == '-' && argv[i][1] == 'v' && !argv[i][2]) {
		vc_verbose = 1;
		i++;
	}
	if (i >= argc) return usage();

	const char *cmd = argv[i++];
	argc -= i;
	argv += i;

	if (!strcmp(cmd, "box")) return cmd_box(argc, argv);
	if (!strcmp(cmd, "help") || !strcmp(cmd, "--help")) return usage();

	if (argc < 1) return usage();
	const char *box = argv[0];

	if (!strcmp(cmd, "ls")) return cmd_ls(box);

	struct vc_ctx c;
	if (vc_ctx_open(&c, box) < 0) return 1;
	int rc = 0;

	if (!strcmp(cmd, "in")) {
		rc = vc_ops(&c, argc - 1, argv + 1) < 0;
	} else if (!strcmp(cmd, "wait") || !strcmp(cmd, "shot")) {
		// Both are just ops; reuse the same vocabulary.
		char *args[8];
		int n = 0;
		args[n++] = (char *)cmd;
		for (int k = 1; k < argc && n < 8; k++) args[n++] = argv[k];
		rc = vc_op(&c, n, args) < 0;
	} else if (!strcmp(cmd, "run")) {
		if (argc < 2) {
			vc_ctx_close(&c);
			return usage();
		}
		FILE *f = !strcmp(argv[1], "-") ? stdin : fopen(argv[1], "r");
		if (!f) vc_die("cannot open %s", argv[1]);
		rc = vc_run_script(&c, f) < 0;
		if (f != stdin) fclose(f);
	} else {
		vc_ctx_close(&c);
		return usage();
	}

	vc_ctx_close(&c);
	return rc;
}
