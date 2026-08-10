#define _GNU_SOURCE
#include "input.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "util.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112
#define BTN_SIDE 0x113
#define BTN_EXTRA 0x114

// Slots 0..3 are permanently the modifier keys, so a chord never has to reshape
// the keymap; everything else is assigned on demand.
#define SLOT_SHIFT 0
#define SLOT_CTRL 1
#define SLOT_ALT 2
#define SLOT_SUPER 3
#define SLOT_FIRST_FREE 4

static void flush(struct vc_input *in) {
	wl_display_flush(in->w->dpy);
	if (in->delay_ms > 0) vc_sleep_s(in->delay_ms / 1000.0);
}

/* --- keymap ------------------------------------------------------------- */

// Returns the wayland keycode for `sym`, adding it to the keymap if needed.
static int slot_for(struct vc_input *in, xkb_keysym_t sym) {
	for (int i = 0; i < in->n_syms; i++)
		if (in->syms[i] == sym) return i + 1;
	if (in->n_syms >= VC_MAX_KEYSYMS) {
		vc_warn("keymap full (%d keysyms)", VC_MAX_KEYSYMS);
		return -1;
	}
	in->syms[in->n_syms++] = sym;
	in->keymap_dirty = 1;
	return in->n_syms; // slot index + 1
}

// Builds a keymap holding exactly the keysyms used so far, one per keycode.
static char *build_keymap(struct vc_input *in) {
	size_t cap = 4096 + (size_t)in->n_syms * 128;
	char *s = malloc(cap);
	if (!s) return NULL;
	size_t n = 0;

	n += (size_t)snprintf(s + n, cap - n,
	                      "xkb_keymap {\n"
	                      "xkb_keycodes \"vc\" {\n"
	                      "\tminimum = 8;\n"
	                      "\tmaximum = 255;\n");
	for (int i = 0; i < in->n_syms; i++)
		n += (size_t)snprintf(s + n, cap - n, "\t<K%d> = %d;\n", i + 9, i + 9);
	n += (size_t)snprintf(s + n, cap - n,
	                      "};\n"
	                      "xkb_types \"vc\" { include \"complete\" };\n"
	                      "xkb_compatibility \"vc\" { include \"complete\" };\n"
	                      "xkb_symbols \"vc\" {\n"
	                      "\tname[Group1] = \"vc\";\n");
	for (int i = 0; i < in->n_syms; i++) {
		char name[128];
		if (xkb_keysym_get_name(in->syms[i], name, sizeof name) <= 0)
			snprintf(name, sizeof name, "0x%08x", in->syms[i]);
		n += (size_t)snprintf(s + n, cap - n, "\tkey <K%d> { [ %s ] };\n", i + 9, name);
	}
	// Bind the four reserved slots to real modifiers so chords behave like a
	// physical keyboard rather than a bare modifier mask.
	n += (size_t)snprintf(s + n, cap - n,
	                      "\tmodifier_map Shift { <K%d> };\n"
	                      "\tmodifier_map Control { <K%d> };\n"
	                      "\tmodifier_map Mod1 { <K%d> };\n"
	                      "\tmodifier_map Mod4 { <K%d> };\n"
	                      "};\n};\n",
	                      SLOT_SHIFT + 9, SLOT_CTRL + 9, SLOT_ALT + 9, SLOT_SUPER + 9);
	return s;
}

static int send_keymap(struct vc_input *in) {
	char *text = build_keymap(in);
	if (!text) return -1;

	// Compile locally first: a keymap the compositor rejects is silently fatal
	// (every later key event gets dropped), so fail loudly here instead.
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *check =
		xkb_keymap_new_from_string(ctx, text, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!check) {
		vc_warn("generated keymap failed to compile; some keysym is unnameable");
		xkb_context_unref(ctx);
		free(text);
		return -1;
	}
	xkb_keymap_unref(check);
	xkb_context_unref(ctx);

	size_t len = strlen(text) + 1;
	int fd = memfd_create("vc-keymap", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)len) < 0) {
		vc_warn("memfd for keymap failed");
		free(text);
		return -1;
	}
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		close(fd);
		free(text);
		return -1;
	}
	memcpy(p, text, len);
	munmap(p, len);
	free(text);

	zwp_virtual_keyboard_v1_keymap(in->kbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)len);
	wl_display_roundtrip(in->w->dpy);
	close(fd);

	in->keymap_dirty = 0;
	return 0;
}

