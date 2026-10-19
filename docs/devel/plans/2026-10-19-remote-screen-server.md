# Built-in Screen Server Implementation Plan (prestonbrown/helixscreen#1825)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. § Verification Cadence in the root `CLAUDE.md` overrides both skills: tags while building, one review + one scoped mutation run + one `make land-gate` per batch at the finish.

**Goal:** `helix-screen` serves its own UI as a Mainsail/Fluidd webcam and accepts taps and drags back from a browser, on every platform whose firmware ships no screen server.

**Architecture:** A new `RemoteScreenSink` (`ScreenServerSink`) copies flushed dirty rects into a shadow frame held by a thread-safe `ScreenFrameStore`. A libhv `ScreenHttpServer` encodes the shadow to JPEG on request (rate-capped, cached by generation) and turns `touch` POSTs into `RemotePointer` states on the UI thread. Where nginx fronts Moonraker the server binds loopback behind an installer-managed `auth_request` location. Elsewhere it binds the LAN, and control needs a 6-digit pairing code shown on the device.

**Tech Stack:** C++17, LVGL 9.5, libhv `hv::HttpServer`, vendored `stb_image_write.h`, POSIX shell installer modules + bats, Catch2.

**Spec:** `docs/devel/plans/2026-10-19-remote-screen-server-design.md`

## Global Constraints

- Off by default. View (`/remote_screen/enabled`) then control (`/remote_screen/control`) as a second opt-in; control is settable only while view is on.
- Nothing runs and nothing is allocated while no browser is looking: `wants_frames()` is true only within `kViewerIdleMs = 10000` of the last request; buffers are freed on idle.
- Rate caps: 5 fps idle; while printing 2 fps, or 1 fps on a constrained host. JPEG quality 70.
- Port `7130` (setting `/remote_screen/port`, not in the UI).
- The firmware's own server wins: `HELIX_REMOTE_SCREEN=firmware` or `HELIX_REMOTE_SCREEN_FB0` set means the settings rows are hidden and the server never starts.
- Webcam name `HelixScreen Screen`, a single entry. `iframe` → control page when control is on; `mjpegstreamer-adaptive`, `target_fps: 2` when view-only.
- nginx mount `/helixscreen/screen/`; route marker `config/.remote_screen_route`; installer marker `.nginx_remote_screen` in `HELIX_USER_CONFIG_FILES`.
- Pairing (Preston, 2026-10-19): a 6-digit numeric code on the device, entered once in the browser, which is then remembered by a long-lived cookie. No QR. View needs no pairing.
- Vendor rule (`.claude/rules/vendor-abstraction.md`): "constrained host" and "firmware has its own server" are capability questions, never printer names.
- Threading (`.claude/rules/threading.md`): HTTP handlers never touch LVGL; everything UI-side goes through `ui_queue_update()`. No detached threads.
- Firmware rules: no `std::regex`, no exceptions in anything the ESP32 image compiles. None of these files join `scripts/check_esp32_app_srcs.py`'s app list.
- Comments carry no archaeology; constants are `kCamelCase`; UI strings are US English and go through `lv_tr` / `label_tag` + `make translation-sync`.

## Corrections to the design (rulings, made while planning)

1. **Frames are logical, not physical.** The design says software rotation happens before `flush_cb`, so the hook sees physical pixels. It does not. `DisplayManager::install_color_transform_hook` wraps the backend's `flush_cb`, and `lv_linux_fbdev.c#flush_cb` rotates inside the wrapped callback (`lv_draw_sw_rotate` into `rotated_buf`). The hook therefore sees logical-orientation pixels at logical coordinates, `disp_w × disp_h` from `lv_display_get_{horizontal,vertical}_resolution`. Consequences:
   - The shadow is `disp_w × disp_h` and the served image is upright as the user sees it. No `phys_w/phys_h` fields are needed.
   - Touch has to go the other way. LVGL runs `lv_display_rotate_point` (physical → logical) on every pointer indev, so the synthetic pointer must be fed the **inverse** of the logical image point. `remote_image_to_indev_point` is that inverse.
   - The `ctl` pointer currently feeds logical coordinates straight in, so on a rotated display it lands in the wrong place. It is fixed in the same task.
   - Hardware check on a rotated K1 (480x800 panel shown as 800x480): it costs one rule if wrong.
2. **The token row is replaced by pairing.** The settings `token` row becomes:
   - `/remote_screen/pair_code`, 6 digits, generated when control is first enabled;
   - `/remote_screen/paired`, a list of up to 8 browser tokens (128-bit hex, least recently used evicted).

   The cookie is `helix_screen=<token>; Max-Age=31536000; HttpOnly; SameSite=Strict; Path=/`. "Forget paired browsers" clears the list and rotates the code. Brute-force limit: 5 failed code attempts per 60 s (429 after that, with `Retry-After`), and the code rotates after 50 cumulative failures.
3. **Rate-cap class is a capability.** The design names `ad5m`/`cc1`/`mips`. This plan uses `MemoryInfo::is_constrained_device() || std::thread::hardware_concurrency() <= 1`, which covers those boards without naming them.

The design doc is edited in Task 0 so the two documents agree.

## Review Focus

