// Synthetic pointer and keyboard input.
//
// Keyboard input deliberately does NOT use the host's keyboard layout. Every
// keysym a command needs gets its own keycode in a keymap generated on the fly,
// at shift level 1, so typing never depends on — or disturbs — modifier state.
// Modifiers are only involved when a chord explicitly asks for them.
#ifndef VC_INPUT_H
#define VC_INPUT_H

#include <xkbcommon/xkbcommon.h>

#include "wl.h"

#define VC_MAX_KEYSYMS 200

struct vc_input {
	struct vc_wl *w;
	struct vc_output *out;
	struct zwlr_virtual_pointer_v1 *ptr;
	struct zwp_virtual_keyboard_v1 *kbd;

	xkb_keysym_t syms[VC_MAX_KEYSYMS]; // slot i lives at wayland keycode i + 1
	int n_syms;
	int keymap_dirty;

	double x, y;    // last pointer position, in logical output coordinates
	int have_pos;
	int delay_ms;   // pause after each generated event
};

int  vc_input_init(struct vc_input *in, struct vc_wl *w, const char *output_name);
void vc_input_finish(struct vc_input *in);

int  vc_input_move(struct vc_input *in, double x, double y);
int  vc_input_move_rel(struct vc_input *in, double dx, double dy);
int  vc_input_button(struct vc_input *in, const char *button, int pressed);
int  vc_input_click(struct vc_input *in, const char *button, int count);
int  vc_input_scroll(struct vc_input *in, double dx, double dy);
int  vc_input_drag(struct vc_input *in, double x0, double y0, double x1, double y1, int steps);

int  vc_input_type(struct vc_input *in, const char *utf8);
// `spec` is a chord such as "Return", "F5", "ctrl+s" or "ctrl+shift+p".
int  vc_input_key(struct vc_input *in, const char *spec);
int  vc_input_key_hold(struct vc_input *in, const char *spec, int down);

#endif