static int kbd_ready(struct vc_input *in) {
	if (!in->w->keyboard_mgr) {
		vc_warn("compositor has no zwp_virtual_keyboard_manager_v1");
		return -1;
	}
	if (!in->kbd) {
		in->kbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(in->w->keyboard_mgr,
		                                                                  in->w->seat);
		// Reserve the modifier slots up front so their keycodes never move.
		in->syms[SLOT_SHIFT] = XKB_KEY_Shift_L;
		in->syms[SLOT_CTRL] = XKB_KEY_Control_L;
		in->syms[SLOT_ALT] = XKB_KEY_Alt_L;
		in->syms[SLOT_SUPER] = XKB_KEY_Super_L;
		in->n_syms = SLOT_FIRST_FREE;
		in->keymap_dirty = 1;
	}
	if (in->keymap_dirty && send_keymap(in) < 0) return -1;
	return 0;
}

static void raw_key(struct vc_input *in, int keycode, int pressed) {
	zwp_virtual_keyboard_v1_key(in->kbd, vc_now_ms(), (uint32_t)keycode,
	                            pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
	                                    : WL_KEYBOARD_KEY_STATE_RELEASED);
	flush(in);
}

// Hyprland does not derive modifier state from a virtual keyboard's key events,
// so the mask is asserted explicitly alongside the real modifier key presses.
static void set_mods(struct vc_input *in, uint32_t mask) {
	zwp_virtual_keyboard_v1_modifiers(in->kbd, mask, 0, 0, 0);
	wl_display_flush(in->w->dpy);
}

#define MOD_SHIFT (1u << 0)
#define MOD_CTRL (1u << 2)
#define MOD_ALT (1u << 3)
#define MOD_SUPER (1u << 6)

static int mod_slot(uint32_t bit) {
	switch (bit) {
	case MOD_SHIFT: return SLOT_SHIFT;
	case MOD_CTRL: return SLOT_CTRL;
	case MOD_ALT: return SLOT_ALT;
	case MOD_SUPER: return SLOT_SUPER;
	}
	return -1;
}

static uint32_t mod_from_name(const char *n) {
	if (!strcasecmp(n, "shift")) return MOD_SHIFT;
	if (!strcasecmp(n, "ctrl") || !strcasecmp(n, "control")) return MOD_CTRL;
	if (!strcasecmp(n, "alt") || !strcasecmp(n, "mod1")) return MOD_ALT;
	if (!strcasecmp(n, "super") || !strcasecmp(n, "logo") || !strcasecmp(n, "meta") ||
	    !strcasecmp(n, "mod4"))
		return MOD_SUPER;
	return 0;
}

// Splits "ctrl+shift+p" into a modifier mask and the trailing keysym.
static int parse_chord(const char *spec, uint32_t *mods, xkb_keysym_t *sym) {
	char buf[256];
	snprintf(buf, sizeof buf, "%s", spec);
	*mods = 0;

	char *part = buf, *plus;
	while ((plus = strchr(part, '+')) && plus != part) {
		*plus = 0;
		uint32_t m = mod_from_name(part);
		if (!m) {
			vc_warn("unknown modifier '%s'", part);
			return -1;
		}
		*mods |= m;
		part = plus + 1;
	}

	*sym = xkb_keysym_from_name(part, XKB_KEYSYM_NO_FLAGS);
	if (*sym == XKB_KEY_NoSymbol)
		*sym = xkb_keysym_from_name(part, XKB_KEYSYM_CASE_INSENSITIVE);
	if (*sym == XKB_KEY_NoSymbol) {
		vc_warn("unknown key '%s'", part);
		return -1;
	}
	return 0;
}

/* --- pointer ------------------------------------------------------------ */

static int ptr_ready(struct vc_input *in) {
	if (!in->w->pointer_mgr) {
		vc_warn("compositor has no zwlr_virtual_pointer_manager_v1");
		return -1;
	}
	if (in->ptr) return 0;

	if (zwlr_virtual_pointer_manager_v1_get_version(in->w->pointer_mgr) >= 2 && in->out)
		in->ptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
			in->w->pointer_mgr, in->w->seat, in->out->output);
	else
		in->ptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(in->w->pointer_mgr,
		                                                                 in->w->seat);
	wl_display_roundtrip(in->w->dpy);
	return 0;
}