1. **The app picks its own feed as the printer camera.** Our `HelixScreen Screen` entry shows up in `server.webcams.list` and would become the camera widget's feed, a screen showing itself. Discovery must exclude it (Task 6 test `webcam discovery skips HelixScreen's own screen entry`).
2. **First request while the display sleeps or before any flush.** No frame exists yet. Expect a `503` with `Retry-After: 1`, never a black JPEG or a hang (Task 4 test `snapshot with no frame yet is 503`).
3. **A browser left open on a printer that later turns view off.** The server stops, so requests fail fast. The webcam entry is deleted. The shadow is freed even though no idle tick will come (Task 5 test `disabling view frees the store and stops the server`).
4. **Display resolution changes while a viewer is active** (rotation or resize handler, `DisplayManager::remove_resize_handler`). The shadow must re-size on a frame whose `disp_w/h` differ, never blit out of bounds (Task 3 test `resolution change reallocates the shadow`).
5. **A slow or stuck browser holding `touch` mid-press.** A `down` with no `up` must not leave the synthetic pointer pressed forever. The server releases the pointer after `kTouchStaleMs = 3000` with no event (Task 10 test `a down with no follow-up is released after kTouchStaleMs`).

---

### Task 0: Branch, design-doc corrections

**Files:**
- Modify: `docs/devel/plans/2026-10-19-remote-screen-server-design.md` (§ Capture "Shadow frame", § Control "Coordinates", § Security rule 3, § Settings table, § Tests "Physical-size shadow" row)

- [ ] **Step 1:** `scripts/setup-worktree.sh feature/remote-screen-server`. Take `worktree:remote-screen-server` with `--session`. Announce the branch to peers (`helix-claim list`).
- [ ] **Step 2:** Rewrite the five passages to match rulings 1-3 above. Present tense, no history.
- [ ] **Step 3:** Commit: `git commit -m "docs(plans): screen server plan; frames are logical, pairing code" -- docs/devel/plans/` (add the plan with `git add -N` first).

### Task 1: Extract `blit_remote_frame`

**Files:**
- Create: `include/remote_screen_blit.h`, `src/application/remote_screen_blit.cpp`
- Modify: `src/application/remote_screen_fb0_sink.cpp#Fb0MailboxSink::on_frame` (its body becomes a call plus the existing one-shot logging)
- Test: `tests/unit/test_remote_screen_blit.cpp` (new), `tests/unit/test_remote_screen_fb0_sink.cpp` (unchanged, must stay green)

**Interfaces:**
- Produces:
  ```cpp
  namespace helix {
  struct BlitTarget { uint8_t* data; size_t len; uint32_t stride; int w; int h; int bpp; }; // bpp 16|32
  enum class BlitStatus { Copied, NoSource, UnknownFormat, Empty, BeforeOrigin, SourceOutOfBounds, TargetOutOfBounds };
  struct BlitResult { BlitStatus status; int32_t x, y, w, h; }; // clamped rect actually written
  BlitResult blit_remote_frame(const RemoteScreenFrame& f, const BlitTarget& dst);
  int remote_frame_src_bpp(RemoteScreenPixelFormat fmt); // 4, 2, or 0
  inline void rgb565_to_rgb(uint16_t p, uint8_t& r, uint8_t& g, uint8_t& b); // the existing 5/6-bit expansion
  }
  ```

The bounds rules move verbatim: clamp to the target, origin subtraction, `px_map_len` authority, the `src_stride * disp_h` fallback. Add one new rule: no write may pass `dst.data + dst.len` (`TargetOutOfBounds`), since a heap shadow has no mmap slack. Logging stays in the sink: it warns once on `BeforeOrigin` / `SourceOutOfBounds`, as now.

- [ ] **Step 1: Write the failing tests** in `test_remote_screen_blit.cpp`, tag `[remote_screen][blit]`:
  - `blit: every src x dst format pair converts one known pixel`. A 4-case table: BGRA→32 copies bytes; RGB565 `0xF800`→32 gives `B=0,G=0,R=0xFF,A=0xFF`; RGB565→16 copies verbatim; BGRA `(B=0x10,G=0x20,R=0xF0)`→16 gives `((0xF0>>3)<<11)|((0x20>>2)<<5)|(0x10>>3)`, little-endian.
  - `blit: partial-mode origin reads the rect's own row 0`: `px_map_x/y = (10,20)`, rect (10,20)-(13,21). The destination at (10,20) gets source byte 0.
  - `blit: px_map_len smaller than the rect is SourceOutOfBounds and writes nothing`.
  - `blit: target len smaller than stride*h is TargetOutOfBounds and writes nothing`: dst.len = stride*h - 1, rect covering the last row.
  - `blit: rect clamped at the right/bottom edge reports the clamped w/h`.
  - `blit: Unknown format and null px_map return their statuses`.
- [ ] **Step 2:** `make t F='[blit]'`. Expected: compile failure, `remote_screen_blit.h` not found.
- [ ] **Step 3:** Implement. Point `Fb0MailboxSink::on_frame` at it with `BlitTarget{map_, map_size_, fb_stride_, fb_w_, fb_h_, fb_bpp_}`.
- [ ] **Step 4:** `make t F='[remote_screen]'`. Expected: all pass, including every existing `[fb0]` case unchanged. Hand-break the `TargetOutOfBounds` check once, see its test go red, restore.
- [ ] **Step 5:** Commit: `refactor(remote-screen): one blit for every remote-screen sink`.

### Task 2: JPEG encoder

**Files:**
- Create: `lib/stb/stb_image_write.h` (upstream nothings/stb `stb_image_write.h` v1.16, unmodified; put the upstream commit SHA in the commit body)
- Create: `include/jpeg_encode.h`, `src/system/jpeg_encode.cpp` (the only TU defining `STB_IMAGE_WRITE_IMPLEMENTATION`; `STBIW_ASSERT` stays default)
- Test: `tests/unit/test_jpeg_encode.cpp`, tag `[jpeg]`

**Interfaces:**
- Consumes: `remote_frame_src_bpp`, `rgb565_to_rgb` (Task 1)
- Produces:
  ```cpp
  namespace helix {
  constexpr int kRemoteScreenJpegQuality = 70;
  /// src is w x h at src_bpp 2 (RGB565) or 4 (BGRA); dst holds w*h*3.
  void convert_to_rgb888(const uint8_t* src, uint32_t src_stride, int w, int h, int src_bpp, uint8_t* dst);
  /// false on w/h <= 0, null rgb, or encoder failure. quality clamped to [1,100]. out is replaced.
  bool encode_jpeg_rgb888(const uint8_t* rgb, int w, int h, int quality, std::vector<uint8_t>& out);
  }
  ```

