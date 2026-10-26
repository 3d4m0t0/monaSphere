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
# HMD Roots menu — indexed slots (Name|URL). Empty slots are ignored; fill the next index to add.
live_app[0]=YouTube|https://www.youtube.com/
live_app[1]=
```

`live_virtual_scale` sets `krfb-virtualmonitor --scale` (and kscreen). Keep
`live_chrome_scale` near `1` so UI is not double-scaled. After changing
scale/size, stop live capture → Set up again (recreates the virtual output).

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
   - **creates** `krfb-virtualmonitor` (`Virtual-monaSphere`) for this session
   - KWin place **before** launch; ScreenCast only after place succeeds
   - pick **`Virtual-monaSphere`** and enable **Remember** / allow restore
     (KDE hides this unless `persist_mode` is accepted) → token written to conf
   - on Streaming success: **stops capture, closes Chrome, and destroys Virtual-***
   - if conf still has `live_restore_token=` empty after setup, the portal did not
     grant persistence — re-run Set up with Remember checked.
4. In the HMD file menu, open a **`[Name]`** from `live_app[N]=` (default `[YouTube]`)
   — recreates Virtual and restores ScreenCast with the saved token (no picker if
   still valid).
5. Leave the live app in the HMD (Options / ○ long) to stop Chromium / PipeWire
   and destroy the managed virtual display.

If place fails, setup stops with a log error (it will not open ScreenCast against
an empty Virtual-* while Chrome sits on the main desktop). Clear
`live_restore_token=` only if the portal keeps rejecting restore.

Resolution is `live_virtual_size` (e.g. `1920x1080`). Changing size restarts the
virtual output on the next setup.

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
| L stick | Move DE cursor on Virtual-* (slow exponential fine band; gain: `live_cursor_scale`) |
| R stick | Scroll (vertical mouse wheel) |
| 十字 | Coarse cursor steps |
| × | Click |
| ○ short | Browser **Back** |
| ○ long (~0.65s) | Close live app → file menu |
| △ | Flat seat preset |

View look (R stick) and seek / menu toggle are disabled while a live app is open.

HMD Options / ○ long-press ends the stream, browser, and destroys any
app-owned virtual display.

## Limits

- Capture is the **whole monitor** (or whole window). Not a single browser tab.
- Managed virtual display needs **Plasma Wayland + krfb**. GNOME / X11 use a spare
  MONITOR or WINDOW mode (`live_virtual=0`).
- Pointer: KDE uses KWin EIS; GNOME uses portal RemoteDesktop.ConnectToEIS.
- `restore_token` behaviour differs by desktop (GNOME / KDE / wlroots).
- DRM / EME titles that Chrome paints black stay black when captured.
- Flat OpenAL surround is temporarily off (OpenAL Soft HRTF SEGV on stop/play).
  Chrome audio uses the system sink (often the HMD) until that path is re-enabled.
- GStreamer is not used; only libpipewire + portal.
- Optional: `MONASPHERE_YT_WAYLAND=1` forces Chromium `--ozone-platform=wayland`.

## Config keys

| Key | Meaning |
|-----|---------|
| `live_capture` | `monitor` (default) or `window` |
| `live_virtual` | `1` = app starts/stops `krfb-virtualmonitor` (KDE Wayland only) |
| `live_virtual_size` | e.g. `1280x720` (managed virtual output `Virtual-monaSphere`) |
| `live_virtual_scale` | `1.0`–`3.0` Virtual MONITOR scale (default `2` = 200%) |
| `live_chrome_scale` | `1.0`–`3.0` Chromium device scale (default `1`) |
| `live_cursor_scale` | `1`–`6` stick cursor gain (default `1`) |
| `live_chrome_profile` | `default` (system logins) or `isolated` (scale always applies) |
| `live_output` | Output name substring for Chromium placement |
| `live_restore_token` | Portal restore token (refreshed after each Start) |
| `live_app[N]` | `Name\|https://…` → HMD Roots `[Name]`; empty value = unused slot |

If you switch between `monitor` and `window`, clear `live_restore_token=` and run
**Set up live capture…** again.
