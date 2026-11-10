# Live browser via Chrome / Firefox + PipeWire ScreenCast

monaSphere can show web video services (`live_app` URLs) by capturing a
**dedicated display output** (preferred) or a browser window through
**xdg-desktop-portal ScreenCast** + **PipeWire**, then uploading RGBA frames to
the Flat screen. Choose the browser with `live_browser=chrome|firefox`
(default chrome).

## Recommended: dedicated monitor (virtual or spare)

Host desktop stays clean: put the Live browser fullscreen on a second / virtual
output and cast **that MONITOR** into the HMD.

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
live_browser=chrome      # chrome|chromium|firefox — Live URL browser (default chrome)
live_chrome_scale=1      # Chromium --force-device-scale-factor (keep 1 when Virtual is scaled; Firefox ignores)
live_cursor_scale=1      # 1–6 stick cursor gain (exponential curve; lower = slower)
live_chrome_profile=default  # default = system browser logins; isolated = monasphere profile
# FSR1 for browser Live only (not [アプリ] / not file UI). 0=Off 1=UQ 2=Q 3=P (default).
# □ / X (menus closed) cycles Off→UQ→Q→P and writes this key. File FSR stays preferred_fsr / UI.
live_fsr=3
live_output=             # filled with Virtual-monaSphere on first setup
# One ScreenCast Remember token for Virtual-monaSphere (x2/x3 sizes reuse it).
live_restore_token=
# Tools → Set up uses x1 size once (ignored if missing; setup always auth once at x1).
live_setup_spans=1
# HMD Roots menu — indexed slots (Name|URL). Empty slots are ignored; fill the next index to add.
live_app[0]=Example|https://example.com/
live_app[1]=
# HMD [アプリ] — single-window GUI on Virtual-monaSphere (Name|exec[|arg…]).
# App process exit stops ScreenCast. No seed; leave empty until you add entries.
live_desktop_app[0]=
# Titles: Name|x1…x3|arg…  (fixed width from live_virtual_size; height shrinks)
# e.g. live_virtual_size=3840x2160 → x1 3840×2160, x2 3840×1440, x3 3840×960
# live_desktop_app[0]=App|/usr/bin/app
# live_desktop_app[0].title[0]=Title A|x1
# Args: one `|` field each, or shell-like in one field (spaces + ~ expanded):
# live_desktop_app[0].title[1]=Title B|x3|-flag|/path/to/data
# live_desktop_app[0].title[1]=Title B|x3|-flag ~/path/to/data
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

`live_browser=chrome` (default) launches Chrome/Chromium with `--app=`.
`live_browser=firefox` launches Firefox (`--new-window` / `--profile` when isolated).
Both try `--class=monaSphereYT` so KWin can place the Live window.

Firefox on a 200% Virtual (`live_virtual_scale=2`) hits a Wayland bug when the browser
is compositor-fullscreen and YouTube then goes fullscreen (player ~2×). Live therefore:
- starts Firefox with `--new-instance` (so `--class` / env are not dropped by remoting)
- sets `MOZ_ENABLE_WAYLAND=0` (XWayland; the bug is Wayland-only)
- uses KWin maximize + noborder instead of Force fullscreen

If system Firefox is already open and `live_chrome_profile=default`, Live falls back to
the isolated profile (`~/.config/monasphere/firefox-yt`) so a real second instance can
start — quit Firefox first to use system logins. Chrome still Force-fullscreen.

`live_chrome_profile=default` uses the normal browser profile (subscription logins).
Stop closes only the Live window — it does not quit the system browser.

`live_chrome_scale` (`--force-device-scale-factor`) is ignored when a system Chrome is
already running (singleton), and is unused for Firefox. Prefer `live_virtual_scale`
for size, or set `live_chrome_profile=isolated` (Chrome:
`~/.config/monasphere/chromium-yt`; Firefox: `~/.config/monasphere/firefox-yt`)
when a dedicated profile must apply every launch.

TODO (later): expose conf keys such as `live_browser` in the host UI.

Legacy `youtube_*` keys are renamed to `live_*` on startup (and
`Virtual-monaSphereYT` → `Virtual-monaSphere`; restore token is cleared when the
output name changes — re-run Set up).