- [ ] **Step 1: Write the failing tests:**
  - `jpeg: four-quadrant pattern decodes with dims and per-quadrant means within 8`. 64x48 with red/green/blue/white quadrants, encoded at 70 and decoded with `stbi_load_from_memory`. Sample the center 8x8 of each quadrant; each channel mean is within ±8 of the source.
  - `jpeg: output starts with FFD8 and ends with FFD9`.
  - `jpeg: zero size and null input return false and leave out empty`.
  - `jpeg: RGB565 and BGRA sources convert to the same RGB888`.
- [ ] **Step 2:** `make t F='[jpeg]'`. Expected: compile failure.
- [ ] **Step 3:** Implement with `stbi_write_jpg_to_func` appending into `out`.
- [ ] **Step 4:** `make t F='[jpeg]'`. Expected: PASS. Hand-break: swap R/B in `convert_to_rgb888` and the quadrant test goes red.
- [ ] **Step 5:** Commit: `feat(remote-screen): vendored stb JPEG encoder`.

### Task 3: `ScreenFrameStore` and `ScreenServerSink`

**Files:**
- Create: `include/remote_screen_server_sink.h`, `src/application/remote_screen_server_sink.cpp`
- Test: `tests/unit/test_remote_screen_server_sink.cpp`, tag `[remote_screen][server_sink]`

**Interfaces:**
- Consumes: `blit_remote_frame`, `BlitTarget` (Task 1); `convert_to_rgb888`, `encode_jpeg_rgb888`, `kRemoteScreenJpegQuality` (Task 2)
- Produces:
  ```cpp
  namespace helix {
  constexpr int64_t kViewerIdleMs = 10000;
  constexpr int64_t kFirstFrameWaitMs = 500;
  struct SnapshotRateCap { int idle_fps; int printing_fps; };
  SnapshotRateCap snapshot_rate_cap(bool constrained_host);      // {5,2} or {5,1}
  bool remote_screen_host_is_constrained();                      // MemoryInfo constrained || hardware_concurrency() <= 1
  std::chrono::milliseconds min_encode_interval(SnapshotRateCap cap, bool printing); // 1000/fps

  struct EncodedSnapshot { std::shared_ptr<const std::vector<uint8_t>> jpeg; uint64_t generation; int w; int h; };

  class ScreenFrameStore {   // thread-safe; one mutex guards shadow + generation + cache
    public:
      using Clock = std::function<std::chrono::steady_clock::time_point()>;
      explicit ScreenFrameStore(Clock clock = {});               // empty = steady_clock::now
      // Flush (main) thread
      bool wants_frames() const;                                 // now - last_request < kViewerIdleMs
      void on_frame(const RemoteScreenFrame& f);                 // (re)allocates shadow at f.disp_w x f.disp_h, src bpp; blits; ++generation; notifies waiters
      // Any thread
      enum class Need { Ready, FullRedraw };
      Need note_request();                                       // stamps last_request; FullRedraw when no frame since last release
      bool wait_for_generation_after(uint64_t gen, std::chrono::milliseconds timeout);
      uint64_t generation() const;
      std::optional<EncodedSnapshot> snapshot(bool printing, SnapshotRateCap cap); // nullopt if no frame ever; cache hit when gen unchanged or inside min_encode_interval
      bool release_if_idle();                                    // frees shadow, rgb and cache once idle; true if it freed
      void release();                                            // unconditional (view turned off)
      size_t allocated_bytes() const;                            // for tests and logs
  };

  class ScreenServerSink : public RemoteScreenSink {
    public:
      explicit ScreenServerSink(std::shared_ptr<ScreenFrameStore> store);
      bool start() override; void stop() override;
      bool wants_frames() const override;                        // store->wants_frames()
      void on_frame(const RemoteScreenFrame& f) override;        // store->on_frame(f)
      const char* name() const override;                         // "ScreenServerSink"
  };
  }
  ```
  RGB888 conversion runs under the lock and the encode runs outside it, per the spec. `snapshot()` does not hold the mutex while encoding.

- [ ] **Step 1: Write the failing tests** (injected clock throughout):
  - `store: wants_frames false before any request, true after, false after kViewerIdleMs`.
  - `store: release_if_idle frees buffers only after idle` (`allocated_bytes()` > 0 → 0).
  - `store: first request after a release asks for FullRedraw; the next asks Ready`.
  - `store: unchanged generation returns the cached jpeg pointer` (same `shared_ptr` address).
  - `store: a new generation inside min_encode_interval still returns the cache`; at +interval it re-encodes.
  - `rate cap: printing selects printing_fps; constrained host gives 1`: `min_encode_interval({5,1}, true) == 1000ms`, `({5,2}, true) == 500ms`, `({5,2}, false) == 200ms`.
  - `store: resolution change reallocates the shadow` (Review Focus 4): a 100x50 frame then a 50x100 frame, the snapshot is 50x100 with no ASAN finding.
  - `store: wait_for_generation_after returns true when on_frame lands from another thread, false at timeout`.
  - `store: snapshot before any frame is nullopt`.
- [ ] **Step 2:** `make t F='[server_sink]'`. Expected: compile failure.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `make t F='[server_sink]'`. Expected: PASS. Hand-break the cache-generation compare and see the cache test go red.
- [ ] **Step 5:** Commit: `feat(remote-screen): frame store and server sink`.

### Task 4: `ScreenHttpServer` (view routes)

**Files:**
- Create: `include/http_listen_socket.h`, `src/system/http_listen_socket.cpp`. `bind_loopback_ephemeral` moves out of `src/api/mock_http_file_server.cpp` and is widened to:
  `int bind_tcp_listener(const char* host, int port, int* out_port); // port 0 = ephemeral; returns fd or -1`. The mock server calls it with `("127.0.0.1", 0, &port)`.
- Create: `include/remote_screen_http_server.h`, `src/application/remote_screen_http_server.cpp`
- Test: `tests/unit/test_remote_screen_http_server.cpp`, tag `[remote_screen][http]`

