#define _GNU_SOURCE
#include "input.h"

#include <stdio.h>
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

// Spare keycodes for keysyms the layout has no key for. The evdev keycode file
// leaves 257..359 unassigned, so binding these disturbs nothing real.
#define VC_SPARE_BASE 300

// A client must receive, mmap and compile a keymap before it can resolve
// anything typed under it; a key event sent into that window is dropped.
#define VC_KEYMAP_SETTLE_S 0.06

static void flush(struct vc_input *in) {
	wl_display_flush(in->w->dpy);
	if (in->delay_ms > 0) vc_sleep_s(in->delay_ms / 1000.0);
}

/* --- modifiers ---------------------------------------------------------- */

// The keysym that presses each VC_MOD_*, and the modifier it asserts.
static const struct {
	const char *keysym;  // the key that asserts it
	const char *xkb_mod; // its name in the keymap
} MOD_DEF[VC_N_MODS] = {
	{ "Shift_L", XKB_MOD_NAME_SHIFT },   { "Control_L", XKB_MOD_NAME_CTRL },
	{ "Alt_L", XKB_MOD_NAME_ALT },       { "Super_L", XKB_MOD_NAME_LOGO },
	{ "ISO_Level3_Shift", "Mod5" },
};

/* --- keymap ------------------------------------------------------------- */

// One keymap, built from the standard evdev/us layout and sent once. Keys
// therefore carry their canonical evdev scancode (Escape 1, Control_L 29, a
// 30), so a client that reads the raw scancode instead of resolving it through
// the keymap — GLFW does — agrees with one that resolves it properly.
//
// It also means the keymap never changes between two key events. It used to:
// each new keysym got the next free slot and was sent immediately before its
// own press, so a client still compiling the new keymap dropped that key while
// every already-mapped key came through. Only keysyms `us` cannot reach at all
// still grow the keymap, and callers bind those before pressing anything.
static char *build_keymap(struct vc_input *in) {
	size_t cap = 1024 + (size_t)in->n_extra * 192;
	char *s = malloc(cap);
	if (!s) return NULL;
	size_t n = 0;

	n += (size_t)snprintf(s + n, cap - n, "xkb_keymap {\nxkb_keycodes { include \"evdev\"\n");
	for (int i = 0; i < in->n_extra; i++)
		n += (size_t)snprintf(s + n, cap - n, "\t<VC%d> = %d;\n", i, VC_SPARE_BASE + i);
	n += (size_t)snprintf(s + n, cap - n,
	                      "};\n"
	                      "xkb_types { include \"complete\" };\n"
	                      "xkb_compat { include \"complete\" };\n"
	                      "xkb_symbols { include \"pc+us+inet(evdev)\"\n");
	for (int i = 0; i < in->n_extra; i++) {
		char name[128];
		if (xkb_keysym_get_name(in->extra[i], name, sizeof name) <= 0)
			snprintf(name, sizeof name, "0x%08x", in->extra[i]);
		n += (size_t)snprintf(s + n, cap - n, "\tkey <VC%d> { [ %s ] };\n", i, name);
	}
	snprintf(s + n, cap - n, "};\n};\n");
	return s;
}

// The evdev scancode carrying `sym` unmodified, or -1.
static int plain_keycode(struct xkb_keymap *km, xkb_keysym_t sym) {
	for (xkb_keycode_t kc = xkb_keymap_min_keycode(km); kc <= xkb_keymap_max_keycode(km); kc++) {
		const xkb_keysym_t *syms;
		int n = xkb_keymap_key_get_syms_by_level(km, kc, 0, 0, &syms);
		for (int i = 0; i < n; i++)
			if (syms[i] == sym) return (int)kc - 8;
	}
	return -1;
}

static int send_keymap(struct vc_input *in) {
	char *text = build_keymap(in);
	if (!text) return -1;

	if (!in->xkb) in->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	// Compile locally first: a keymap the compositor rejects is silently fatal
	// (every later key event gets dropped), so fail loudly here instead.
	struct xkb_keymap *km = xkb_keymap_new_from_string(in->xkb, text, XKB_KEYMAP_FORMAT_TEXT_V1,
	                                                   XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!km) {
		vc_warn("generated keymap failed to compile; some keysym is unnameable");
		free(text);
		return -1;
	}

	size_t len = strlen(text) + 1;
	int fd = memfd_create("vc-keymap", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)len) < 0) {
		vc_warn("memfd for keymap failed");
		xkb_keymap_unref(km);
		free(text);
		return -1;
	}
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		close(fd);
		xkb_keymap_unref(km);
		free(text);
		return -1;
	}
	memcpy(p, text, len);
	munmap(p, len);
	free(text);

	zwp_virtual_keyboard_v1_keymap(in->kbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)len);
	wl_display_roundtrip(in->w->dpy);
	close(fd);

	if (in->km) xkb_keymap_unref(in->km);
	in->km = km;
	for (int i = 0; i < VC_N_MODS; i++) {
		xkb_mod_index_t idx = xkb_keymap_mod_get_index(km, MOD_DEF[i].xkb_mod);
		in->mods[i].bit = idx == XKB_MOD_INVALID ? 0 : 1u << idx;
		in->mods[i].code = plain_keycode(km, xkb_keysym_from_name(MOD_DEF[i].keysym, 0));
	}
	in->keymap_dirty = 0;
	vc_sleep_s(VC_KEYMAP_SETTLE_S);
	return 0;
}