3. **Tools → Set up live capture…** — host prep only:
   - **creates** `Virtual-monaSphere` once at **x1** size (`live_virtual_size`)
   - pick it with **Remember** → one `live_restore_token` (shared by later x2/x3 sizes)
   - silent restore verify, then stop / destroy Virtual
   - KWin place **before** launch; ScreenCast only after place succeeds
   - if conf still has `live_restore_token=` empty after setup, the portal did not
     grant persistence — re-run Set up with Remember checked.
4. In the HMD file menu, open a **`[Name]`** from `live_app[N]=`
   — recreates Virtual at the requested span size and restores ScreenCast with the
   same token (no picker if still valid).
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

monaSphere detects `XDG_CURRENT_DESKTOP` and branches:

#### Managed virtual (`live_virtual=1`, recommended)

Uses portal **RemoteDesktop + ScreenCast** with source type **VIRTUAL**
(Mutter `RecordVirtual` via xdg-desktop-portal-gnome). Unlike Plasma (krfb
creates `Virtual-*` first), Meta-* appears only **after** PipeWire negotiates
size on `Start`.

Procedure (portal + Mutter; KDE-parity goal — Virtual creation differs):

1. Install `xdg-desktop-portal-gnome` (and a current GNOME / Mutter).
2. Leave `live_virtual=1` (same conf as Plasma).
3. **Tools → Set up live capture…** — approve **remote control / virtual
   monitor** (no monitor list). Enable Remember when offered.
4. Meta-* stream → HMD shows the virtual wallpaper. Portal grant dialogs are
   moved off Meta-* onto the **primary** (they must not stay in the capture).
5. HMD pointer uses a **software stick cursor** (`cursor_mode=Hidden` + Flat
   overlay), not the desktop arrow — intentional while Embedded trails/flicker
   on VIRTUAL. Pad **clicks** stay disabled briefly until evacuate settles.
6. After Meta-* appears, Chrome (isolated X11) is placed onto Meta with
   `_NET_WM_FULLSCREEN_MONITORS` — that paint keeps the SHM stream alive.

**GNOME Virtual (monaSphere path):** call Mutter directly —

1. `org.gnome.Mutter.ScreenCast.CreateSession`
2. `RecordVirtual({is-platform: true, cursor-mode: hidden})` — **required**;
   xdg-desktop-portal-gnome omits `is-platform` (defaults false). Non-platform
   Meta shows in XRandR but XWayland/Chrome often never composites into the
   stream (wallpaper-only HMD).
3. `ScreenCast.Start` → `PipeWireStreamAdded`
4. `Mutter.RemoteDesktop` CreateSession/Start/`ConnectToEIS`
5. Connect default PipeWire socket to the node (SHM for Meta)
6. Place Chrome on Meta-0; enable EIS

**Portal RD+SC VIRTUAL** (not used for monaSphere GNOME virtual anymore; kept
for reference / spare MONITOR):

1. `RemoteDesktop.CreateSession` → `SelectDevices` → `SelectSources(VIRTUAL)` →
   `Start` → `ConnectToEIS` — portal never sets `is-platform`.

**Must I approve remote control every time?** Not always — a valid
`restore_token` can make `Start` succeed silently (`restore=yes`,
`Start Response=0`). When GNOME cannot restore (token spent, VIRTUAL source
gone, permission revoked), the live grant UI appears again. If that UI opens
on Meta-*, it is visible in the HMD and pad/EIS can operate it; X11 evacuate
cannot move gnome-shell Wayland surfaces.

Teardown evacuates windows from Meta before `Session.Close` (Mutter can log
you out if a window still references a removed logical monitor).

**Capture / DMA research (GNOME VIRTUAL):**