**Interfaces:**
- Consumes: `ScreenFrameStore`, `EncodedSnapshot`, `snapshot_rate_cap`, `remote_screen_host_is_constrained`, `kFirstFrameWaitMs` (Task 3)
- Produces:
  ```cpp
  namespace helix {
  struct ScreenServerConfig {
      std::string bind_host;     // "127.0.0.1" (nginx route) or "0.0.0.0" (LAN)
      int port = 7130;           // 0 in tests
      bool control_allowed = false;
      bool lan_mode = false;     // pairing required for control
      std::string page_path = "assets/remote_screen/index.html";
  };
  struct ScreenServerHooks {     // called on the HTTP thread; implementers queue to the UI thread
      std::function<void()> request_full_redraw;
      std::function<bool()> is_printing;
      std::function<void(const struct TouchEvent&)> apply_touch;    // Task 10
      std::function<class PairingStore*()> pairing;                  // Task 10; null in nginx mode
  };
  class ScreenHttpServer {
    public:
      ScreenHttpServer(std::shared_ptr<ScreenFrameStore> store, ScreenServerHooks hooks);
      ~ScreenHttpServer();       // stop()
      bool start(const ScreenServerConfig& cfg);
      void stop();               // idempotent; wakes any handler blocked in wait_for_generation_after
      int port() const;
      void set_control_allowed(bool allowed);   // live, atomic
  };
  }
  ```
  Routes: `GET /snapshot.jpg` (query string ignored; `Content-Type: image/jpeg`; `Cache-Control: no-store`), `GET /info` → `{"w":..,"h":..,"control":bool,"paired":bool}`, `GET /` → the page file. The server mounts at `/` on its own port; nginx strips `/helixscreen/screen/`. One worker thread (`setThreadNum(1)`).

- [ ] **Step 1: Write the failing tests.** Server on `port = 0`; requests via libhv's `requests::` client:
  - `http: snapshot returns image/jpeg with no-store after a frame`.
  - `http: snapshot with no frame yet is 503 with Retry-After 1` (Review Focus 2). Assert the response arrives within `kFirstFrameWaitMs + 300ms`.
  - `http: first snapshot calls request_full_redraw once`.
  - `http: snapshot?ts=123 is served like snapshot.jpg`.
  - `http: info reports w, h and control`.
  - `http: stop() while a request waits returns promptly`. A handler is blocked waiting; `stop()` completes in under 1 s.
  - `http: mock file server still serves on an ephemeral port` (existing tests for `MockHttpFileServer` must still pass: run their tag).
- [ ] **Step 2:** `make t F='[http]'`. Expected: compile failure.
- [ ] **Step 3:** Implement. A missing page file returns 404 with a one-line text body.
- [ ] **Step 4:** `make t F='[remote_screen]'` plus the mock file server's tag. Expected: PASS. Hand-break the `Retry-After` header and see its test go red.
- [ ] **Step 5:** Commit: `feat(remote-screen): HTTP snapshot server`.

### Task 5: Settings, lifecycle, Settings rows (view)

**Files:**
- Create: `include/remote_screen_settings_manager.h`, `src/system/remote_screen_settings_manager.cpp` (shape: `SafetySettingsManager` with its `persisted_table()`)
- Create: `include/remote_screen_service.h`, `src/application/remote_screen_service.cpp`
- Modify:
  - `src/application/display_manager.cpp`: `init()` adds the `ScreenServerSink` when the service is available; add an accessor `RemoteScreenManager& remote_screen()`.
  - `src/application/subject_initializer.cpp`: register the manager's subjects.
  - Application startup/shutdown: `RemoteScreenService::instance().start_if_enabled()` after display init, `shutdown()` before `DisplayManager::shutdown()`.
- Modify: `ui_xml/settings_connection_overlay.xml` (new `setting_group name="group_remote_screen"`), `src/ui/ui_settings_connection.cpp`
- Create: `assets/remote_screen/index.html`. View-only page for now: an `<img>` refreshed from `snapshot.jpg` at the `info`-advertised cadence, pausing while the tab is hidden.
- Test: `tests/unit/test_remote_screen_settings.cpp`, `tests/unit/test_remote_screen_service.cpp`, tag `[remote_screen][settings]`

**Interfaces:**
- Consumes: Tasks 3-4.
- Produces:
  ```cpp
  namespace helix {
  bool remote_screen_firmware_provides();   // getenv HELIX_REMOTE_SCREEN == "firmware" || HELIX_REMOTE_SCREEN_FB0 set
  /// Mount path read from config/.remote_screen_route ("/helixscreen/screen/"), empty when the file is absent or blank.
  std::string remote_screen_route(const std::string& config_dir);

  class RemoteScreenSettingsManager {       // Scope::Global keys from Global Constraints
      static RemoteScreenSettingsManager& instance();
      static settings::TableView persisted_table();
      void init_subjects(); void deinit_subjects();
      bool get_enabled() const;  void set_enabled(bool);
      bool get_control() const;  void set_control(bool);         // forced false while !enabled
      int  get_port() const;
      // Task 10 adds pair_code / paired accessors
  };
  // Subjects: remote_screen_available (int 0/1), remote_screen_enabled, remote_screen_control, remote_screen_lan_mode

  class RemoteScreenService {               // main thread
      static RemoteScreenService& instance();
      void start_if_enabled();              // no-op when firmware provides or disabled
      void apply_settings();                // live start/stop on toggle
      void shutdown();
      bool running() const;
      std::shared_ptr<ScreenFrameStore> store() const;
  };
  }
  ```
  Hook wiring:
  - `request_full_redraw` → `ui_queue_update`, which invalidates `lv_screen_active()` and `lv_layer_top()`.
  - `is_printing` → an `std::atomic<bool>` kept current by an `observe<int>` on `print_active`.
  - Idle release → a 2 s `lv_timer` that exists only while the store holds buffers, cancelled in both `shutdown()` and the destructor with `lv_timer_cancel_safe`.

  UI rows in `group_remote_screen`, the whole group hidden on `remote_screen_available == 0`:
  - `setting_toggle_row` "Share screen", description "Show this screen as a webcam in Mainsail and Fluidd".
  - `setting_toggle_row` "Allow remote control", description "Let a browser tap and drag on this screen". It is disabled while view is off and wired in Task 10.