// Finds how to produce `sym`: which key, and which modifiers must be held.
// Searches level-major so the unmodified spelling of a keysym wins.
static int lookup_sym(struct vc_input *in, xkb_keysym_t sym, int *code, uint32_t *mask) {
	uint32_t reachable = 0;
	for (int i = 0; i < VC_N_MODS; i++)
		if (in->mods[i].code >= 0) reachable |= in->mods[i].bit;

	for (xkb_level_index_t lvl = 0; lvl < 8; lvl++) {
		for (xkb_keycode_t kc = xkb_keymap_min_keycode(in->km);
		     kc <= xkb_keymap_max_keycode(in->km); kc++) {
			if (lvl >= xkb_keymap_num_levels_for_key(in->km, kc, 0)) continue;
			const xkb_keysym_t *syms;
			int n = xkb_keymap_key_get_syms_by_level(in->km, kc, 0, lvl, &syms), hit = 0;
			for (int i = 0; i < n; i++)
				if (syms[i] == sym) { hit = 1; break; }
			if (!hit) continue;
			if (lvl == 0) {
				*code = (int)kc - 8;
				*mask = 0;
				return 0;
			}
			xkb_mod_mask_t cand[16];
			size_t got = xkb_keymap_key_get_mods_for_level(in->km, kc, 0, lvl, cand,
			                                               sizeof cand / sizeof *cand);
			for (size_t i = 0; i < got; i++)
				if ((cand[i] & ~reachable) == 0) {
					*code = (int)kc - 8;
					*mask = cand[i];
					return 0;
				}
		}
	}
	return -1;
}

// Queues a keysym the layout cannot reach onto a spare key. This is the only
// thing that still changes the keymap, so callers must do it for every keysym
// they are about to need before sending the first key event.
static int want_spare(struct vc_input *in, xkb_keysym_t sym) {
	for (int i = 0; i < in->n_extra; i++)
		if (in->extra[i] == sym) return 0;
	if (in->n_extra >= VC_MAX_EXTRA) {
		vc_warn("out of spare keys (%d unmappable keysyms)", VC_MAX_EXTRA);
		return -1;
	}
	in->extra[in->n_extra++] = sym;
	in->keymap_dirty = 1;
	return 0;
}