static uint32_t button_code(const char *name) {
	if (!name || !strcasecmp(name, "left")) return BTN_LEFT;
	if (!strcasecmp(name, "right")) return BTN_RIGHT;
	if (!strcasecmp(name, "middle")) return BTN_MIDDLE;
	if (!strcasecmp(name, "back") || !strcasecmp(name, "side")) return BTN_SIDE;
	if (!strcasecmp(name, "forward") || !strcasecmp(name, "extra")) return BTN_EXTRA;
	return (uint32_t)strtoul(name, NULL, 0); // raw evdev code
}

int vc_input_init(struct vc_input *in, struct vc_wl *w, const char *output_name) {
	memset(in, 0, sizeof *in);
	in->w = w;
	in->out = vc_wl_output(w, output_name);
	in->delay_ms = 8;
	if (!w->seat) {
		vc_warn("no wl_seat on this display");
		return -1;
	}
	return 0;
}

void vc_input_finish(struct vc_input *in) {
	if (in->kbd) {
		set_mods(in, 0);
		zwp_virtual_keyboard_v1_destroy(in->kbd);
		in->kbd = NULL;
	}
	if (in->ptr) {
		zwlr_virtual_pointer_v1_destroy(in->ptr);
		in->ptr = NULL;
	}
	if (in->w->dpy) wl_display_roundtrip(in->w->dpy);
}

int vc_input_move(struct vc_input *in, double x, double y) {
	if (ptr_ready(in) < 0) return -1;
	if (!in->out) {
		vc_warn("no output to position the pointer in");
		return -1;
	}
	// motion_absolute maps x/x_extent across the output, and the compositor's
	// coordinate space is the logical one, not the raw mode.
	zwlr_virtual_pointer_v1_motion_absolute(in->ptr, vc_now_ms(), (uint32_t)(x < 0 ? 0 : x),
	                                        (uint32_t)(y < 0 ? 0 : y),
	                                        (uint32_t)in->out->logical_w,
	                                        (uint32_t)in->out->logical_h);
	zwlr_virtual_pointer_v1_frame(in->ptr);
	flush(in);
	in->x = x;
	in->y = y;
	in->have_pos = 1;
	return 0;
}

int vc_input_move_rel(struct vc_input *in, double dx, double dy) {
	if (ptr_ready(in) < 0) return -1;
	zwlr_virtual_pointer_v1_motion(in->ptr, vc_now_ms(), wl_fixed_from_double(dx),
	                               wl_fixed_from_double(dy));
	zwlr_virtual_pointer_v1_frame(in->ptr);
	flush(in);
	in->x += dx;
	in->y += dy;
	return 0;
}

int vc_input_button(struct vc_input *in, const char *button, int pressed) {
	if (ptr_ready(in) < 0) return -1;
	zwlr_virtual_pointer_v1_button(in->ptr, vc_now_ms(), button_code(button),
	                               pressed ? WL_POINTER_BUTTON_STATE_PRESSED
	                                       : WL_POINTER_BUTTON_STATE_RELEASED);
	zwlr_virtual_pointer_v1_frame(in->ptr);
	flush(in);
	return 0;
}

int vc_input_click(struct vc_input *in, const char *button, int count) {
	for (int i = 0; i < (count < 1 ? 1 : count); i++) {
		if (vc_input_button(in, button, 1) < 0) return -1;
		vc_sleep_s(0.03);
		if (vc_input_button(in, button, 0) < 0) return -1;
		if (i + 1 < count) vc_sleep_s(0.05); // inside the double-click window
	}
	return 0;
}

int vc_input_scroll(struct vc_input *in, double dx, double dy) {
	if (ptr_ready(in) < 0) return -1;
	zwlr_virtual_pointer_v1_axis_source(in->ptr, WL_POINTER_AXIS_SOURCE_WHEEL);
	if (dy != 0) {
		zwlr_virtual_pointer_v1_axis_discrete(in->ptr, vc_now_ms(),
		                                      WL_POINTER_AXIS_VERTICAL_SCROLL,
		                                      wl_fixed_from_double(dy * 15.0), (int32_t)dy);
	}
	if (dx != 0) {
		zwlr_virtual_pointer_v1_axis_discrete(in->ptr, vc_now_ms(),
		                                      WL_POINTER_AXIS_HORIZONTAL_SCROLL,
		                                      wl_fixed_from_double(dx * 15.0), (int32_t)dx);
	}
	zwlr_virtual_pointer_v1_frame(in->ptr);
	flush(in);
	return 0;
}