- [ ] **Step 1: Write the failing tests:**
  - `settings: defaults are off, port 7130`.
  - `settings: control cannot be on while view is off`: `set_control(true)` with view off leaves it false; `set_enabled(false)` clears control.
  - `route: missing, blank and present route files`.
  - `firmware_provides: both env spellings`.
  - `service: disabling view frees the store and stops the server` (Review Focus 3): start, request a snapshot, set enabled false. `running()` is false and `store()->allocated_bytes() == 0`.
  - `service: firmware-provided screen never starts`.
- [ ] **Step 2:** `make t F='[settings][remote_screen]'`. Expected: compile failure.
- [ ] **Step 3:** Implement. Strings through `lv_tr`/`label_tag`, then `make translation-sync && make translations` (L064).
- [ ] **Step 4:** `make t F='[remote_screen]'`. Expected: PASS. Then headless:
  - Run `HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy` with the pinned-socket recipe and turn on "Share screen" via `ctl`.
  - `curl -s -o /tmp/rs.jpg -w '%{http_code} %{content_type}' http://127.0.0.1:7130/snapshot.jpg` prints `200 image/jpeg`.
  - `file /tmp/rs.jpg` shows JPEG at the mock's resolution. Read the image to confirm it shows the home panel.
- [ ] **Step 5:** Commit: `feat(remote-screen): Share screen setting and server lifecycle`.

### Task 6: Webcam registration and self-exclusion

**Files:**
- Modify: `include/i_moonraker_sub_apis.h#ITimelapseAPI`, `include/moonraker_timelapse_api.h`, `src/api/moonraker_timelapse_api.cpp`, and the mock's timelapse API (the mock records posted entries so `get_webcam_list` returns them)
- Modify: `src/api/moonraker_discovery_sequence.cpp` (exclude our entry), `include/webcam_selection.h` (`is_own_screen_webcam`)
- Create: `include/remote_screen_webcam.h`, `src/application/remote_screen_webcam.cpp`
- Modify: `src/application/discovery_steps_core.cpp#kCoreSteps`: add `{"remote_screen_webcam", false, remote_screen_webcam_step, nullptr}` after `helix_plugin_check`.
- Modify: `include/host_identity.h` / `src/system/host_identity.cpp`: make the interface walk public as `std::vector<std::string> local_interface_ips()`. `src/plugin/plugin_backend_app.cpp` and the host-identity scan both call it.
- Test: `tests/unit/test_remote_screen_webcam.cpp`, tag `[remote_screen][webcam]`; the interface drift tests (`[compile][drift]`).

**Interfaces:**
- Consumes: `RemoteScreenSettingsManager` getters, `remote_screen_route`, `remote_screen_firmware_provides` (Task 5)
- Produces:
  ```cpp
  // ITimelapseAPI additions
  virtual void post_webcam(const nlohmann::json& item, SuccessCallback, ErrorCallback) = 0;   // server.webcams.post_item
  virtual void delete_webcam(const std::string& name, SuccessCallback, ErrorCallback) = 0;    // server.webcams.delete_item {name}

  namespace helix {
  constexpr const char* kRemoteScreenWebcamName = "HelixScreen Screen";
  bool is_own_screen_webcam(const WebcamInfo& cam);              // name match
  /// nullopt = the entry should not exist (view off or firmware provides)
  std::optional<nlohmann::json> remote_screen_webcam_item(bool enabled, bool control, const std::string& route,
                                                          const std::string& lan_ip, int port);
  /// Pure: the first non-loopback, non-link-local IPv4 in ips; "" if none.
  std::string select_lan_ipv4(const std::vector<std::string>& ips);
  void sync_remote_screen_webcam(IMoonrakerAPI& api);            // post or delete from current settings
  }
  ```
  Item shapes:
  - With a route: `stream_url = route` (control) or `snapshot_url = route + "snapshot.jpg"` (view), both relative.
  - Without one: `http://<lan_ip>:<port>/...`.
  - Common fields: `"location": "printer"`, `"enabled": true`.

  `sync_remote_screen_webcam` runs from the discovery step (every connect) and from `RemoteScreenService::apply_settings()`.

- [ ] **Step 1: Write the failing tests:**
  - `webcam item: view-only with route is mjpegstreamer-adaptive, relative snapshot, target_fps 2`.
  - `webcam item: control with route is iframe pointing at the route`.
  - `webcam item: LAN mode uses http://ip:port`.
  - `webcam item: disabled is nullopt`.
  - `select_lan_ipv4 skips 127.x, 169.254.x and IPv6`.
  - `webcam discovery skips HelixScreen's own screen entry` (Review Focus 1). The mock list holds our entry plus one real camera; the selected camera is the real one, and with only ours, no camera is selected.
  - `mock: post then list round-trips; delete removes it`.
- [ ] **Step 2:** `make t F='[webcam]'`. Expected: compile failure.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `make t F='[webcam]'` and `make t F='[drift]'`. Expected: PASS. Hand-break `is_own_screen_webcam` (always false) and see the discovery test go red.
- [ ] **Step 5:** Commit: `feat(remote-screen): register the screen as a Moonraker webcam`.

### Task 7: Routing benchmark (hardware, spike, not landed)

Decides whether the plugin route exists at all. It needs Preston's OK for each device and a `device:` claim. Prints are started by Preston, or confirmed with him each time.

**Files:** a throwaway branch `spike/remote-screen-plugin-proxy`: a `GET /server/helix/screen/snapshot.jpg` endpoint in `moonraker-plugin/helix_print.py` that proxies `127.0.0.1:7130/snapshot.jpg` with `aiohttp`/`tornado` (whichever the plugin already imports). Results go in `docs/devel/plans/2026-10-19-remote-screen-server-design.md` § Decisions, Routing.

