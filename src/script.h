// One op vocabulary, used by both `vc in` (one shot, real time) and `vc run`
// (a whole script over a single connection).
#ifndef VC_SCRIPT_H
#define VC_SCRIPT_H

#include <stdio.h>

#include "input.h"
#include "wl.h"

struct vc_ctx {
	struct vc_wl w;
	struct vc_input in;
	const char *box;
	int connected;
	int failures;
};

int  vc_ctx_open(struct vc_ctx *c, const char *box);
void vc_ctx_close(struct vc_ctx *c);

// Runs one op. Returns the number of argv entries consumed, or -1 on error.
int vc_op(struct vc_ctx *c, int argc, char **argv);
// Runs every op in argv, one after another.
int vc_ops(struct vc_ctx *c, int argc, char **argv);

int vc_run_script(struct vc_ctx *c, FILE *f);

int vc_wait_window(struct vc_ctx *c, const char *match, double timeout);
int vc_wait_settle(struct vc_ctx *c, const char *match, double timeout, double threshold);
int vc_wait_gone(struct vc_ctx *c, const char *match, double timeout);

#endif