| Source | Finding |
|--------|---------|
| [Fjarr ADR 0041](https://fjarr.io/adr/0041-desktop-capture-dmabuf/) | **Meta- / RecordVirtual → prefer SHM**; virtual monitors have no KMS CRTC, DMA-BUF path not validated. Physical monitors: DMA-BUF + hold until consumer done. |
| [WebRTC SharedScreenCastStream](https://webrtc.googlesource.com/src) | Dequeue (keep newest), **ProcessBuffer, `queue_buffer` in the same `on_process`**. DMA via EGL import+readback on discrete GPUs — never mmap tiled. |
| [niri NVIDIA flicker](https://github.com/niri-wm/niri/issues/1432) | Screencast flicker on NVIDIA = missing frame-complete / explicit sync before reuse; fixed by wait-for-completion / SyncTimeline. |
| Mutter !3876 | Producer-side `SPA_META_SyncTimeline`; consumers must wait acquire / signal release. |
| [logical_monitor assert](https://github.com/ubuntu/Tiling-Assistant/issues/416) | Removing Meta-* while a window/extension still references it → gnome-shell ABRT → **logout**. |

monaSphere therefore uses **SHM-only** for GNOME `live_virtual=1`. Teardown still moves the cursor/windows off Meta-* before `Session.Close`.

#### Spare physical MONITOR (`live_virtual=0`)

1. Set `live_virtual=0` and `live_output=` to a Displays name substring.
2. Setup uses portal **RemoteDesktop + ScreenCast** MONITOR share + EIS.

Package-only installs on a Plasma session do **not** exercise Mutter APIs —
test on a GNOME Wayland session or VM.

### Fallback: window capture

```
live_capture=window
live_virtual=0
```

Then the portal picks the **monaSphereYT** Chromium window (legacy path). The
window may be parked off-screen after setup.

## In the HMD

Open the file menu → a **`[Name]`** entry from `live_app[N]=`.
Every live app is forced **borderless fullscreen** on the Virtual MONITOR.

| Control | Action |
|---------|--------|
| L stick | Move DE cursor on Virtual-* (speed ∝ mode width; gain: `live_cursor_scale`) |
| R stick | Scroll (vertical mouse wheel) |
| 十字 | Coarse cursor steps |
| × | Click |
| ○ short | Browser **Back** (live browser only; not `[アプリ]`) |
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

## Flat live audio (verified)

Field-checked routing (yt sink / Soft-first / pactl move) with Live ↔ `[アプリ]` ↔
VR 動画切替. Soft layout matches **file 2ch**: side-diff → Soft HRTF 4.0.
Do not reintroduce mute-competitors, Soft-before-route inversion, or
`pa_mainloop_iterate(block=1)` in Live helpers.

### Signal path

```
Chrome / [アプリ]
    │  PULSE_SINK / PIPEWIRE_NODE=monaSphereYt   (new processes)
    │  + pactl move-sink-input                   (shared Chrome / late sink-inputs)
    ▼
monaSphereYt  (null sink, device.class=filter)
    │  .monitor
    ▼
PulseMonitor  (S16 stereo @ 48 kHz, audio worker)
    ▼
LoudnessNormalizer  (audio_normalize=1; before upmix/HRTF; RMS toward -16 dBFS, peak -6 dBFS)
    ▼
StereoSurround  (L−R + 14 ms + HPF 180 Hz → BL/BR; gain = surround_gain)
    ▼
OpenAL Soft HRTF  4.0 (FL|FR|BL|BR)  →  HMD HDMI
    app name monaSphereHrtf  (never open Soft on monaSphereYt)
```

Same Soft + `StereoSurround` path as local file 2ch. Shared knobs:

| Knob | Conf / UI | Applies to |
|------|-----------|------------|
| File volume | HMD menu / conf `volume` | file only, Vol 0–100. 100 is the safe maximum |
| Live base | listener `1.0` | Live / `[アプリ]`. Not the file slider |
| Pre-HRTF trim | conf `hrtf_trim_db` (0..24, default `12`) | dB cut after the limiter, before upmix and HRTF |
| Rear upmix | conf `surround_gain` (0..2, default `0.8`) | file 2ch, Live, `[アプリ]` |
| Loudness | conf `audio_normalize` (`1`/`on` default, `0`/`off`), peak -6 dBFS | file, Live, `[アプリ]` (before upmix and HRTF) |

`surround_gain`, `audio_normalize`, and `hrtf_trim_db` have no HMD UI — edit
`~/.config/monasphere/monasphere.conf` and restart the HMD session.
Missing `audio_normalize` is written as `1`, and missing `hrtf_trim_db` as `12`,
on the next launch. Retired keys such as `volume_uncap` are commented out.

### Startup order (must keep)

1. **`enable_audio=true`** on Live / `[アプリ]` session (`host_window`).
2. **`PulseMonitor::ensure_yt_null_sink()`** before guest launch (`youtube_session`)
   so new processes can bind `PULSE_SINK=monaSphereYt`.
3. Guest starts (Chrome may be **shared** — `chrome_running=1`; env often ignored).
4. ScreenCast streaming → `audio_wanted`; XR calls **`request_start_live_monitor()`**.
5. **Soft first:** ensure yt sink → Soft configure / play / monitor record on the
   audio worker. **Do not** await browser route on this path.
6. **Route second:** detached worker (~30s, 500ms ticks) runs
   `PulseMonitor::route_browser_to_yt_sink()` via **`pactl` only** (no Pulse
   mainloop). Shared Chrome creates sink-inputs only after media plays.
7. Stop: `request_stop_live_monitor` / restore routing + unload null sink when
   Live ends; XR must not call Soft/Pulse on its own thread.

### Soft open rules

- `alcOpenDevice(nullptr)` with temporary `PULSE_SINK=<HMD>`; restore env after open.
- Passing the full sink name to `alcOpenDevice` can **hang** on PipeWire-Pulse
  (`live_op_busy` sticks; no Soft/peak logs).
- Soft identifies as `monaSphereHrtf` so route helpers never move it onto yt.

### Pulse / mainloop rules

- Live Pulse helpers use **timed** `pa_mainloop_prepare` / `poll` / `dispatch`.
  Never `pa_mainloop_iterate(block=1)` — idle block hung Soft start inside
  `drain()` after “routing browsers” (no Soft configure log).
- Browser move is **pactl** (`list sink-inputs` + `move-sink-input` + unmute on yt).
- **Do not mute** other HMD sink-inputs during route (Soft/Chrome race → silence).

### Code map

| Piece | Where |
|-------|--------|
| `enable_audio`, XR `request_start_live_monitor` | `apps/vrp_player/host_window.cpp` |
| yt sink before guest, `audio_wanted` | `apps/vrp_player/youtube_session.cpp` |
| Soft first + route worker | `src/audio/audio_player.cpp` |
| Soft open / HRTF | `src/audio/openal_spatial.cpp` |
| null sink, pactl route, monitor record | `src/audio/pulse_monitor.cpp` |
| Chrome `PULSE_SINK` / shared warning | `src/capture/chromium_yt.cpp` |

### Healthy log order

`Soft first (monitor=…)` → `configuring Soft HRTF` →
`Soft configure ok (4.0 HRTF, stereo side-diff upmix)` →
`live route worker start` → `no browser sink-input yet` and/or
`moved N` / `route retry moved` → feed line with `upmix=1 gain=…`.

Silent Live: Soft never opened (hang/busy) or Chrome never landed on
`monaSphereYt` (shared session + no move yet — play media and wait for route).

## Limits

- NVIDIA DMA-BUF import is **experimental** (OPAQUE_FD fallback, explicit DRM
  modifiers, armed frames). First hard failure permanently falls back to CPU RGBA
  for that session (driver import-poisoning). Set nothing special to try it.
- Capture is the **whole monitor** (or whole window). Not a single browser tab.
- Managed virtual: Plasma = `krfb-virtualmonitor`; GNOME = portal **VIRTUAL** /
  Mutter `RecordVirtual`. Spare MONITOR: `live_virtual=0` + `live_output=`.
- Pointer: KDE uses KWin EIS; GNOME uses portal RemoteDesktop.ConnectToEIS.
- `restore_token` behaviour differs by desktop (GNOME / KDE / wlroots).
- DRM / EME titles that Chrome paints black stay black when captured.
- Live audio design: see **Flat live audio (verified)** above.
- GStreamer is not used; only libpipewire + portal.
- Live Chromium defaults to `--ozone-platform=wayland` (picks up Virtual
  `live_virtual_scale` like normal desktop Chrome). Opt out with
  `MONASPHERE_YT_X11=1` if you need the old XWayland path.
- Wayland live needs KWin EIS (or portal EIS on GNOME). Pure XTest fallback
  cannot drive a Wayland Chrome window — if EIS fails, sticks look dead.
- System Chrome already running as X11 may keep the singleton on X11; quit
  Chrome fully (or use `live_chrome_profile=isolated`) when switching ozone.

## Config keys

`#` starts a comment. On launch, a line whose key is not used, or whose value would be ignored, is rewritten as a comment (`# ` plus the old line). That drops retired keys such as `thumb_count`, `thumb_edge`, `thumb_jpeg`, and `volume_uncap` without deleting the text.

| Key | Meaning |
|-----|---------|
| `surround_gain` | Stereo→4.0 rear level for **file / Live / [アプリ]** (0..2, default `0.8`) |
| `audio_normalize` | Loudness match before upmix and HRTF (`1` or `on` default, `0` or `off`). Peak ceiling -6 dBFS. Listener gain stays as-is |
| `volume` | File slider 0–1 (Vol 0–100). Live does not follow this |
| `hrtf_trim_db` | Decibels cut before upmix and HRTF (0..24, default `12`). `0` is no extra cut. Missing key is written as `12` |
| `live_capture` | `monitor` (default) or `window` |
| `live_virtual` | `1` = app starts/stops `krfb-virtualmonitor` (KDE Wayland only) |
| `live_virtual_size` | e.g. `1280x720` (managed virtual output `Virtual-monaSphere`) |
| `live_virtual_scale` | `1.0`–`3.0` Virtual MONITOR scale (default `2` = 200%) |
| `live_browser` | `chrome` / `chromium` (default) or `firefox` |
| `live_chrome_scale` | `1.0`–`3.0` Chromium device scale (default `1`; Firefox ignores) |
| `live_cursor_scale` | `1`–`6` stick/D-pad gain (default `1`; base speed scales with `live_virtual_size`) |
| `live_chrome_profile` | `default` (system logins) or `isolated` (monaSphere profile) |
| `live_output` | Output name substring for Chromium placement |
| `live_restore_token` | ScreenCast Remember token for `Virtual-monaSphere` (shared by all spans) |
| `live_setup_spans` | Kept for compat; setup always authenticates once at x1 size |
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
live_desktop_app[0]=App|/usr/bin/app
live_desktop_app[0].title[0]=Title A|x1
live_desktop_app[0].title[1]=Title B|x3|-flag|/path/to/data
```

Launch args after `xN` are separate `|` fields (or one shell-like field with spaces).
`~` is expanded. Do **not** glue several path arguments into one `|` field without
spaces — they become a single argv and the app may never see the intended files.

`x1` / `x2` / `x3` keep the **width** of `live_virtual_size` and shrink **height**
so wider aspects fit without exploding pixel count (4K-friendly):

| span | aspect | example (`3840x2160`) |
|------|--------|------------------------|
| x1 | 16:9 (base) | 3840×2160 |
| x2 | 8:3 | 3840×1440 |
| x3 | 4:1 | 3840×960 |

ScreenCast restore uses **one token** for the fixed name **`Virtual-monaSphere`**.
**Tools → Set up live capture…** authenticates once at x1 size; x2/x3 only change
resolution and reuse that token (re-auth only if the portal rejects restore).
Old `live_restore_token[x1]` … `[x3]` lines are not settings. If `live_restore_token=`
is empty, x1 then x2 then x3 is copied into it on launch, and the span lines are
commented out.
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

Apps that **recreate their window** on content start (launcher → child process)
stay on Virtual-monaSphere via a session KWin Window Rule (discovered
`wmclass`, **Force screen only**) plus a sticky `windowAdded` place script.
Fullscreen / borderless is left to the app (same as on the primary display);
monaSphere does not Force compositor fullscreen for `[アプリ]` (that caused a
windowed flash then a second FS transition).

If you switch between `monitor` and `window`, clear `live_restore_token=` and run
**Set up live capture…** again.