- [ ] **Step 1:** Cross-build the branch at Task 6 for the AD5M or CC1, a K1 and a Pi 4 or K2. Deploy under claims.
- [ ] **Step 2:** For each host, take three 10-minute runs during the same moving print (one print file, same layer band):
  - (a) no viewer;
  - (b) nginx-direct at the printing cap;
  - (c) the plugin proxy at the same cap.

  Record:
  - Moonraker process CPU (`pidstat -p <pid> 5`);
  - helix-screen CPU;
  - `MCU stats` timer margin from `klippy.log`, plus any "Timer too close";
  - p50/p95 snapshot latency and achieved fps from a `curl` loop on a LAN client.
- [ ] **Step 3:** Rule per the spec. nginx wins unless (c) is within noise of (b) on the weak hosts. The plugin path is kept only for hosts with no nginx, and only if it measured well there. Ledger the ruling and write the numbers into the design doc.
- [ ] **Step 4:** If the plugin path is kept, add a Task 8b (endpoint + tests in `moonraker-plugin/tests/test_helix_print.py`) before Task 8. Otherwise delete the spike branch.

### Task 8: Installer nginx module

**Files:**
- Create: `scripts/lib/installer/remote_screen.sh` (sourced the way `camera.sh` is)
- Modify:
  - `scripts/lib/installer/camera.sh`: generalize `_webcam_block_has_directive` to take the location path and move `_nginx_conf_path`, `_nginx_validate_and_reload` and the backup/revert sequence into shared functions both modules call. Nothing is copied.
  - `scripts/lib/installer/platform.sh`: add `.nginx_remote_screen` and `.remote_screen_route` to `HELIX_USER_CONFIG_FILES`.
  - `scripts/lib/installer/uninstall.sh`; the installer's main flow.
  - The bundled installer/uninstaller generators, whichever `make` target concatenates modules.
- Test: `tests/shell/test_remote_screen_installer.bats`

**Interfaces:**
- Produces:
  - `install_remote_screen_route` writes `${CONFIG_DIR}/.remote_screen_route` containing `/helixscreen/screen/` only after `nginx -t` passes.
  - `remove_remote_screen_route` strips the tagged block, reloads nginx, deletes the route marker and DELETEs the `HelixScreen Screen` webcam via `/server/webcams/item?name=...` (best effort).
  - Tag: `NGINX_REMOTE_SCREEN_TAG="helix-managed-remote-screen"`. The block goes in every `server {}` that has a `location /websocket` or `proxy_pass` to Moonraker. Every line carries the tag:
  ```nginx
  location /helixscreen/screen/ {            # helix-managed-remote-screen
      auth_request /helixscreen/screen-auth; # helix-managed-remote-screen
      proxy_pass http://127.0.0.1:7130/;     # helix-managed-remote-screen
      proxy_buffering off;                   # helix-managed-remote-screen
      access_log off;                        # helix-managed-remote-screen
  }                                          # helix-managed-remote-screen
  location = /helixscreen/screen-auth {      # helix-managed-remote-screen
      internal;                              # helix-managed-remote-screen
      proxy_pass http://127.0.0.1:7125/access/user; # helix-managed-remote-screen
      proxy_pass_request_body off;           # helix-managed-remote-screen
      proxy_set_header Content-Length "";    # helix-managed-remote-screen
      proxy_set_header X-Real-IP $remote_addr; # helix-managed-remote-screen
  }                                          # helix-managed-remote-screen
  ```
  The Moonraker upstream is the one the server block already proxies to. Read it from the block; fall back to `127.0.0.1:7125`.
  - Skip with no edit when `nginx -V 2>&1` lacks `http_auth_request_module`, when no Moonraker server block is found, or when the marker already exists (idempotent).
  - Search order: `HELIX_NGINX_CONF`, then `/etc/nginx/nginx.conf`, then `/etc/nginx/sites-enabled/*`. The K1 Helper Script path is added after the hardware check in "To verify".

- [ ] **Step 1: Write the failing bats tests.** `HELIX_NGINX_CONF` points at fixtures; `nginx` is stubbed on `PATH`:
  - `inserts the block once per Moonraker server block` (a fixture with two: Fluidd 4408 + Mainsail 4409);
  - `second run changes nothing`;
  - `failing nginx -t restores the original byte for byte and writes no route marker`;
  - `nginx without auth_request is left untouched`;
  - `config with no Moonraker proxy is left untouched`;
  - `uninstall strips only tagged lines`, with a user line containing `screen` that survives.
- [ ] **Step 2:** `bats tests/shell/test_remote_screen_installer.bats`. Expected: FAIL, functions undefined.
- [ ] **Step 3:** Implement. `tests/shell/test_camera_k2.bats` must still pass after the shared-helper move.
- [ ] **Step 4:** Run both files. A `tests/shell/` change runs the whole bats suite once at the finish (memory: new bats test → full shell suite), not here. Hand-break the idempotency check and see the second-run test go red.
- [ ] **Step 5:** Commit: `feat(installer): nginx route for the built-in screen server`.

### Task 9: `RemotePointer` for release builds and rotation-correct mapping

**Files:**
- Move: `src/remote/remote_pointer.cpp` → `src/application/remote_pointer.cpp`, compiled in every build. The header stays at `include/remote_pointer.h`.
- Modify:
  - `include/remote_pointer.h`: add the mapping and wait helpers below.
  - `src/remote/remote_control_server.cpp#RemoteControlServer::apply_pointer_state`: use both helpers. The ctl pointer becomes rotation-correct.
  - The Makefile source lists, if `src/remote/` is globbed separately.
