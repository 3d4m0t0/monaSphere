# Live browser via Chromium + PipeWire ScreenCast

monaSphere can show web video services (`live_app` URLs) by capturing a
**dedicated display output** (preferred) or a Chromium window through
**xdg-desktop-portal ScreenCast** + **PipeWire**, then uploading RGBA frames to
the Flat screen.

## Recommended: dedicated monitor (virtual or spare)

Host desktop stays clean: put Chromium fullscreen on a second / virtual output and
cast **that MONITOR** into the HMD.

### KDE Plasma (this project’s usual host)

**Do not use「仮想デスクトップ」(Virtual Desktops / workspaces).** That only switches
desktops on the **same** monitor — there is no per-desktop resolution.

#### Managed virtual display (default on Wayland)

On **Plasma Wayland**, monaSphere can start and stop a virtual output itself via
`krfb-virtualmonitor` (package **`krfb`**):

1. Install once: `sudo zypper in krfb` (or distro equivalent).
2. Leave defaults in `~/.config/monasphere/monasphere.conf`:

```
live_capture=monitor
live_virtual=1
live_virtual_size=1280x720
live_virtual_scale=2     # 1.0–3.0 — krfb Virtual MONITOR scale (2 = 200%)
live_chrome_scale=1      # Chromium --force-device-scale-factor (keep 1 when Virtual is scaled)
live_cursor_scale=1      # 1–6 stick cursor gain (exponential curve; lower = slower)
live_chrome_profile=default  # default = system Chrome logins; isolated = monasphere profile
live_output=             # filled with Virtual-monaSphere on first setup
live_restore_token=
# Per-span tokens (x1–x3). Bare live_restore_token= is x1.
# live_restore_token[x1]=
# live_restore_token[x2]=
# live_restore_token[x3]=
# Tools → Set up live capture registers these spans in order (default x1→x2→x3).
live_setup_spans=1,2,3
# HMD Roots menu — indexed slots (Name|URL). Empty slots are ignored; fill the next index to add.
live_app[0]=YouTube|https://www.youtube.com/
live_app[1]=
# HMD [アプリ] — single-window GUI on Virtual-monaSphere (Name|exec[|arg…]).
# App process exit stops ScreenCast. No seed; leave empty until you add entries.
live_desktop_app[0]=
# Titles: Name|x1…x3|arg…  (fixed width from live_virtual_size; height shrinks)
# e.g. live_virtual_size=3840x2160 → x1 3840×2160, x2 3840×1440, x3 3840×960
# live_desktop_app[0]=RetroArch|/usr/bin/retroarch
# live_desktop_app[0].title[0]=タイトル１|x1
# Args: one `|` field each, or shell-like in one field (spaces + ~ expanded):
# live_desktop_app[0].title[1]=Darius|x3|-f|-L|~/.config/retroarch/cores/mame2003_plus_libretro.so|~/.config/retroarch/roms/darius.zip
# live_desktop_app[0].title[1]=Darius|x3|-f -L ~/.config/retroarch/cores/mame2003_plus_libretro.so ~/.config/retroarch/roms/darius.zip
```

`live_virtual_scale` sets `krfb-virtualmonitor --scale` and kscreen (Plasma
output scale). On Wayland Chrome this is the **only** scale factor — same as
setting the display to 200% in System Settings. Do **not** also raise
`live_chrome_scale` or UI becomes ~scale² (e.g. virtual=2 + chrome=2 → ~400%).
Leave `live_chrome_scale=1` (default); Wayland omits `--force-device-scale-factor`
so Chromium follows the output. After changing scale/size, stop live → Set up
again. Conf is re-read on each setup/HMD start.

If the host portal reports `AvailableCursorModes=0` (no Embedded DE cursor in
the stream), Flat draws a software stick cursor so the pointer stays visible.

`live_chrome_profile=default` uses the normal Chrome profile (subscription logins).
Stop closes only the app window — it does not quit Chrome.

`live_chrome_scale` (`--force-device-scale-factor`) is ignored when a system Chrome is
already running (singleton). Prefer `live_virtual_scale` for size, or set
`live_chrome_profile=isolated` (separate logins in `~/.config/monasphere/chromium-yt`)
when device-scale must apply every launch.

Legacy `youtube_*` keys are renamed to `live_*` on startup (and
`Virtual-monaSphereYT` → `Virtual-monaSphere`; restore token is cleared when the
output name changes — re-run Set up).

3. **Tools → Set up live capture…** — host prep only:
   - forces a new portal picker (conf token is replaced only when Start returns a new one)
   - **creates** `krfb-virtualmonitor` (`Virtual-monaSphere-xN` per span) for this session
   - KWin place **before** launch; ScreenCast only after place succeeds
   - pick **that Virtual** and enable **Remember** / allow restore
     (KDE hides this unless `persist_mode` is accepted) → token written for that span
   - on Streaming success: **stops capture, closes Chrome, and destroys Virtual-***
   - if conf still has `live_restore_token[xN]=` empty after setup, the portal did not
     grant persistence — re-run Set up with Remember checked.
