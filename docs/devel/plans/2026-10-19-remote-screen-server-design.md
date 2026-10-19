# Built-in screen server (prestonbrown/helixscreen#1825)

Status: design for approval. Delete this file in the change that ships the work (`docs/CLAUDE.md`).

HelixScreen serves its own UI as a webcam that Mainsail and Fluidd can show, and accepts taps and drags back from a browser. It runs only where the firmware ships no screen server; the U1 on PAXX (`fb-http`) and the AD5X on ZMOD keep using theirs.


## Decisions (Preston, 2026-10-19)

- **Pairing on boards with nothing in front of Moonraker (AD5M, CC1):** least friction wins. The printer's screen shows a short numeric code (6 digits); the control page asks for it once and remembers the browser with a long-lived cookie. No QR code: the control page usually runs on a desktop browser, where a QR code means fetching a phone. View works without pairing.
- **Mainsail/Fluidd entry:** one webcam entry. An iframe of the control page when control is on, a plain snapshot feed when view only.
- **Routing:** benchmark both before choosing. Measure nginx-direct against proxying through the `helix_print` Moonraker plugin on the weakest hosts we ship (AD5M or CC1, a K1) and one strong one (Pi 4 or K2): Moonraker process CPU, Klipper timer margin (`MCU stats` / timer-too-close incidence) during a moving print, snapshot latency and achieved fps. Pick nginx unless the plugin path is within noise on the weak hosts, and keep the plugin path only where no nginx exists if it measures well there.
- **"Remote control active" indicator:** yes, in phase 3.

## Settled

- Built into the `helix-screen` binary. No external process, no init script.
- Remote control (tap, drag, long-press) is in scope from the first release.
- Off by default. Two settings: view, then control as a second opt-in.
- Nothing runs and nothing is allocated while no browser is looking.

## What exists

| Piece | Where | Reuse |
|---|---|---|
| Frame tap | `src/application/display_manager.cpp#install_color_transform_hook` builds a `RemoteScreenFrame` per flushed dirty rect (post color transform) and hands it to `RemoteScreenManager` | As is |
| Sink interface | `include/remote_screen_sink.h#RemoteScreenSink` (`start`/`stop`/`wants_frames`/`on_frame`, main thread) | The new sink implements it |
| fb0 mirror | `src/application/remote_screen_fb0_sink.cpp#Fb0MailboxSink::on_frame`: the dirty-rect blit with RGB565/BGRA conversion and the `px_map_len` / `px_map_x` bounds rules | Extract the blit (below) |
| Synthetic pointer | `include/remote_pointer.h#RemotePointer`: a second LVGL pointer indev driven through LVGL's own input pipeline, so drag, long-press and scroll arbitration behave as under a finger | Move out of `src/remote/` |
| ctl pointer | `src/remote/remote_control_server.cpp#RemoteControlServer::apply_pointer_state` (wakes the display, waits two indev reads) | Same wait rule |
| HTTP server | libhv is built with `WITH_HTTP_SERVER=yes` (`lib/libhv/config.mk`); `src/api/mock_http_file_server.cpp` is a working `hv::HttpServer` with the port-0 binding workaround | Server base |
| Webcam registration | `scripts/lib/installer/camera.sh#_moonraker_migrate_webcams` upserts through Moonraker's `/server/webcams/item`; `#_tune_webcam_proxy_block` edits nginx with tagged lines, `nginx -t`, and revert | Pattern for the nginx edit |

Two things in place today do not fit and change:

- `src/remote/` is compiled out of release builds (`ENABLE_REMOTE_CONTROL ?= no` under `HELIX_PACKAGING=1`, `Makefile`). `RemotePointer` moves to `src/application/` so a release binary has it; the ctl server keeps calling it.
- No JPEG encoder ships on any device. `libturbojpeg` is only `dlopen`ed for camera decode (`src/system/camera_stream.cpp`), cameras are compiled off on every embedded target (`lv_conf.h#HELIX_HAS_CAMERA`), and `lib/libjpeg-turbo` is built for Android only. We vendor `stb_image_write.h` beside the existing `lib/stb/stb_image.h`: header-only, no exceptions, baseline JPEG, no per-arch build. libturbojpeg is not worth a cross build for five frames a second.

`fb-http` on the U1 serves PNG snapshots (poll, no stream) and injects touch by writing `/dev/input/event0`; its page (`/usr/local/share/fb-http/html/index.html` on the U1) POSTs `down`/`move`/`up` in image coordinates (`docs/devel/printers/SNAPMAKER_U1_SUPPORT.md` § "Remote screen / gui webcam streaming"). The repo does not document ZMOD's screen feed.