- Test: `tests/unit/test_remote_pointer_mapping.cpp`, tag `[remote_pointer]`; the existing `test_remote_pointer_long_press.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace helix::remote {
  /// Inverse of lv_display_rotate_point: the raw indev point that LVGL will rotate onto
  /// logical (x, y). orig_w/orig_h are lv_display_get_original_{horizontal,vertical}_resolution.
  lv_point_t remote_image_to_indev_point(lv_display_rotation_t rot, int32_t orig_w, int32_t orig_h, int32_t x, int32_t y);
  // RemotePointer additions
  bool set_logical_state(int32_t x, int32_t y, bool pressed);  // UI thread; maps through the default display's rotation
  /// Any thread: blocks until read_count() >= baseline + 2, keep_going() goes false, or timeout.
  enum class WaitResult { Sampled, Stopped, TimedOut };
  WaitResult wait_for_samples(uint64_t baseline, std::chrono::milliseconds timeout, const std::function<bool()>& keep_going) const;
  }
  ```

- [ ] **Step 1: Write the failing tests:**
  - `mapping round-trips through lv_display_rotate_point at 0/90/180/270`. For each rotation, an `LVGLTestFixture` display 800x480 (so the logical size is 480x800 at 90/270), and the four corners plus the center. `lv_display_rotate_point(disp, remote_image_to_indev_point(...)) == input`.
  - `set_logical_state on a 90-degree display clicks the button under the logical point`. A button at logical (40,40) gets `LV_EVENT_CLICKED` after down/up with `wait_for_samples` between.
  - `wait_for_samples returns Stopped when keep_going flips`.
- [ ] **Step 2:** `make t F='[remote_pointer]'`. Expected: compile failure.
- [ ] **Step 3:** Implement. `apply_pointer_state` keeps its exceptions, which are host-only ctl code, mapping `WaitResult` onto the existing messages.
- [ ] **Step 4:** `make t F='[remote_pointer]'` and `make t F='[remote]'`. Expected: PASS. Build once with `ENABLE_REMOTE_CONTROL=no` (`make HELIX_PACKAGING=1` app only) and confirm it links. Hand-break by swapping the 90/270 cases and see the round-trip test go red.
- [ ] **Step 5:** Commit: `fix(remote): synthetic pointer maps logical points through display rotation`.

### Task 10: Control: touch endpoint, CSRF, pairing, page, setting

**Files:**
- Create: `include/remote_screen_pairing.h`, `src/application/remote_screen_pairing.cpp`
- Create: `include/random_bytes.h`, `src/system/random_bytes.cpp`. Move `fill_random_bytes` out of `src/system/telemetry_manager.cpp`'s anonymous namespace; the telemetry manager calls the shared one.
- Modify:
  - `src/application/remote_screen_http_server.cpp`: the `POST /touch`, `POST /pair` and `GET /info` `paired` field.
  - `src/system/remote_screen_settings_manager.cpp`: the `pair_code` and `paired` keys plus the `remote_screen_pair_code` string subject.
  - `src/application/remote_screen_service.cpp`: the `apply_touch` and `pairing` hooks.
  - `assets/remote_screen/index.html`: the pointer events, the pairing prompt and the view-only state.
  - `ui_xml/settings_connection_overlay.xml` and `src/ui/ui_settings_connection.cpp`: wire the control toggle. In LAN mode with control on, add `setting_info_row` "Pairing code" bound to `remote_screen_pair_code` and `setting_action_row` "Forget paired browsers".
- Test: `tests/unit/test_remote_screen_pairing.cpp`, `tests/unit/test_remote_screen_touch.cpp`, tag `[remote_screen][control]`

**Interfaces:**
- Consumes: `RemotePointer::set_logical_state`, `wait_for_samples` (Task 9); `ScreenHttpServer`, `ScreenServerHooks` (Task 4)
- Produces:
  ```cpp
  namespace helix {
  struct TouchEvent { enum class Type { Down, Move, Up } type; int32_t x; int32_t y; };
  /// Parses {"type":"down|move|up","x":int,"y":int}; nullopt on anything else. Exception-free (json::parse(s, nullptr, false)).
  std::optional<TouchEvent> parse_touch_body(const std::string& body, int img_w, int img_h); // rejects out-of-image points
  enum class TouchGate { Allowed, ControlOff, BadContentType, MissingHeader, OriginMismatch, NotPaired };
  /// Pure; 403 for all but NotPaired (401).
  TouchGate check_touch_request(bool control_allowed, bool lan_mode, const std::string& content_type,
                                const std::string& x_helix_touch, const std::string& origin, const std::string& host,
                                bool cookie_paired);
  constexpr int64_t kTouchStaleMs = 3000;

  class PairingStore {                // thread-safe; persistence through injected callbacks
    public:
      using Clock = std::function<std::chrono::steady_clock::time_point()>;
      struct Persist { std::function<void(const std::string& code)> save_code;
                       std::function<void(const std::vector<std::string>& tokens)> save_tokens; };
      PairingStore(std::string code, std::vector<std::string> tokens, Persist persist, Clock clock = {});
      enum class PairResult { Paired, WrongCode, RateLimited };
      PairResult try_pair(const std::string& code, std::string& token_out);  // mints a 32-hex token, LRU cap 8
      bool is_paired(const std::string& token) const;                        // constant-time compare
      void forget_all();                                                     // clears tokens, rotates code
      std::string code() const;
      static std::string generate_code();                                    // 6 digits, leading zeros kept
  };
  }
  ```
  - `POST /pair` takes `{"code":"123456"}` → `200` with `Set-Cookie`, `403` on a wrong code, `429` + `Retry-After` when rate-limited.
  - `POST /touch` responds once `wait_for_samples` returns `Sampled` (`204`), or `503` on a timeout.
  - In nginx mode, `is_paired` is not consulted: nginx's `auth_request` already authenticated the request.
  - Stale-press guard: a main-thread `lv_timer` releases the pointer at its last position when `kTouchStaleMs` passes after a `Down`/`Move` with no `Up` (Review Focus 5). Destructor-cancelled per rule 5.

