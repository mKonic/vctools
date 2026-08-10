# vctools

Drive a real GUI — clicks, keystrokes, screenshots — **without touching your
live session**. Your cursor never moves, your focus never changes, your
workspace never switches, and no window gets resized.

```sh
vc box start ladle --size 1920x1080     # isolated compositor, hidden on the host
vc box exec ladle -- ./ladle &          # run the app under test inside it
vc wait ladle window "Ladle Editor"
vc in ladle move 400 300 click left
vc in ladle type "hello" key Return
vc shot ladle out.png
vc box kill ladle
```

## Why a nested compositor

Wayland is **single-seat**: one cursor, one keyboard focus, shared by every
output. So the obvious approaches don't work:

- `ydotool` and friends inject into the seat via uinput, so a synthetic
  keystroke lands in whatever *you* have focused.
- A virtual monitor (`hyprctl output create headless`) doesn't help either — an
  off-screen output still shares that one seat, so moving the pointer there
  drags your real cursor off your screen.
- Hyprland doesn't implement `ext-transient-seat`, so a second seat is not
  available in-process.

A **box** is a nested Hyprland instance. It has its own seat, its own cursor,
its own keyboard focus and its own IPC socket. `vc` sends virtual input to
*that* compositor, so by construction it cannot leak into yours. The box renders
into a window that `vc` parks on a hidden workspace before it ever maps.

Not supported: **touch input**. There is no virtual-touch Wayland protocol, and
uinput would create a device on your real seat, defeating the point.

## Commands

| | |
|---|---|
| `vc box start <name>` | `--size WxH` `--scale S` `--hide WS` `--fullscreen` `--decorated` `--host-input` `--rule '<lua>'` |
| `vc box kill/list/env <name>` | lifecycle; `env` prints export lines for a shell |
| `vc box exec <name> -- <cmd>` | run a program inside the box |
| `vc ls <name>` | list windows in the box |
| `vc in <name> <ops…>` | send input and exit |
| `vc run <name> <script\|->` | run a script of ops over one connection |
| `vc shot <name> [match] <out.png>` | capture a window, or the whole screen |
| `vc wait <name> window\|gone\|settle …` | synchronise instead of sleeping |

Ops (shared by `in`, `run` and scripts):

```
move X Y | moverel DX DY | click [btn] [n] | down/up [btn]
drag X0 Y0 X1 Y1 [steps] | scroll [DX] DY
type TEXT | key SPEC | hold SPEC | release SPEC     SPEC: Return, F5, ctrl+shift+p
sleep S | delay MS | shot [match] PATH | wait window|gone|settle …
```

Coordinates are **logical pixels** in the box's output, origin top-left.

Use `host` as the box name to work against the live session instead
(`vc ls host`, `vc shot host "Ladle Editor" out.png`). That is winshot's old job,
absorbed here — capture works on any workspace, focused or not, because it grabs
the toplevel's surface rather than an output. Input sent to `host` is *not*
isolated, by definition.

### Real-time or scripted

Every `vc in` is a fresh short-lived client, so you can keep poking at a running
box interactively — send some input, look at a screenshot, decide what to do
next — with no restart. `vc run` exists for when you want a fixed sequence over
a single connection; `vc run <box> -` reads a script from stdin.

## Rules

The box does **not** read your Hyprland config, so per-app rules like "open on
workspace 4 silent, fullscreen" never apply to the app under test — it just
fills the box's output. Inside the box every window gets `render_unfocused` so
captures never come back stale; `--fullscreen` fullscreens them, and `--rule`
appends any `hl.window_rule{…}` line you want.

On the host, `vc` adds transient rules for its own window (class `aquamarine`)
*before* the box starts — placement has to be decided by a rule, since
dispatching after the window maps means it has already shoved your layout
around. Rules land on `special:vcbox` by default (`--hide 4` to use a normal
workspace instead), and `hyprctl reload` clears them.

## Build

```sh
make            # -> ./vc          (needs wayland-client, xkbcommon, zlib)
make install    # -> ~/.local/bin/vc
make test       # smoke test; also asserts your host windows never moved
```

Protocol glue is generated from XML at build time into `build/`; nothing
generated is checked in, so a protocol bump is just `make clean`.

## Notes from getting this working

- The nested Wayland backend mirrors the host keyboard into the box, and that
  device carries **stale modifier state** (Shift and Super stuck down) which
  merges into every synthetic keystroke: letters arrive uppercase and Return
  arrives as `^[[27;10;13~`, so keybinds silently never match. The generated
  config disables the mirrored devices; `--host-input` keeps them if you want to
  drive the box by hand.
- Typing never depends on your keyboard layout: `vc` builds an xkb keymap on the
  fly with one keycode per keysym it needs, so no modifier is involved unless a
  chord explicitly asks for one.
- Pointer coordinates are logical, but `wl_output` reports the *physical* mode;
  on a scaled output those differ by the scale factor. `vc` reads the logical
  size from `xdg-output`.
- `HYPRLAND_NO_SD_VARS=1` is essential: otherwise the nested compositor pushes
  its own `WAYLAND_DISPLAY` into systemd and D-Bus, and every app you launch
  afterwards tries to connect to the box.
- Hyprland cannot start with only a headless backend
  (`CBackend::create() failed!`), so the box always has a host window; it is just
  never anywhere you can see it.
