# freak

**Wayland gamma control that doesn't care where you log in from.**

`freak` controls red, orange, green, blue, contrast and gamma in real time on
wlroots-based Wayland compositors (sway, river, hyprland, mango, niri-mango,
wayfire, …) through the `zwlr_gamma_control_v1` protocol — from a TUI, from a
CLI, from a script, or from another machine over SSH.

## Why

Most Wayland gamma tools assume you are sitting at the machine looking at a
usable screen inside a graphical session. That assumption breaks exactly when
you need brightness control:

- **The screen is too dim to read.** You can't open the brightness applet
  because you can't see the brightness applet.
- **You are on a TTY.** No desktop, no tray icon.
- **You are SSH'd in.** You still need to push the gamma up so whoever is at
  the keyboard can see again.
- **You are on a handheld** with no keyboard attached and want something that
  stays up across sessions and reboots.

```sh
ssh you@thatbox
freakctl reset            # back to neutral
freakctl set gamma 95     # crank it
freakctl load warm
```

The compositor updates its ramps immediately. No portal, no D-Bus, no "please
log in graphically first".

## Build

```sh
make
sudo make install                 # /usr/local/bin
sudo make install PREFIX=/usr     # if you use the shipped systemd unit
```

Build needs `libwayland-client` and `wayland-scanner`; the protocol XML is
vendored in `protocol/`, so no `wlr-protocols` package is required. At runtime
`freak` needs libc, libwayland-client and libm — `freakctl` needs only libc.

## Use

```sh
freak                     # TUI
freak --daemon            # daemon: holds gamma, serves the socket
freakctl set red 88
```

Run as a systemd user service:

```sh
systemctl --user enable --now freak
```

If your compositor doesn't import the Wayland environment into systemd, do it
once:

```sh
systemctl --user import-environment WAYLAND_DISPLAY XDG_RUNTIME_DIR
systemctl --user restart freak
```

The compositor drops the gamma table the moment the client that set it
disconnects. That is why there is a daemon and no fire-and-forget command:
something has to hold the connection open. If no daemon is running, the TUI
takes the gamma controls itself **and** opens the socket, so `freakctl` over
SSH keeps working while the TUI is on screen.

## TUI keys

| Key     | Action                           |
| ------- | -------------------------------- |
| ← →     | adjust value                     |
| ↑ ↓     | select parameter                 |
| `r`     | reset all to 80% (neutral)       |
| `s`     | save preset                      |
| `p`     | preset panel                     |
| `1`–`9` | load preset by index             |
| `h`     | help                             |
| `c`     | config info                      |
| `q`     | quit                             |

In the preset panel: `Enter` loads, `Del` deletes, `Esc` closes.

## Value mapping

Sliders show 0–100%. Internally that maps to −3.0…+2.0 fed into the ramp
formula. Neutral (identity ramp) is at **80%**.

```
internal = (percent / 100) × 5 − 3
```

## Presets

Files in `~/.config/controlorfreak/presets/`, plain `key = value`:

```toml
red      = 86.0
orange   = 80.0
green    = 86.0
blue     = 86.0
contrast = 81.0
gamma    = 83.0
```

Bundled: `neutral`, `warm`, `cool`, `cinema`, `freakshow`, `CODE`,
`acerbudget`, `asus_oled`, `lenovolegions`. Current values are persisted to
`~/.config/controlorfreak/state.freak` and restored on start.

## IPC protocol

Line-based text over the unix socket `/tmp/controller.sucker`:

```
GET                          → JSON with current values
SET <field> <0-100>          → OK
RESET                        → OK
LOAD <preset>                → OK / ERROR: ...
SAVE <preset>                → OK / ERROR: ...
PRESETS                      → JSON array of preset names
```

Fields: `red` `orange` `green` `blue` `contrast` `gamma`.

## Source layout

| File | Contents |
| --- | --- |
| `main.c`   | argv; TUI or daemon |
| `tui.c`    | terminal UI — raw mode, ANSI truecolor, no curses |
| `daemon.c` | daemon mode: one poll loop over the wayland fd and the socket |
| `gamma.c`  | `zwlr_gamma_control_v1` and the ramp maths |
| `ipc.c`    | the line protocol, client and server side |
| `preset.c` | presets and persisted state |
| `params.c` | the six parameters and the percent mapping |
| `sway.c`   | asks sway/i3 to float the window |
| `ctl.c`    | `freakctl` |

## License

MIT.