int vc_input_drag(struct vc_input *in, double x0, double y0, double x1, double y1, int steps) {
	if (steps < 2) steps = 20;
	if (vc_input_move(in, x0, y0) < 0) return -1;
	vc_sleep_s(0.03);
	if (vc_input_button(in, "left", 1) < 0) return -1;
	for (int i = 1; i <= steps; i++) {
		double t = (double)i / steps;
		if (vc_input_move(in, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t) < 0) return -1;
	}
	vc_sleep_s(0.03);
	return vc_input_button(in, "left", 0);
}

/* --- keyboard ----------------------------------------------------------- */

static int tap_sym(struct vc_input *in, xkb_keysym_t sym) {
	int code = slot_for(in, sym);
	if (code < 0) return -1;
	if (in->keymap_dirty && send_keymap(in) < 0) return -1;
	raw_key(in, code, 1);
	raw_key(in, code, 0);
	return 0;
}

// Decodes one UTF-8 sequence, advancing *p.
static uint32_t utf8_next(const char **p) {
	const unsigned char *s = (const unsigned char *)*p;
	uint32_t cp;
	int extra;

	if (s[0] < 0x80) { cp = s[0]; extra = 0; }
	else if ((s[0] & 0xe0) == 0xc0) { cp = s[0] & 0x1f; extra = 1; }
	else if ((s[0] & 0xf0) == 0xe0) { cp = s[0] & 0x0f; extra = 2; }
	else if ((s[0] & 0xf8) == 0xf0) { cp = s[0] & 0x07; extra = 3; }
	else { *p += 1; return 0xfffd; }

	for (int i = 1; i <= extra; i++) {
		if ((s[i] & 0xc0) != 0x80) { *p += 1; return 0xfffd; }
		cp = (cp << 6) | (s[i] & 0x3f);
	}
	*p += extra + 1;
	return cp;
}

int vc_input_type(struct vc_input *in, const char *utf8) {
	if (kbd_ready(in) < 0) return -1;

	// Assign every character a keycode first, so the keymap is sent once.
	for (const char *p = utf8; *p;) {
		uint32_t cp = utf8_next(&p);
		xkb_keysym_t sym = cp == '\n' ? XKB_KEY_Return : xkb_utf32_to_keysym(cp);
		if (sym == XKB_KEY_NoSymbol) {
			vc_warn("no keysym for U+%04X, skipping", cp);
			continue;
		}
		slot_for(in, sym);
	}
	if (in->keymap_dirty && send_keymap(in) < 0) return -1;
	set_mods(in, 0); // never inherit whatever state was left behind

	for (const char *p = utf8; *p;) {
		uint32_t cp = utf8_next(&p);
		xkb_keysym_t sym = cp == '\n' ? XKB_KEY_Return : xkb_utf32_to_keysym(cp);
		if (sym == XKB_KEY_NoSymbol) continue;
		if (tap_sym(in, sym) < 0) return -1;
	}
	return 0;
}

static int chord(struct vc_input *in, const char *spec, int down, int up) {
	if (kbd_ready(in) < 0) return -1;

	uint32_t mods;
	xkb_keysym_t sym;
	if (parse_chord(spec, &mods, &sym) < 0) return -1;

	slot_for(in, sym);
	if (in->keymap_dirty && send_keymap(in) < 0) return -1;
	int code = slot_for(in, sym);

	static const uint32_t all[] = { MOD_SHIFT, MOD_CTRL, MOD_ALT, MOD_SUPER };

	if (down) {
		for (size_t i = 0; i < sizeof all / sizeof *all; i++)
			if (mods & all[i]) raw_key(in, mod_slot(all[i]) + 1, 1);
		set_mods(in, mods);
		raw_key(in, code, 1);
	}
	if (up) {
		raw_key(in, code, 0);
		for (size_t i = sizeof all / sizeof *all; i-- > 0;)
			if (mods & all[i]) raw_key(in, mod_slot(all[i]) + 1, 0);
		set_mods(in, 0);
	}
	return 0;
}

int vc_input_key(struct vc_input *in, const char *spec) {
	return chord(in, spec, 1, 1);
}

int vc_input_key_hold(struct vc_input *in, const char *spec, int down) {
	return chord(in, spec, down, !down);
}