4. In the HMD file menu, open a **`[Name]`** from `live_app[N]=` (default `[YouTube]`)
   — recreates Virtual and restores ScreenCast with the saved token (no picker if
   still valid).
5. Leave the live app in the HMD (Options / ○ long) to stop Chromium / PipeWire
   and destroy the managed virtual display.

If place fails, setup stops with a log error (it will not open ScreenCast against
an empty Virtual-* while Chrome sits on the main desktop). Clear
`live_restore_token=` only if the portal keeps rejecting restore.

Resolution is `live_virtual_size` (e.g. `1920x1080` or `3840x2160`). Changing size
restarts the virtual output on the next setup. On recent KWin, the PipeWire
consumer’s preferred stream size can resize a virtual MONITOR — monaSphere
passes `live_virtual_size` so a 4K Virtual is not silently negotiated down to
1080p.

Disable app-managed creation with `live_virtual=0` if you prefer a spare panel
or a display you create yourself.

#### Or: spare physical monitor / manual virtual output

Plug any panel (or run `krfb-virtualmonitor` yourself), set its mode in
**System Settings → Display & Monitor**, set `live_virtual=0` and
`live_output=` to a name substring from `kscreen-doctor -o`, then run setup.

### GNOME (Wayland)

Managed `krfb-virtualmonitor` is **KDE-only**. On GNOME:

1. Set `live_virtual=0` and point `live_output=` at a spare MONITOR name
   substring (from Settings → Displays), **or** use window capture below.
2. **Tools → Set up live capture…** — monaSphere uses portal
   **RemoteDesktop + ScreenCast** so you get a screen/window picker **and**
   pointer inject via `ConnectToEIS` (libei). Approve both remote-control and
   the chosen MONITOR; enable Remember when offered.
3. Stick cursor uses the Embedded compositor cursor inside the capture.

Do not leave `live_virtual=1` on GNOME — setup fails with a clear log message.

### Fallback: window capture

```
live_capture=window
live_virtual=0
```

Then the portal picks the **monaSphereYT** Chromium window (legacy path). The
window may be parked off-screen after setup.

## In the HMD

Open the file menu → a **`[Name]`** entry from `live_app[N]=` (default `[YouTube]`).
Every live app is forced **borderless fullscreen** on the Virtual MONITOR.

| Control | Action |
|---------|--------|
| L stick | Move DE cursor on Virtual-* (speed ∝ mode width; gain: `live_cursor_scale`) |
| R stick | Scroll (vertical mouse wheel) |
| 十字 | Coarse cursor steps |
| × | Click |
| ○ short | Browser **Back** (YouTube / live browser only) |
| PS long | Open/close VR menu (monaSphere-wide; [アプリ] swallows via pad filter) |
| Options long | Also opens/closes VR menu |
| L3+R3 | Recenter (monaSphere-wide) |
| △ | Flat seat preset |

View look (R stick) and seek are disabled while a live app is open. ○ never ends live.
On **[アプリ]**, monaSphere grabs the physical pad and mirrors it through **uinput**
(`monaSphere Virtual Pad`): **PS long** opens the VR menu without reaching the guest;
short PS is injected after release. **L3+R3** recenters. If the filter fails to start,
fallback chords are **PS+R1 (menu) / PS+L1 (recenter)**.
Switching live↔live / live↔app / app↔app **reuses Virtual** when WxH matches
(spinner:「切替中」). A different span/size waits until Virtual is removed
(「Virtual 削除中」), then creates the next size and launches.

## Limits

- NVIDIA DMA-BUF import is **experimental** (OPAQUE_FD fallback, explicit DRM
  modifiers, armed frames). First hard failure permanently falls back to CPU RGBA
  for that session (driver import-poisoning). Set nothing special to try it.
- Capture is the **whole monitor** (or whole window). Not a single browser tab.
- Managed virtual display needs **Plasma Wayland + krfb**. GNOME / X11 use a spare
  MONITOR or WINDOW mode (`live_virtual=0`).
- Pointer: KDE uses KWin EIS; GNOME uses portal RemoteDesktop.ConnectToEIS.
- `restore_token` behaviour differs by desktop (GNOME / KDE / wlroots).
- DRM / EME titles that Chrome paints black stay black when captured.
- Flat OpenAL surround is temporarily off (OpenAL Soft HRTF SEGV on stop/play).
  Chrome audio uses the system sink (often the HMD) until that path is re-enabled.
- GStreamer is not used; only libpipewire + portal.
- Live Chromium defaults to `--ozone-platform=wayland` (picks up Virtual
  `live_virtual_scale` like normal desktop Chrome). Opt out with
  `MONASPHERE_YT_X11=1` if you need the old XWayland path.
- Wayland live needs KWin EIS (or portal EIS on GNOME). Pure XTest fallback
  cannot drive a Wayland Chrome window — if EIS fails, sticks look dead.
- System Chrome already running as X11 may keep the singleton on X11; quit
  Chrome fully (or use `live_chrome_profile=isolated`) when switching ozone.

## Config keys

