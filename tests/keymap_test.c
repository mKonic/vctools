// Keyboard resolution, without a compositor: the keymap must be canonical and
// it must not change while a string is being typed. Both failures this covers
// were only ever seen through a GUI — a client got nonsense scancodes, and a
// keymap swapped between two key events cost the key that followed it.
#include "../src/input.c"

#include <stdio.h>

static int fails;

#define CHECK(cond, ...)                                        \
	do {                                                        \
		if (!(cond)) {                                          \
			fails++;                                            \
			printf("  FAIL %s:%d ", __FILE__, __LINE__);        \
			printf(__VA_ARGS__);                                \
			printf("\n");                                       \
		}                                                       \
	} while (0)

// send_keymap() without the wayland half: compile the text and adopt it.
static int load_keymap(struct vc_input *in) {
	char *text = build_keymap(in);
	if (!text) return -1;
	if (!in->xkb) in->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *km =
		xkb_keymap_new_from_string(in->xkb, text, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
	free(text);
	if (!km) return -1;
	if (in->km) xkb_keymap_unref(in->km);
	in->km = km;
	for (int i = 0; i < VC_N_MODS; i++) {
		xkb_mod_index_t idx = xkb_keymap_mod_get_index(km, MOD_DEF[i].xkb_mod);
		in->mods[i].bit = idx == XKB_MOD_INVALID ? 0 : 1u << idx;
		in->mods[i].code = plain_keycode(km, xkb_keysym_from_name(MOD_DEF[i].keysym, 0));
	}
	in->keymap_dirty = 0;
	return 0;
}

static struct vc_input in;

static void expect_key(const char *what, xkb_keysym_t sym, int code, uint32_t modbit) {
	int got_code;
	uint32_t got_mask;
	if (lookup_sym(&in, sym, &got_code, &got_mask) < 0) {
		fails++;
		printf("  FAIL %s: not in the keymap at all\n", what);
		return;
	}
	CHECK(got_code == code, "%s: evdev %d, want %d", what, got_code, code);
	CHECK(got_mask == modbit, "%s: mods 0x%x, want 0x%x", what, got_mask, modbit);
}

// Every character of `s` must resolve against the keymap already loaded. A
// character that does not is one that used to be appended to the keymap and
// sent immediately before its own press — the dropped-character bug.
static void expect_types_clean(const char *s) {
	int dirty_before = in.keymap_dirty;
	for (const char *p = s; *p;) {
		uint32_t cp = utf8_next(&p);
		xkb_keysym_t sym = cp == '\n' ? XKB_KEY_Return : xkb_utf32_to_keysym(cp);
		int code;
		uint32_t mask;
		CHECK(sym != XKB_KEY_NoSymbol, "\"%s\": U+%04X has no keysym", s, cp);
		CHECK(lookup_sym(&in, sym, &code, &mask) == 0,
		      "\"%s\": U+%04X ('%c') needs the keymap rebuilt mid-string", s, cp,
		      cp < 0x80 ? (char)cp : '?');
	}
	CHECK(in.keymap_dirty == dirty_before, "\"%s\" dirtied the keymap", s);
}

int main(void) {
	for (int i = 0; i < VC_N_MODS; i++) in.mods[i].code = -1;

	printf("keymap compiles and is canonical\n");
	CHECK(load_keymap(&in) == 0, "the default keymap failed to compile");
	if (!in.km) return 1;

	uint32_t SH = in.mods[VC_MOD_SHIFT].bit;
	CHECK(SH != 0, "no Shift modifier in the keymap");
	CHECK(in.mods[VC_MOD_SHIFT].code == 42, "Shift_L is evdev %d, want 42",
	      in.mods[VC_MOD_SHIFT].code);
	CHECK(in.mods[VC_MOD_CTRL].code == 29, "Control_L is evdev %d, want 29",
	      in.mods[VC_MOD_CTRL].code);
	CHECK(in.mods[VC_MOD_ALT].code == 56, "Alt_L is evdev %d, want 56", in.mods[VC_MOD_ALT].code);
	CHECK(in.mods[VC_MOD_SUPER].code == 125, "Super_L is evdev %d, want 125",
	      in.mods[VC_MOD_SUPER].code);

	// The codes GLFW read straight off the wire and got wrong: Escape arrived
	// as GLFW_KEY_4 and LeftCtrl as GLFW_KEY_1 under the old per-keysym slots.
	expect_key("Escape", XKB_KEY_Escape, 1, 0);
	expect_key("Control_L", XKB_KEY_Control_L, 29, 0);
	expect_key("Return", XKB_KEY_Return, 28, 0);
	expect_key("Tab", XKB_KEY_Tab, 15, 0);
	expect_key("BackSpace", XKB_KEY_BackSpace, 14, 0);
	expect_key("F5", XKB_KEY_F5, 63, 0);
	expect_key("Left", XKB_KEY_Left, 105, 0);
	expect_key("'a'", XKB_KEY_a, 30, 0);
	expect_key("'k'", XKB_KEY_k, 37, 0);
	expect_key("'1'", XKB_KEY_1, 2, 0);
	expect_key("space", XKB_KEY_space, 57, 0);
	// Shifted levels are reached by holding shift, not by their own bare key.
	expect_key("'A'", XKB_KEY_A, 30, SH);
	expect_key("'!'", XKB_KEY_exclam, 2, SH);
	expect_key("'?'", XKB_KEY_question, 53, SH);

	printf("printable ascii is fully reachable\n");
	for (uint32_t cp = 0x20; cp < 0x7f; cp++) {
		int code;
		uint32_t mask;
		CHECK(lookup_sym(&in, xkb_utf32_to_keysym(cp), &code, &mask) == 0,
		      "U+%04X ('%c') is unreachable", cp, (char)cp);
	}

	printf("typing never rebuilds the keymap\n");
	// The three strings that came back short from VAE's Player.
	expect_types_clean("relay works");
	expect_types_clean("hello from ada");
	expect_types_clean("got it, grace here");
	expect_types_clean("The quick brown fox! 0123456789 ~`@#$%^&*()_+-=[]{}\\|;:'\",.<>/?\n");

	printf("unmappable keysyms go to spare keys, once\n");
	const char *exotic = "café → naïve";
	for (const char *p = exotic; *p;) {
		uint32_t cp = utf8_next(&p);
		int code;
		uint32_t mask;
		if (lookup_sym(&in, xkb_utf32_to_keysym(cp), &code, &mask) < 0)
			CHECK(want_spare(&in, xkb_utf32_to_keysym(cp)) == 0, "no spare for U+%04X", cp);
	}
	CHECK(in.keymap_dirty == 1, "the exotic string should have dirtied the keymap");
	CHECK(in.n_extra == 3, "expected 3 spare bindings (eacute, rightarrow, idiaeresis), got %d",
	      in.n_extra);
	CHECK(load_keymap(&in) == 0, "the keymap with spare bindings failed to compile");
	expect_types_clean(exotic); // and now it types clean, like anything else
	for (int i = 0; i < in.n_extra; i++) {
		int code;
		uint32_t mask;
		CHECK(lookup_sym(&in, in.extra[i], &code, &mask) == 0, "spare %d did not take", i);
		CHECK(code == VC_SPARE_BASE - 8 + i, "spare %d is evdev %d, want %d", i, code,
		      VC_SPARE_BASE - 8 + i);
		CHECK(mask == 0, "spare %d needs modifiers", i);
	}

	printf("%s\n", fails ? "FAILED" : "ok");
	return fails != 0;
}