// lookup_sym, binding a spare key first if the layout cannot reach the keysym.
static int resolve_sym(struct vc_input *in, xkb_keysym_t sym, int *code, uint32_t *mask) {
	if (lookup_sym(in, sym, code, mask) == 0) return 0;
	if (want_spare(in, sym) < 0) return -1;
	if (in->keymap_dirty && send_keymap(in) < 0) return -1;
	if (lookup_sym(in, sym, code, mask) < 0) {
		vc_warn("no key can produce keysym 0x%04x", sym);
		return -1;
	}
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

// Holds down (or lets go of) every modifier in `mask`, pressing the real key
// as well as asserting the mask, per set_mods above.
static void hold_mods(struct vc_input *in, uint32_t mask, int down) {
	if (down) {
		for (int i = 0; i < VC_N_MODS; i++)
			if ((mask & in->mods[i].bit) && in->mods[i].code >= 0)
				raw_key(in, in->mods[i].code, 1);
		set_mods(in, mask);
	} else {
		for (int i = VC_N_MODS; i-- > 0;)
			if ((mask & in->mods[i].bit) && in->mods[i].code >= 0)
				raw_key(in, in->mods[i].code, 0);
		set_mods(in, 0);
	}
}

static int mod_from_name(struct vc_input *in, const char *n) {
	if (!strcasecmp(n, "shift")) return VC_MOD_SHIFT;
	if (!strcasecmp(n, "ctrl") || !strcasecmp(n, "control")) return VC_MOD_CTRL;
	if (!strcasecmp(n, "alt") || !strcasecmp(n, "mod1")) return VC_MOD_ALT;
	if (!strcasecmp(n, "super") || !strcasecmp(n, "logo") || !strcasecmp(n, "meta") ||
	    !strcasecmp(n, "mod4"))
		return VC_MOD_SUPER;
	if (!strcasecmp(n, "altgr") || !strcasecmp(n, "mod5") || !strcasecmp(n, "level3"))
		return VC_MOD_LEVEL3;
	(void)in;
	return -1;
}

// Splits "ctrl+shift+p" into a modifier mask and the trailing keysym.
static int parse_chord(struct vc_input *in, const char *spec, uint32_t *mods, xkb_keysym_t *sym) {
	char buf[256];
	snprintf(buf, sizeof buf, "%s", spec);
	*mods = 0;

	char *part = buf, *plus;
	while ((plus = strchr(part, '+')) && plus != part) {
		*plus = 0;
		int m = mod_from_name(in, part);
		if (m < 0) {
			vc_warn("unknown modifier '%s'", part);
			return -1;
		}
		*mods |= in->mods[m].bit;
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

int vc_is_button_name(const char *name) {
	if (!name || !*name) return 0;
	if (!strcasecmp(name, "left") || !strcasecmp(name, "right") ||
	    !strcasecmp(name, "middle") || !strcasecmp(name, "back") ||
	    !strcasecmp(name, "side") || !strcasecmp(name, "forward") ||
	    !strcasecmp(name, "extra")) return 1;
	char *end = NULL;
	strtoul(name, &end, 0);
	return end && *end == '\0'; // a raw evdev code, and nothing but
}

static uint32_t button_code(const char *name) {
	if (!name || !strcasecmp(name, "left")) return BTN_LEFT;
	if (!strcasecmp(name, "right")) return BTN_RIGHT;
	if (!strcasecmp(name, "middle")) return BTN_MIDDLE;
	if (!strcasecmp(name, "back") || !strcasecmp(name, "side")) return BTN_SIDE;
	if (!strcasecmp(name, "forward") || !strcasecmp(name, "extra")) return BTN_EXTRA;

	char *end = NULL;
	unsigned long raw = strtoul(name, &end, 0);
	if (end && *end == '\0' && raw > 0) return (uint32_t)raw;

	// Anything else used to fall through strtoul as button 0, which is not an evdev button at
	// all: clients that derive their index as code - BTN_LEFT got a negative one and either
	// asserted or wrote out of bounds. Refuse it loudly instead.
	fprintf(stderr, "vc: '%s' is not a button; using left\n", name);
	return BTN_LEFT;
}

int vc_input_init(struct vc_input *in, struct vc_wl *w, const char *output_name) {
	memset(in, 0, sizeof *in);
	for (int i = 0; i < VC_N_MODS; i++) in->mods[i].code = -1;
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
	if (in->km) xkb_keymap_unref(in->km);
	if (in->xkb) xkb_context_unref(in->xkb);
	in->km = NULL;
	in->xkb = NULL;
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
	int code;
	uint32_t mask;
	if (resolve_sym(in, sym, &code, &mask) < 0) return -1;
	if (mask) hold_mods(in, mask, 1);
	raw_key(in, code, 1);
	raw_key(in, code, 0);
	if (mask) hold_mods(in, mask, 0);
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

	// Bind every character the layout cannot reach first, so the keymap is
	// settled before the first key event rather than replaced between two.
	for (const char *p = utf8; *p;) {
		uint32_t cp = utf8_next(&p);
		xkb_keysym_t sym = cp == '\n' ? XKB_KEY_Return : xkb_utf32_to_keysym(cp);
		if (sym == XKB_KEY_NoSymbol) {
			vc_warn("no keysym for U+%04X, skipping", cp);
			continue;
		}
		int code;
		uint32_t mask;
		if (lookup_sym(in, sym, &code, &mask) < 0) want_spare(in, sym);
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
	if (parse_chord(in, spec, &mods, &sym) < 0) return -1;

	int code;
	uint32_t level_mods;
	if (resolve_sym(in, sym, &code, &level_mods) < 0) return -1;
	// "ctrl+P" asks for shift twice over — once by name, once by naming a keysym
	// that only exists at a shifted level. Either spelling holds it down.
	mods |= level_mods;

	if (down) {
		hold_mods(in, mods, 1);
		raw_key(in, code, 1);
	}
	if (up) {
		raw_key(in, code, 0);
		hold_mods(in, mods, 0);
	}
	return 0;
}

int vc_input_key(struct vc_input *in, const char *spec) {
	return chord(in, spec, 1, 1);
}

int vc_input_key_hold(struct vc_input *in, const char *spec, int down) {
	return chord(in, spec, down, !down);
}