## Platforms

| Target | Display | Firmware screen server | nginx in front of Moonraker | Plan |
|---|---|---|---|---|
| K1 / K1C / K1 Max, Creator 5 Pro (`mips`) | fbdev, 480x800 panel, software-rotated to 800x480 | none | Helper Script installs one (Fluidd 4408, Mainsail 4409); config path to confirm on the rig | built-in, nginx route |
| K2 family (`k2`) | fbdev 480x800 portrait, no `/dev/dri` (`CREALITY_K2_SUPPORT.md`) | none | stock, `/etc/nginx/nginx.conf`, port 4408; `camera.sh` already edits it | built-in, nginx route |
| AD5M (`ad5m`) on Forge-X / Klipper Mod | fbdev 800x480 32bpp | none known | usually none (`INSTALLER.md` § update_manager skips AD5M) | built-in, LAN bind |
| AD5M on ZMOD | fbdev | ZMOD's, if it ships one on AD5M (verify) | as ZMOD ships | firmware's if present |
| AD5X on ZMOD | fbdev 800x480 | ZMOD's | nginx serves Fluidd on 80, does not proxy Moonraker (`FLASHFORGE_AD5X_SUPPORT.md`) | firmware's |
| AD5X on Forge-X | fbdev | none known | verify | built-in |
| CC1 (`cc1`, `yocto`) | fbdev 480x272 | none | verify; the OpenCentauri recipe owns its own integration | built-in, LAN bind unless nginx |
| Pi / Pi32, x86 | DRM (fbdev variants) | none | MainsailOS/KIAUH: `/etc/nginx/sites-available/{mainsail,fluidd}` | built-in, nginx route |
| QIDI | fbdev or DRM | none | stock Fluidd stack; verify | built-in |
| U1 on PAXX | DRM 480x320 | `fb-http` via `hooks-snapmaker-u1.sh#start_remote_screen` | firmware `/screen/` route | firmware's, unchanged |
| U1 on stock firmware | DRM | none | firmware nginx | built-in |

Excluding a firmware server is a platform-hook decision, not a printer-name check: a hook that starts or finds one exports `HELIX_REMOTE_SCREEN=firmware` (the U1 hook already exports `HELIX_REMOTE_SCREEN_FB0`, which implies it). The app then hides both settings and never starts the server.

## Design

### Capture: `ScreenServerSink : RemoteScreenSink`

- **Shadow frame.** One heap frame in the flush's own pixel format, sized to the **logical** resolution (`RemoteScreenFrame::disp_w/h`). The hook wraps the backend's `flush_cb`, and the fbdev driver rotates inside that callback (`lv_linux_fbdev.c#flush_cb`), so the rect and `px_map` the hook sees are logical. The served image is upright as the user sees the panel.
- **One blit.** `Fb0MailboxSink::on_frame`'s copy loop becomes a pure function, `blit_remote_frame(const RemoteScreenFrame&, uint8_t* dst, size_t dst_len, uint32_t dst_stride, int dst_w, int dst_h, int dst_bpp)`, used by both sinks. The fb0 sink then differs only in where `dst` comes from (mmap vs heap).
- **No viewer, no work.** `wants_frames()` is true only within `kViewerIdleMs` (10 s) of the last request. The shadow is allocated on the first request and freed when the viewer goes idle. Because only dirty rects arrive, the first request after idle queues a full-screen invalidate on the UI thread (`ui_queue_update`) and waits up to 500 ms for the frame generation to advance; on timeout it serves the stale frame rather than failing.
- **Locking.** A mutex guards the shadow and a generation counter. The flush hook holds it for the memcpy of one rect. The HTTP thread holds it only while converting the shadow to an RGB888 encode buffer, then encodes outside the lock.
- **Encode on request, rate-capped.** The last JPEG is cached with its generation. A request inside the rate window, or with an unchanged generation, gets the cache, so a static screen costs one encode total. Caps: 5 fps idle; while printing, 2 fps on multi-core hosts and 1 fps on a constrained host (`MemoryInfo::is_constrained_device()` or a single core: the AD5M, CC1 and K1 class), since host work during motion is what trips TTC on these boards (`FLASHFORGE_AD5X_SUPPORT.md` § "Host work during toolhead motion"). Quality 70.
- **Memory while a viewer is active** (zero otherwise): shadow `W*H*bpp` + RGB888 buffer `W*H*3` + cached JPEG (~50-120 KB). 800x480 at 32 bpp: about 2.8 MB. 480x272 (CC1): about 0.9 MB. 1280x800 Pi: about 7.2 MB.
- **Sleep.** A sleeping display stops flushing; the server serves the last frame with the screensaver state as it was drawn. A remote tap wakes the display exactly as a physical one does, through the same path ctl uses.

