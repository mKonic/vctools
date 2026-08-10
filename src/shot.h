// Frame capture, absorbed from winshot: grabs one toplevel's surface (works on
// a hidden workspace, unlike anything built on wlr-screencopy) or a whole output.
#ifndef VC_SHOT_H
#define VC_SHOT_H

#include <stdint.h>

#include "wl.h"

struct vc_frame {
	uint8_t *data;
	int w, h, stride;
	uint32_t format; // wl_shm format as offered by the compositor
};

int  vc_shot_window(struct vc_wl *w, struct vc_toplevel *t, struct vc_frame *out);
int  vc_shot_output(struct vc_wl *w, struct vc_output *o, struct vc_frame *out);
void vc_frame_free(struct vc_frame *f);

// Mean absolute per-channel difference between two same-sized frames, 0..255.
double vc_frame_diff(const struct vc_frame *a, const struct vc_frame *b);

#endif