- [ ] **Step 1: Write the failing tests:**
  - `parse_touch_body accepts the three types and rejects unknown type, missing x, non-integer, out-of-image`.
  - `check_touch_request`, a table of 7 rows: one per `TouchGate` value plus Allowed in nginx mode with no cookie.
  - `pairing: right code mints a token that is_paired accepts; wrong code does not`.
  - `pairing: 6th wrong code inside 60s is RateLimited, and a right code at +61s pairs`.
  - `pairing: 50 cumulative failures rotate the code`.
  - `pairing: 9th token evicts the least recently used`.
  - `pairing: forget_all clears tokens and changes the code`.
  - `generate_code is 6 digits`, over 1000 draws.
  - `http: touch is 403 with control off, 403 without X-Helix-Touch, 403 on Origin mismatch, 401 unpaired in LAN mode`.
  - `http: pair sets an HttpOnly SameSite=Strict cookie`.
  - `touch through LVGL: POST down/up at a button center fires CLICKED` (`LVGLTestFixture` with the real hooks).
  - `touch through LVGL: down/move/up across a list scrolls it and clicks nothing`.
  - `a down with no follow-up is released after kTouchStaleMs`.
- [ ] **Step 2:** `make t F='[control]'`. Expected: compile failure.
- [ ] **Step 3:** Implement. The page:
  - maps `pointerdown/move/up` (with pointer capture) on the `<img>` to natural-image pixels;
  - sends `fetch(..., {method:'POST', headers:{'Content-Type':'application/json','X-Helix-Touch':'1'}, credentials:'same-origin'})`;
  - throttles `move` to one in flight;
  - shows a 6-digit input when `info.paired` is false in LAN mode, and a "View only" badge when `control` is false.
- [ ] **Step 4:** `make t F='[remote_screen]'`. Expected: PASS. Then headless:
  - with control on, drive `curl -X POST -H 'Content-Type: application/json' -H 'X-Helix-Touch: 1' -d '{"type":"down","x":..,"y":..}'` then `up` at the `ctl geom` center of the home panel's navbar Settings button;
  - `ctl current` reports the settings panel.
  - Hand-break `check_touch_request`'s Origin rule and see its row go red.
- [ ] **Step 5:** Commit: `feat(remote-screen): remote control with on-device pairing`.

### Task 11 (phase 3): `stream.mjpg`

**Files:** `src/application/remote_screen_http_server.cpp`; `src/application/remote_screen_webcam.cpp`. When view-only, the item becomes `mjpegstreamer` with `stream_url` = `stream.mjpg` and `snapshot_url` = `snapshot.jpg`. Test: `[remote_screen][http]`.

**Interfaces:** `GET /stream.mjpg` → `multipart/x-mixed-replace; boundary=helixframe`, one part per new `EncodedSnapshot.generation` at the same cap, through libhv's chunked writer (`HttpResponseWriter`). It ends when the client disconnects or `stop()` runs. Each open stream counts as a request for `wants_frames()`.

- [ ] **Step 1:** Failing tests:
  - `stream: first two parts are JPEGs with the boundary`;
  - `stream: stop() ends an open stream within 1s`;
  - `stream: an open stream keeps wants_frames true past kViewerIdleMs`.
- [ ] **Step 2:** `make t F='[http]'`. Expected: FAIL (404).
- [ ] **Step 3:** Implement. **Step 4:** `make t F='[remote_screen]'` passes; hand-break the keep-alive stamp and see it go red. **Step 5:** Commit: `feat(remote-screen): MJPEG stream`.

### Task 12 (phase 3): Remote-control indicator

**Files:** `ui_xml/` status bar component (the one hosting the existing status icons); `src/application/remote_screen_service.cpp` publishes subject `remote_screen_control_active` (1 within `kViewerIdleMs` of the last accepted `touch`). Icon `remote` (or the closest MDI glyph; add it to both icon lists + `make regen-fonts`). Test: `[remote_screen][control]`.

- [ ] **Step 1:** Failing test: `control_active goes 1 on an accepted touch and 0 after kViewerIdleMs` (injected clock).
- [ ] **Step 2-4:** Implement, `make t F='[control]'`, one screenshot of the status bar with the icon shown (`ctl`-driven, headless).
- [ ] **Step 5:** Commit: `feat(remote-screen): show when a browser has control`.

### Task 13: Docs, finish

- [ ] **Step 1:** User doc: a "Remote Screen" section in the user guide's settings chapter, covering what it does, nginx vs pairing, and how to turn it off. Devel doc: a short `docs/devel/REMOTE_SCREEN.md` (sink, store, server, routes, security model) routed from `docs/devel/CLAUDE.md`. Delete both `docs/devel/plans/2026-10-19-remote-screen-server-*.md` files in this change.
- [ ] **Step 2:** The batch finish, once, per § Verification Cadence:
  - one Opus review of the branch, with this plan's Review Focus;
  - `make mutate-diff MUTATE_ARGS="--tests '[remote_screen],[remote_pointer],[jpeg],[blit],[webcam]'"`;
  - one screenshot each of Settings → Connection in LAN mode with control on, and of the status bar indicator;
  - `make test-shell` for the bats change;
  - one `make land-gate`.
- [ ] **Step 3:** Land on main per the shared-tree rules, close #1825 with commit evidence, tear down the worktree.

## To verify on hardware before Task 8 ships (from the spec, plus ruling 1)

These need Preston's OK for each device:
- the K1 Helper Script nginx path;
- `auth_request` in the K1 and K2 nginx builds (`nginx -V`);
- whether Moonraker honors `X-Real-IP` from nginx for `trusted_clients`. If it does not, every request looks like 127.0.0.1 and passes, and Task 8 must refuse to install where `trusted_clients` includes 127.0.0.1 and `force_logins` is off;
- ZMOD's screen server on the AD5M;
- fb-http's `touch` body, from the U1's `index.html`;
- the served image on a rotated K1 is upright, and a tap lands under the cursor.