### HTTP: `ScreenHttpServer`

libhv `hv::HttpServer`, one worker thread, its own port. Routes, all relative to the mount point:

| Route | Method | Purpose |
|---|---|---|
| `snapshot.jpg` | GET | One JPEG. Ignores query strings (Mainsail adds a cache-buster). `Cache-Control: no-store` |
| `stream.mjpg` | GET | `multipart/x-mixed-replace` MJPEG at the same cap (phase 3) |
| `touch` | POST | `{type: down|move|up, x, y}` in snapshot-image pixels, fb-http's event set |
| `` (index) | GET | The control page |
| `info` | GET | `{w, h, control: bool}` so the page sizes itself and hides control when off |

**Which webcam service type needs what** (Moonraker `/server/webcams/item` fields):

- `mjpegstreamer-adaptive` (Mainsail and Fluidd): polls `snapshot_url` at `target_fps`; `stream_url` unused. Phase 1 registers this, `target_fps: 2`.
- `mjpegstreamer` (both, the default type): `stream_url` must be a multipart MJPEG stream; `snapshot_url` is used for snapshots and timelapse. Needs `stream.mjpg` (phase 3).
- `iframe` (both): `stream_url` is a page shown in an iframe. Pointing it at the control page puts tap-to-control inside the Mainsail/Fluidd webcam panel (open question 2).

### Control

- `touch` POSTs are handed to the UI thread and drive `RemotePointer` with the same "wait two indev reads" rule as ctl, so a fast down/up pair is never coalesced away.
- **Coordinates.** The served image is logical-orientation pixels, but LVGL applies `lv_display_rotate_point` (physical to logical) to every pointer indev. So an image point goes to `RemotePointer` through the inverse of that rotation, and lands correctly at 0/90/180/270. One pure function owns the mapping (`remote_image_to_indev_point`); the ctl pointer uses it too, since it otherwise feeds logical coordinates into a rotated pipeline. Unlike `fb-http`, there is no `/dev/input` round trip and no double rotation (`SNAPMAKER_U1_SUPPORT.md` § "Rotation caveat").
- **The page** is one static HTML file, `assets/remote_screen/index.html`, served by us: an `<img>` refreshed from `snapshot.jpg`, pointer events mapped to image pixels, POSTed to `touch` as JSON. Our own file, written against fb-http's event model, not a copy of fb-http's page.
- Control off: `touch` returns 403 and the page shows view-only.

### Security defaults

1. **Off until the user turns it on** in Settings, on the device.
2. **Where nginx fronts Moonraker** (K1, K2, Pi, x86, likely QIDI and stock U1): the server binds `127.0.0.1` only. The installer adds a tagged `location /helixscreen/screen/` block in each server block that proxies Moonraker, with `auth_request` to Moonraker's `/access/user`. Whoever Moonraker would let send G-code (trusted client, valid API key or JWT, or authorization disabled) may view and control; nobody else can reach the port. `access_log off` and `proxy_buffering off` as on the K2 `/webcam/` block, for the tmpfs reasons `camera.sh` records. If `nginx -V` lacks `auth_request`, or `nginx -t` rejects the edit, the installer reverts and falls through to rule 3.
   Consequence: a Mainsail user with Moonraker login on and an untrusted IP sees no screen feed, because an `<img>` request carries no JWT. That matches Moonraker's own policy.
3. **Where nothing fronts Moonraker** (AD5M, CC1): the server binds the LAN on port 7130. View needs only the setting. Control additionally needs pairing: the settings row shows a 6-digit code, the control page asks for it once, and a correct code earns the browser a random 128-bit token in a cookie (`HttpOnly; SameSite=Strict`, one year). Up to 8 browsers stay paired, least recently used evicted. Wrong codes are limited to 5 per minute, and 50 cumulative failures rotate the code. "Forget paired browsers" clears every token and rotates the code. Physical access to the screen is the credential.
4. **CSRF**, both modes: `touch` accepts only `Content-Type: application/json` with an `X-Helix-Touch: 1` header and sends no CORS headers, so a cross-origin page cannot trigger a preflight-free POST. Requests whose `Origin` does not match `Host` are rejected.
5. While a browser has control, the status bar shows a small remote-control icon (phase 3), so the person at the printer knows.

### Settings and registration

`RemoteScreenSettingsManager` alongside the other `*_settings_manager.h`, `Scope::Global`:

