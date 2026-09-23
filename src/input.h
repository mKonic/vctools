// Synthetic pointer and keyboard input.
//
// Keyboard input uses a fixed keymap of its own: the standard evdev/us layout,
// compiled and sent once. That gives every key its canonical evdev scancode, so
// clients reading raw scancodes and clients resolving through the keymap agree,
// and it means the keymap never changes between two key events — a keymap
// swapped mid-stream costs whichever key follows it.
#ifndef VC_INPUT_H
#define VC_INPUT_H

#include <xkbcommon/xkbcommon.h>

#include "wl.h"

// Keysyms the `us` layout has no key for at all get a spare key each; only
// these still grow the keymap.
#define VC_MAX_EXTRA 32

// The modifiers vc can physically press. A shift level needing anything else is
// out of reach, and the keysym living there falls back to a spare key instead.
enum { VC_MOD_SHIFT, VC_MOD_CTRL, VC_MOD_ALT, VC_MOD_SUPER, VC_MOD_LEVEL3, VC_N_MODS };

struct vc_input {
	struct vc_wl *w;
	struct vc_output *out;
	struct zwlr_virtual_pointer_v1 *ptr;
	struct zwp_virtual_keyboard_v1 *kbd;

	struct xkb_context *xkb;
	struct xkb_keymap *km; // the keymap the client is resolving keys through
	int keymap_dirty;

	xkb_keysym_t extra[VC_MAX_EXTRA]; // keysyms bound to spare keys
	int n_extra;

	struct {
		int code;     // evdev scancode of the key that asserts it, -1 if none
		uint32_t bit; // its bit in a modifier mask
	} mods[VC_N_MODS];
	uint32_t held;  // modifiers down through `hold`, kept across later chords

	double x, y;    // last pointer position, in logical output coordinates
	int have_pos;
	int delay_ms;   // pause after each generated event
};

int  vc_input_init(struct vc_input *in, struct vc_wl *w, const char *output_name);
void vc_input_finish(struct vc_input *in);

int  vc_input_move(struct vc_input *in, double x, double y);
int  vc_input_move_rel(struct vc_input *in, double dx, double dy);
// Does this token name a mouse button? Ops with an optional button argument must ask, or they
// swallow the next op as a button name.
int  vc_is_button_name(const char *name);
int  vc_input_button(struct vc_input *in, const char *button, int pressed);
int  vc_input_click(struct vc_input *in, const char *button, int count);
int  vc_input_scroll(struct vc_input *in, double dx, double dy);
int  vc_input_drag(struct vc_input *in, double x0, double y0, double x1, double y1, int steps);

int  vc_input_type(struct vc_input *in, const char *utf8);
// `spec` is a chord such as "Return", "F5", "ctrl+s" or "ctrl+shift+p".
int  vc_input_key(struct vc_input *in, const char *spec);
int  vc_input_key_hold(struct vc_input *in, const char *spec, int down);

#endif
