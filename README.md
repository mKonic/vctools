# vctools

Drive a GUI with synthetic clicks and keystrokes, and screenshot it, without
touching the live session. Your cursor never moves, focus never changes, no
window is resized.

The app under test runs in a *box*: a nested Hyprland with its own seat, cursor,
keyboard focus and IPC socket, parked on a hidden workspace. Input goes to that
compositor, so it cannot leak into yours. Wayland has one seat per session,
which is why `ydotool` and a virtual monitor both fail at this.

## Usage

```sh
vc box start ladle --size 1920x1080
vc box exec ladle -- ./ladle &
vc wait ladle window "Ladle Editor"
vc in ladle move 400 300 click left
vc in ladle type "hello" key Return
vc shot ladle out.png
vc box kill ladle
```

| Command | |
|---|---|
| `box start <name>` | `--size WxH` `--scale S` `--hide WS` `--fullscreen` `--decorated` `--host-input` `--rule '<lua>'` |
| `box kill/list/env <name>` | `env` prints export lines for a shell |
| `box exec <name> -- <cmd>` | run a program inside the box |
| `ls <name>` | list windows in the box |
| `in <name> <ops…>` | send input and exit |
| `run <name> <script\|->` | run a script of ops over one connection |
| `shot <name> [match] <out.png>` | one window, or the whole screen |
| `wait <name> window\|gone\|settle …` | synchronise instead of sleeping |

Ops, shared by `in`, `run` and scripts:

```
move X Y | moverel DX DY | click [btn] [n] | down/up [btn]
drag X0 Y0 X1 Y1 [steps] | scroll [DX] DY
type TEXT | key SPEC | hold SPEC | release SPEC      SPEC: Return, F5, ctrl+shift+p
sleep S | delay MS | shot [match] PATH | wait window|gone|settle …
```

Coordinates are logical pixels in the box's output, origin top-left.

Every `vc in` is a fresh short-lived client, so a running box can be driven
interactively — send input, look at a screenshot, decide what to do next — with
no restart. `vc run` is for fixed sequences, and reads stdin as `-`.

Use `host` as the box name to work against the live session instead:
`vc ls host`, `vc shot host "Ladle Editor" out.png`. Capture works on any
workspace, focused or not. Input sent to `host` is not isolated, by definition.

## Rules

A box ignores your Hyprland config, so per-app rules never reach the app under
test; it just fills the box's output. Inside the box, every window gets
`render_unfocused` so captures are never stale, `--fullscreen` fullscreens them,
and `--rule` appends an `hl.window_rule{…}` line.

For its own window on the host, `vc` adds transient rules before the box starts,
so placement happens at map time and never disturbs your layout. Default is
`special:vcbox`; `--hide 4` uses a normal workspace. `hyprctl reload` clears them.

## Build

Needs `wayland-client`, `xkbcommon`, `zlib`, and Hyprland on the host.

```sh
make            # -> ./vc
make install    # symlink ~/.local/bin/vc -> ./vc, so it tracks every rebuild
make test       # 12-check smoke suite
```

The suite's main assertion is that host window class, workspace and size are
identical before and after, so the isolation claim stays proven. Protocol glue
is generated from XML into `build/`; a protocol bump is `make clean`.

## Limitations

- **No touch.** There is no virtual-touch Wayland protocol, and uinput would
  create a device on the real seat.
- A box always has a host window — Hyprland will not start with only a headless
  backend — it is just never anywhere visible.
- Hyprland only. The box is a nested Hyprland instance and placement uses
  `hyprctl`.

## Licence

GPL-2.0-only. See [LICENSE](LICENSE).