| Key | Default | Notes |
|---|---|---|
| `/remote_screen/enabled` | false | view |
| `/remote_screen/control` | false | settable only when enabled |
| `/remote_screen/pair_code` | "" | 6 digits, generated on first control enable (LAN mode) |
| `/remote_screen/paired` | [] | paired browser tokens, at most 8 |
| `/remote_screen/port` | 7130 | not in the UI |

Toggling starts and stops the server live, no restart. The **app** registers and removes its webcam through Moonraker (`server.webcams.post_item` / `delete_item`, new calls on the webcams sub-API next to discovery's `server.webcams.list`), on toggle and on each Moonraker connect, so the entry tracks the setting and the host's current IP. With an nginx route it registers relative URLs (`/helixscreen/screen/snapshot.jpg`); without one, `http://<lan-ip>:7130/...`. The app learns which mode it is in from `config/.remote_screen_route`, which the installer writes (the mount path) only after `nginx -t` passes.

Installer: a `remote_screen.sh` module beside `camera.sh`, reusing its nginx find/validate/revert helpers rather than copying them; marker `.nginx_remote_screen` added to `HELIX_USER_CONFIG_FILES` (`scripts/lib/installer/platform.sh`). Uninstaller strips the tagged block, reloads nginx, and deletes the `HelixScreen Screen` webcam via REST in case the app never ran to remove it.

## Tests

| Unit | How |
|---|---|
| `blit_remote_frame` | Table test over src {RGB565, BGRA} x dst bpp x partial/direct origin x an OOB rect, with `px_map_len` bounds. Existing `Fb0MailboxSink` tests must pass unchanged on the extracted function |
| Shadow resize | A frame whose `disp_w/h` differ from the shadow's reallocates it before the blit |
| Viewer gating | Injected clock: `wants_frames()` false before any request, true after, false after 10 s, buffers freed |
| Rate cap and cache | Injected clock and generation: second request inside the window encodes nothing; unchanged generation encodes nothing; print-state cap selects the lower rate |
| Encoder | Encode a known pattern, decode with `stb_image`, check dims and per-block mean color within tolerance |
| Touch mapping | `remote_image_to_indev_point` round-trips through `lv_display_rotate_point` at all four rotations |
| Touch through LVGL | `LVGLTestFixture`: POST down/up at a button center, assert `LV_EVENT_CLICKED`; down/move/up across a list, assert scroll and no click |
| HTTP | Server on an ephemeral port (the `mock_http_file_server.cpp` port-0 workaround): 403 with control off, 403 without the CSRF header, 403 on Origin mismatch, 401 on a bad token in LAN mode, JPEG `Content-Type` on snapshot |
| Installer | bats with `HELIX_NGINX_CONF` redirected: block inserted once per Moonraker server block, idempotent, reverted on a failing `nginx -t` stub, stripped on uninstall, untouched configs left alone |

## Phasing

1. **View.** `blit_remote_frame` extraction, `ScreenServerSink`, encoder, `ScreenHttpServer` with `snapshot.jpg`/`info`, the enable setting, app-side webcam registration (`mjpegstreamer-adaptive`), installer nginx module. Ships nothing user-visible until Settings shows the row.
2. **Control.** `RemotePointer` move, `touch` endpoint, control page, CSRF rules, pairing code for LAN mode, control setting. Lands with or right after 1, same release.
3. **Polish.** `stream.mjpg` (enables `mjpegstreamer`), remote-active indicator.

To verify on hardware before phase 1 lands, not decisions: the K1 Helper Script nginx path; `auth_request` in the K1/K2 nginx builds; whether Moonraker trusts `X-Real-IP` from nginx for `trusted_clients` (else every request appears to come from 127.0.0.1 and passes); ZMOD's screen server on the AD5M; fb-http's exact `touch` body, read from the U1's `index.html`.

## Open questions

1. **Control where nothing fronts Moonraker (AD5M, CC1).** Recommend shipping it behind the on-screen pairing token above. The alternative is view-only on those boards, which leaves the AD5M (the most-installed target) without the feature's point.
2. **One webcam entry or two when control is on.** Recommend one entry, `HelixScreen Screen`, registered as `iframe` pointing at the control page when control is on and as `mjpegstreamer-adaptive` when view-only. Two entries clutter the webcam picker and the iframe entry alone gives tap-to-control inside Mainsail/Fluidd.
3. **Route through nginx or through the `helix_print` Moonraker plugin.** A plugin endpoint would sit behind Moonraker's own auth on every platform, nginx or not. Recommend nginx: the plugin is an optional install, and relaying JPEGs through Python's single-threaded event loop on a 110 MB MIPS or Cortex-A7 host is exactly the host load that causes TTC.
4. **Remote-active indicator on the device.** Recommend yes, in phase 3: a person standing at the printer should be able to see that someone else is driving it.