| Key | Meaning |
|-----|---------|
| `live_capture` | `monitor` (default) or `window` |
| `live_virtual` | `1` = app starts/stops `krfb-virtualmonitor` (KDE Wayland only) |
| `live_virtual_size` | e.g. `1280x720` (managed virtual output `Virtual-monaSphere`) |
| `live_virtual_scale` | `1.0`–`3.0` Virtual MONITOR scale (default `2` = 200%) |
| `live_chrome_scale` | `1.0`–`3.0` Chromium device scale (default `1`) |
| `live_cursor_scale` | `1`–`6` stick/D-pad gain (default `1`; base speed scales with `live_virtual_size`) |
| `live_chrome_profile` | `default` (system logins) or `isolated` (scale always applies) |
| `live_output` | Output name substring for Chromium placement |
| `live_restore_token` | Alias of `live_restore_token[x1]` (legacy) |
| `live_restore_token[xN]` | Portal restore token for span N (N=1..3); refreshed after each Start at that size |
| `live_setup_spans` | e.g. `1,2,3` — spans registered in one **Tools → Set up** run (host approves each) |
| `live_app[N]` | `Name\|https://…` → HMD Roots `[Name]`; empty value = unused slot |
| `live_desktop_app[N]` | `Name\|/path/to/app[\|arg…]` → HMD `[アプリ]` list; process exit ends capture |
| `live_desktop_app[N].title[M]` | `Name\|x1…x3[\|arg…]` → titles (monitor span + launch args) |

### Desktop apps (`[アプリ]`)

Register single-window GUIs in conf, then open **`[アプリ]`** under Roots (below
`live_app` entries). monaSphere creates **Virtual-monaSphere**, launches the
command, places the new window fullscreen on that MONITOR, and restores
ScreenCast. When the launched process exits, capture stops (same as Options /
stop live). Multi-window or daemonizing launchers are out of scope for this
test path.

Titles use **monitor span** instead of raw WxH:

```
live_desktop_app[0]=RetroArch|/usr/bin/retroarch
live_desktop_app[0].title[0]=タイトル１|x1
live_desktop_app[0].title[1]=Darius|x3|-f|-L|~/.config/retroarch/cores/mame2003_plus_libretro.so|~/.config/retroarch/roms/darius.zip
```

Launch args after `xN` are separate `|` fields (or one shell-like field with spaces).
`~` is expanded. Do **not** put `-L /path/core.so /path/rom` as a single
un-split token without spaces handled — that used to pass one argv to RetroArch
so the core never loaded.

`x1` / `x2` / `x3` keep the **width** of `live_virtual_size` and shrink **height**
so wider aspects fit without exploding pixel count (4K-friendly):

| span | aspect | example (`3840x2160`) |
|------|--------|------------------------|
| x1 | 16:9 (base) | 3840×2160 |
| x2 | 8:3 | 3840×1440 |
| x3 | 4:1 | 3840×960 |

ScreenCast restore tokens are stored **per span** (`live_restore_token[x1]` … `[x3]`).
Each span uses a **distinct Virtual name** (`Virtual-monaSphere-x1` … `-x3`) so the
portal Remember grant is not overwritten when you auth another size (same name
used to make only the last-auth’d token work).
**Tools → Set up live capture (x1–x3)…** walks `live_setup_spans` (default `1,2,3`):
for each span it creates that Virtual, asks you to pick it with **Remember**,
verifies restore, then continues to the next (toast: “next: approve x2 on host”).
Re-run setup once after upgrading if tokens were granted under the old shared
`Virtual-monaSphere` name. Use `live_setup_spans=2,3` to register only those.
HMD navigation is **`[アプリ]` → App → Title`**. Omitting titles keeps a flat list
(A launches with x1).

While a desktop app is live, the guest is launched with **`LD_PRELOAD=libmonasphere_padhook.so`**
(no `/dev/uinput` or `input` group required). The hook filters SDL2 Guide/PS:
**PS long** → VR menu, **short PS** → app, **L3+R3** → recenter; while the VR menu is
open the host sends `BLOCK 1` so the guest sees a dead pad. If the hook library is
missing, monaSphere may fall back to grab+uinput or PS+R1/PS+L1 chords. Opening a
video while live / while Virtual exists waits until Virtual is confirmed gone, then
starts playback.

**[アプリ] exit procedure** (unchanged intent):
1. App process exit and/or ScreenCast Session.Closed → busy「終了中」
2. XR drops DMA, then `stop()` destroys Virtual and waits for kscreen clear
3. Only then open the [アプリ] menu

`Session.Closed` often arrives before `QProcess::finished`; both paths arm the
same return-to-[アプリ] flag so the menu is not skipped. Stop by process exit or
launching something else from the menu.

Apps that **recreate their window** on content start (e.g. RetroArch launcher →
core) stay on Virtual-monaSphere via a session KWin Window Rule (discovered
`wmclass`, **Force screen only**) plus a sticky `windowAdded` place script.
Fullscreen / borderless is left to the app (same as on the primary display);
monaSphere does not Force compositor fullscreen for `[アプリ]` (that caused a
windowed flash then a second FS transition).

If you switch between `monitor` and `window`, clear `live_restore_token=` and run
**Set up live capture…** again.
