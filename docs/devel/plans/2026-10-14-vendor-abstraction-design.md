# Vendor Abstraction: Board Identity, Filament-System Dispatch, Capability Queries

Status: design, approved scope is all three phases, built and landed phase by phase.
Rule: `.claude/rules/vendor-abstraction.md`. Reference shape: `include/z_offset_persistence.h`
with `src/printer/z_offset_persistence.cpp` (a provider table keyed on a detection
predicate, free-function capability questions over it).

## Goal

A vendor, board or mod name appears in one module per capability, and generic code asks
that module a capability question. Today the board question ("which printer am I on?") is
answered four different ways in four files, two of them wrongly for the Creator 5; the
filament subscription builder hardcodes six backends' object lists while three other
backends already answer for themselves; and a handful of UI and startup sites branch on
`AmsType` or a backend's `name()` string where a capability exists or should.

The test for done, per capability: adding a second board or backend with the same
capability touches exactly one file, and the lint ratchet in Phase 1 step 1a refuses a new
vendor branch in generic code.

## Order and independence

| Phase | Lands | Depends on | Behaviour changes |
|-------|-------|------------|-------------------|
| 1. Board identity | 1a gate, 1b module, 1c updater, 1d per-board facts, 1e storage rung | nothing | four bug fixes (below) |
| 2. Filament-system dispatch | one branch, two commits | nothing | none (golden-tested parity) |
| 3. Capability queries and leftovers | one branch | Phase 1 for the gate baseline only | none user-visible |

Phase 2 does not need Phase 1 and can run in parallel in its own worktree. Phase 3 lowers
the Phase 1 ratchet baseline, so it lands after 1a. Each step below is one commit that
builds and passes `make t` on its own tags; each phase is one branch with one
`make full-test-run` at the end.

---

## Phase 1: one board question

### What the code does today (verified 2026-10-14 on main 486aaa520)

The unified `mips` build (`mk/cross.mk`, `PLATFORM_TARGET` `mips`/`k1`/`ad5x`, defines
`HELIX_PLATFORM_MIPS`) serves the K1 series, the AD5X, the Creator 5 and the Creator 5 Pro.
Which board it is on is asked like this:

| Site | Question asked | Wrong on |
|------|----------------|----------|
| `src/system/update_checker.cpp#this_build_release_asset_keys` | `key == "mips" && ad5x_mod_layout_present()` picks `ad5x` vs `k1` legacy asset | Creator 5 / Pro: takes the `ad5x` asset (bug 1) |
| `src/ui/touch_calibration.cpp#platform_default_calibration` | `#if HELIX_PLATFORM_MIPS` then `ad5x_board_present()`; `#elif HELIX_PLATFORM_AD5M` | correct since b955e2aa9 |
| `src/system/debug_bundle_collector.cpp#collect_printer_info` | `flashforge_machine_id()`, else `ad5x_mod_layout_present() ? "ad5x" : "k1"` | correct, but a third copy of the rule |
| `src/system/diagnostics.cpp#collect` | `mod_flavor = ad5x_mod_layout_present("/", &flavor)` | reports a mod flavor, no board at all (bug 5) |
| `src/system/jz_pwm_sound_backend.cpp#available` | `ad5x_mod_layout_present()` | right answer by accident: the layout probe is a FlashForge-tree probe, not a piezo probe (bug 6) |
| `src/system/platform_info.cpp#local_camera_probe_safe` | machine id or layout present, `#ifdef HELIX_PLATFORM_AD5X` arm | the AD5X arm is dead (bug 4) |
| `src/application/display_metrics.cpp#known_panel` | `kKnownPanels` keyed on `platform::current_key()` | a release K1 is `"mips"`, so the `"k1"` row never matches (bug 2) |
| `src/application/display_manager.cpp#init` (backlight block) | `m_backlight->name() == "Allwinner"` | keyed on a backlight driver for a firmware behaviour (bug 3) |
| `src/system/platform_info.cpp#is_printer_embedded` | `#if` over seven macros incl. `HELIX_PLATFORM_AD5X` | dead arm (bug 4) |
| `src/system/platform_info.cpp#platform_defaults_to_external_updates` | `#ifdef HELIX_PLATFORM_SNAPMAKER_U1` | fine, but a board fact outside the board table |
| `src/ui/filament_tube_stroker.cpp#reduced_effects` | `#if HELIX_PLATFORM_K2` | fine, but a board fact outside the board table |
| `src/system/helix_cache_dir.cpp#cache_path_candidates` | `#if AD5M / CC1 / K2 / MIPS` | MIPS arm writes `/usr/data/...` on an AD5X whose hook and manifest say `/srv/helixscreen-state`; U1 has no arm |

`HELIX_PLATFORM_AD5X` is defined by nothing: `mk/cross.mk` maps the `ad5x` target to
`HELIX_PLATFORM_MIPS`. It also appears in `lv_conf.h` (camera gate) and
`tests/unit/test_android_platform.cpp` (a mirror of the `is_printer_embedded` `#if`); all
three arms are dead.

### Corrections to the audit

- **Bug 1 is real and reachable today, not only on a pre-unified release.** v1.0.5 (the
  current stable) ships `ad5x` and `k1` assets and no `mips` asset. A Creator 5 on the beta
  channel that switches to Stable is offered v1.0.5 as a downgrade and would install the
  AD5X build. Beta.8 ships `mips`, `ad5x` and `k1`, so beta and dev are unaffected.
- **Bug 2 has no visible effect today.** `ui_scale_for_dpi` is flat 1.0 through 225 DPI and
  the fallback is 160 DPI, so a K1 at 218 DPI and a K1 on the fallback both scale 1.0. The
  fix is correctness: the startup log line names `KnownPanel` instead of `Fallback`, and a
  future deadband change cannot silently mis-scale the K1.
- **Bug 3: the right home is the board table, not `BacklightBackend`.** The re-assert
  undoes the FlashForge firmware's `reset_screen` delayed_gcode (documented in
  `docs/devel/printers/FLASHFORGE_AD5M_SUPPORT.md`), which is firmware behaviour. The
  `Allwinner` backend is also picked on the CC1 and on any Sonic Pad running a Pi build,
  neither of which dims behind our back. Every FlashForge hook (`hooks-ad5m-zmod.sh`,
  `hooks-ad5m-forgex.sh`, `hooks-ad5x.sh`, `hooks-ad5x-forgex.sh`) creates the
  `/tmp/helixscreen_active` flag that exists only to fight that dim, so the AD5X family
  carries the same behaviour. A `BacklightBackend::needs_startup_reassert()` would put a
  firmware fact on a driver class.
- **Bug 6: the buzzer gate already admits the Creator 5.** The C5 carries `/usr/prog`, so
  `ad5x_mod_layout_present()` is true there today. What the board table changes is the
  reason: the gate becomes "this board has the jz_pwm piezo", which also keeps a K1 out
  explicitly instead of by the absence of a FlashForge tree. Note what still decides
  whether the piezo plays: `JzPwmSoundBackend::available()` also needs `/dev/jz_pwm` and an
  `fx-pwm` binary, and `fx-pwm` ships in the Forge-X rootfs only. A Z-Mod Creator 5 falls
  through to the M300 backend (Z-Mod defines `[gcode_macro M300]`) exactly as a Z-Mod AD5X
  does. See open question 2.
- **Bug 5 needs a hardware check before its value set changes.** The `/usr/prog` arm
  reports `ZMOD` on any FlashForge firmware tree, modded or not. Splitting it into `ZMOD`
  (marker) and `FlashForge` (tree only) is right only if the `/ZMOD` marker is visible from
  inside the Z-Mod chroot HelixScreen runs in; if it is not, the split would relabel every
  Z-Mod host. Phase 1 renames the probe and adds the board to diagnostics; the value split
  is a follow-up gated on `ls -la /ZMOD` from the app's namespace on the AD5X and the C5.
- **`hardware_fingerprint.cpp` needs no change** (Phase 2 note): it hashes discovery
  flags, it does not decide anything.
- **The camera probe already refuses the AD5M**, by accident: `/usr/prog` matches there.
  Before 9ec343427 no embedded build probed loopback cameras at all, so refusing the AD5M
  matches what it has always done. The board table keeps it false; flipping it is a
  hardware-verify item, not part of this work.

### New module: `include/board_identity.h` + `src/system/board_identity.cpp`

Board identity is one enum and one table. Capability modules that own large per-board data
(touch fits, panel geometry, cache paths) key their own tables on `board::Board`, the same
way `display_metrics.cpp` owns `kKnownPanels` today: one module per capability, one shared
identity question. Small per-board booleans live in the board table itself.

```cpp
namespace helix::board {

enum class Board {
    Generic, ///< Drives a printer somewhere else: Pi, x86, desktop, Android, K-Touch
    Ad5m,    ///< Adventurer 5M and 5M Pro, one board
    Ad5x,
    Creator5,
    Creator5Pro,
    K1, ///< K1, K1C, K1 Max
    K2, ///< K2, K2 Pro, K2 Plus
    Cc1,
    SnapmakerU1,
};

struct Info {
    Board board;
    const char* key;          ///< "creator5-pro": logs, bundles, crash reports
    const char* display_name; ///< printer-database spelling; "" for Generic
    const char* legacy_release_asset; ///< pre-unified asset for this board, or nullptr
    bool local_camera_probe_safe;
    bool has_jz_pwm_piezo;
    bool reduced_effects;
    bool firmware_dims_backlight_after_boot;
    bool firmware_manages_updates;
};

Board resolve(const std::string& platform_key, const std::string& probe_root = "/");
Board current();                 // resolve(platform::current_key()), computed once
const Info& info(Board board);   // every Board has a row
inline const Info& current_info() { return info(current()); }
void set_current_override(std::optional<Board> board); // tests
}
```

`resolve()` maps `"ad5m"`, `"ad5x"`, `"k1"`, `"k2"`, `"cc1"`, `"snapmaker-u1"` to their board
with no filesystem access and every other key except `"mips"` to `Generic`. For `"mips"`:
`flashforge_machine_id(probe_root)` names Ad5x / Creator5 / Creator5Pro; otherwise
`flashforge_mod_tree(probe_root) != None` is an Ad5x (the Forge-X chroot, which has no
machine id); otherwise K1. That is today's `ad5x_board_present()` rule plus the K1 default,
in one place.

The board table, with every value read from the code it replaces:

| Board | key | display_name | legacy_release_asset | camera probe | jz_pwm piezo | reduced effects | fw dims backlight | fw manages updates |
|-------|-----|--------------|----------------------|--------------|--------------|-----------------|-------------------|--------------------|
| Generic | `generic` | `""` | nullptr | true | false | false | false | false |
| Ad5m | `ad5m` | FlashForge Adventurer 5M | nullptr | false | false | false | true | false |
| Ad5x | `ad5x` | FlashForge Adventurer 5X | `ad5x` | false | true | false | true | false |
| Creator5 | `creator5` | FlashForge Creator 5 | nullptr | false | true | false | true | false |
| Creator5Pro | `creator5-pro` | FlashForge Creator 5 Pro | nullptr | false | true | false | true | false |
| K1 | `k1` | Creality K1 | `k1` | true | false | false | false | false |
| K2 | `k2` | Creality K2 | nullptr | true | false | true | false | false |
| Cc1 | `cc1` | Elegoo Centauri Carbon | nullptr | true | false | false | false | false |
| SnapmakerU1 | `snapmaker-u1` | Snapmaker U1 | nullptr | true | false | false | false | true |

Display names match `platform_table.cpp` rows and `flashforge_machine_display_name()`, which
`board::info()` replaces for the bundle. `flashforge_machine_display_name()` stays: the
installer-facing tests pin it against `ad5x_board_name()` in `scripts/lib/installer/platform.sh`.

### Changed interfaces in `include/platform_info.h`

- `ad5x_mod_layout_present(probe_root, flavor_out)` becomes
  `FlashForgeModTree flashforge_mod_tree(const std::string& probe_root = "/")` with
  `enum class FlashForgeModTree { None, Zmod, ForgeX }` and
  `const char* flashforge_mod_tree_name(FlashForgeModTree)` ("ZMOD", "Forge-X", "none").
  Same four stat probes, same truth table; the out-parameter becomes the return value.
  Callers after Phase 1: `board_identity.cpp`, `src/system/log_collector.cpp#default_file_paths` (mod
  log paths are a mod-tree fact, not a board fact), `src/system/diagnostics.cpp#collect`, and the cache
  rung in step 1e.
- `ad5x_board_present()` is deleted; `board::resolve()` carries its rule.
- `local_camera_probe_safe()` is deleted; discovery reads
  `board::current_info().local_camera_probe_safe`.
- `is_printer_embedded()` keeps its signature and override; its body becomes
  `board::current() != board::Board::Generic`. The `#if` with the dead AD5X arm goes.
- `platform_defaults_to_external_updates()` keeps its signature and override; its body
  becomes `board::current_info().firmware_manages_updates`.
- `flashforge_machine_id()`, `FlashForgeMachine` and `flashforge_machine_display_name()`
  stay as they are.

`include/platform_table.h`: `release_asset_keys(const std::string& key, bool ad5x_board)`
becomes `release_asset_keys(const std::string& key, board::Board board)`, returning the key,
then the board's `legacy_release_asset` when it is non-null and differs from the key. A
`k1-dynamic` build (key `"k1"`, board K1) therefore answers `{"k1"}`, not `{"k1","k1"}`.

### Steps

**1a. Ratchet gate (lands first, behaviour-neutral).** See "Lint gate" below. Baseline is
today's counts; every later step lowers it with `--write-baseline` in the same commit.

**1b. Board module and its direct consumers.**

- New `board_identity.h/.cpp`. `board_identity.cpp` is added to
  `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` (diagnostics, touch
  calibration and the tube stroker call it there); on ESP32 `current_key()` is `"esp32"`,
  so `resolve()` returns Generic without probing. `platform_info.cpp` stays excluded from
  the ESP32 cut, so `helixapp_platform_stubs2.cpp` replaces its `ad5x_mod_layout_present`
  stub with `flashforge_mod_tree` (returns None) and adds `flashforge_machine_id` (returns
  nullopt). `board_identity.cpp` uses no exceptions and no RTTI.
- `platform_info.cpp`: rename, delete `ad5x_board_present` and `local_camera_probe_safe`,
  fold `is_printer_embedded` and `platform_defaults_to_external_updates`.
- `lv_conf.h` and `tests/unit/test_android_platform.cpp`: drop the dead
  `HELIX_PLATFORM_AD5X` arm.
- `moonraker_discovery_sequence.cpp`: `probe_local_cameras` from the board row.
- `src/system/diagnostics.cpp#collect`: `mod_flavor` from `flashforge_mod_tree_name()`, plus a new
  `identity.board` field from `board::current_info().key`, carried into the crash report
  (`crash_reporter.cpp`: JSON `board`, the text report's Platform line) and the bundle.
- `src/system/debug_bundle_collector.cpp#collect_printer_info`: the whole `if (platform == "mips")`
  block becomes `board::current_info().display_name`, kept empty for Generic.
- `src/system/jz_pwm_sound_backend.cpp#available`: first check becomes
  `board::current_info().has_jz_pwm_piezo`. Comments in `sound_manager.cpp` and the
  Makefile's `JZ_PWM_CXXFLAGS` block say "the FlashForge MIPS boards" instead of "the AD5X".
- `display_manager.cpp`: the 20 s re-assert is gated on
  `board::current_info().firmware_dims_backlight_after_boot`.
- `src/ui/filament_tube_stroker.cpp#reduced_effects`: `#if HELIX_PLATFORM_K2` becomes
  `board::current_info().reduced_effects || get_system_memory_info().is_constrained_device()`,
  still cached in a function-local static.
- `scripts/helix-launcher.sh`: the heap-diag gate's comment names
  `helix::flashforge_mod_tree()`; the four probes and `HELIX_AD5X_PROBE_ROOT` are unchanged,
  so `tests/shell/test_helix_launcher_env.bats` needs no edit. The heap-diag gate keeps
  arming on every FlashForge MIPS board, Creator 5 included, which is what it does today.
- Rename `tests/test_helpers/ad5x_layout_fixture.h` to `flashforge_layout_fixture.h`
  (`make_flashforge_layout`), and add a `machine` argument that writes
  `/usr/prog/app_startup.sh` with `MACHINE=<name>` so board tests build a C5 root in one call.

**1c. Updater refuses a release with no build for this board (bug 1).**

- `src/system/update_checker.cpp#this_build_release_asset_keys` calls
  `release_asset_keys(current_key(), board::current())`. A Creator 5 / Pro asks for
  `{"mips"}` only, so no release without a `mips` asset can match.
- `parse_release_manifest` changes to match `parse_github_release`: a manifest that parses
  but lists none of the keys returns true with `info.download_url` empty, instead of false.
  `fetch_stable_release` then no longer falls back to GitHub for a missing asset (GitHub
  would have none either).
- New `bool release_has_build_for_this_board(const UpdateChecker::ReleaseInfo& info)`
  (true when `download_url` is non-empty). In the check worker's `Newer` and `Older` arms,
  a release without a build reports `Status::Error` instead of `UpdateAvailable`. The error
  is formatted on the main thread in `UpdateChecker::render_version_text()` (the
  `Status::Error` arm), because `lv_tr` is main-thread only; the worker stores the version
  and an error kind, not English text. `Same` keeps reporting `UpToDate`.
- User-facing message, translated:
  **"v%s has no build for the %s, so it can't be installed here. Pick a channel with a newer release."**
  with the release version and `board::current_info().display_name`; for Generic boards,
  which never hit this through the legacy path, "this printer" replaces the board name.
  This also replaces today's misleading "Error: No update available" at install time on
  every platform whose release lacks its asset.
- `start_download()`'s empty-URL guard stays as the last line of defence.

**1d. Per-board facts in their owning modules.**

- `touch_calibration.h`: new `std::optional<TouchCalibration> factory_touch_fit(board::Board)`.
  `platform_default_calibration()` keeps its signature and becomes
  `factory_touch_fit(board::current())` plus the existing validator check. The `#if` goes.

  | Board | a | b | c | d | e | f |
  |-------|---|---|---|---|---|---|
  | Ad5x | 1.171731 | -0.043628 | -66.965828 | -0.006188 | 1.478954 | -118.906227 |
  | Ad5m | 1.174004 | 0.009220 | -79.064400 | -0.000000 | -1.455497 | 570.492126 |
  | every other board | nullopt | | | | | |

  `capture_rotation` stays 0 for both (measured unrotated).
- `display_metrics.h`: `known_panel(const std::string&)` becomes `known_panel(board::Board)`
  and `resolve_dpi(..., const std::string& platform_key)` becomes
  `resolve_dpi(..., board::Board)`; `application.cpp` passes `board::current()` (bug 2).

  | Board | width_px | height_px | diagonal_mm |
  |-------|----------|-----------|-------------|
  | Ad5m | 800 | 480 | 107.0 |
  | Cc1 | 480 | 272 | 107.0 |
  | K1 | 480 | 800 | 108.6 |
  | K2 | 480 | 800 | 108.6 |
  | Ad5x, Creator5, Creator5Pro, SnapmakerU1, Generic | none (unmeasured or user-chosen) | | |

**1e. Storage: one board-keyed cache rung.**

- `helix_cache_dir_internal.h`: new pure
  `std::vector<CacheCandidate> board_cache_candidates(board::Board, FlashForgeModTree, const std::string& subdir)`;
  `cache_path_candidates()` calls it in place of the `#if` chain. The Android arm stays a
  compile-time arm (it is an SDL path, not a board). The `tier` label becomes the board key
  (`"k1"` instead of `"MIPS"`), which `diagnostics` and crash reports already carry as a
  string.

  | Board | tree | cache rung, in order |
  |-------|------|----------------------|
  | Ad5m | any | `/data/.helixscreen/cache/<subdir>` |
  | Cc1 | any | `/user-resource/helixscreen-state/cache/<subdir>` |
  | K2 | any | `/mnt/UDISK/helixscreen-state/cache/<subdir>`, `/usr/data/helixscreen-state/cache/<subdir>` |
  | K1 | any | `/usr/data/helixscreen-state/cache/<subdir>` |
  | Ad5x, Creator5, Creator5Pro | Zmod or None | `/srv/helixscreen-state/cache/<subdir>` |
  | Ad5x, Creator5, Creator5Pro | ForgeX | `/opt/config/mod_data/helixscreen-state/cache/<subdir>` |
  | SnapmakerU1 | any | `/userdata/helixscreen-state/cache/<subdir>` |
  | Generic | any | none |

  Sources: the Ad5x/Zmod row is `assets/config/platforms.json` `ad5x.storage.cache_dir` and
  `hooks-ad5x.sh`; the ForgeX row is `hooks-ad5x-forgex.sh`; U1 is `hooks-snapmaker-u1.sh`.
  Every device hook exports `HELIX_CACHE_DIR`, which wins at rung 1, so this rung only
  decides for a launch without the hook (a dev deploy). The AD5X/Z-Mod change from
  `/usr/data/...` to `/srv/...` moves only that case, and only a cache.
- `migrate_legacy_state_roots()`: `#if HELIX_PLATFORM_AD5M` becomes
  `if (board::current() != board::Board::Ad5m) return;`.
- `include/helix_install_roots.h`: `kStateRoots` gains
  `"/opt/config/mod_data/helixscreen-state"` (AD5X Forge-X), which `HELIX_STATE_DIRS` in
  `scripts/lib/installer/common.sh` already sweeps.
- `scripts/check_platform_manifest.py`: new `check_cache_rungs`: every manifest
  `storage.cache_dir` that starts with `/` appears in `src/system/helix_cache_dir.cpp`, the
  same text check `check_state_roots` makes against `kStateRoots`.

### Tests (Phase 1)

All in existing files unless named. Each fails if the feature is removed.

| Test | Proves | Fails when |
|------|--------|-----------|
| `board::resolve names every FlashForge MIPS board from its machine id` (`test_platform_info.cpp`) | AD5X / Creator5 / Creator5Pro roots resolve to their board on `"mips"` | the machine-id step is dropped (C5 roots resolve Ad5x) |
| `board::resolve calls a mod tree with no machine id an AD5X and nothing at all a K1` | Forge-X chroot root is Ad5x, plain root is K1 | the layout fallback or the K1 default is lost |
| `board::resolve never probes for a compile-time board` | `resolve("k2", <C5 root>)` is K2, `resolve("pi", <AD5X root>)` is Generic | a probe leaks into non-mips keys |
| `every Board has a row and its key round-trips` | `info(b).board == b` for every enumerator, keys unique | a row is missing or misordered |
| `flashforge_mod_tree keeps the layout truth table` | the five existing layout cases now return None/Zmod/ForgeX | the rename changes behaviour |
| `release_asset_keys gives a Creator 5 no legacy asset` (`test_update_checker.cpp`) | `{"mips"}` for Creator5/Pro, `{"mips","ad5x"}`, `{"mips","k1"}`, `{"k1"}` for key k1 + board K1 | the C5 falls back to `ad5x` again (bug 1) |
| `a Creator 5 is refused a release with no mips asset` | v1.0.5-shaped GitHub JSON (`ad5x`, `k1` assets) with board override Creator5Pro: worker reports `Status::Error`, not `UpdateAvailable` | the refusal is removed |
| `a manifest without this board's asset parses with an empty URL` | `parse_release_manifest` returns true, `download_url` empty, `version` set | the old false-return comes back |
| `an up-to-date release without an asset still reports UpToDate` | `Same` arm untouched | the refusal over-reaches |
| `factory_touch_fit ships fits for the AD5X and AD5M only` (`test_touch_calibration.cpp`) | the two rows equal `kShippedFits`; Creator5/Pro, K1, Generic are nullopt | the C5 gets the AD5X fit again |
| `known_panel finds the K1 on the unified build` (`test_display_metrics.cpp`) | `known_panel(Board::K1)` is the 480x800 row; `resolve_dpi(0, nullopt, K1).source == KnownPanel` | keyed on the platform key again (bug 2) |
| `the jz_pwm piezo is a FlashForge MIPS fact` (`test_jz_pwm_sound_backend.cpp`) | with override K1, `available()` is false even with `/dev/jz_pwm` faked; Creator5 passes the board gate | the gate goes back to the layout probe or drops the C5 |
| `only FlashForge firmware re-asserts brightness after boot` | rows: true for Ad5m/Ad5x/Creator5/Creator5Pro, false for Cc1 and Generic | the gate is keyed on a backlight driver again |
| `diagnostics names the board` (`test_diagnostics.cpp`) | `identity.board == "creator5-pro"` under override; crash report JSON has `board` | the field is dropped |
| `board_cache_candidates names each board's rung` (`test_cache_dir.cpp`) | every table row above, U1 included, Ad5x Zmod vs ForgeX split | a board loses its rung or the AD5X goes back to `/usr/data` |
| `is_printer_embedded follows the board` (`test_android_platform.cpp`) | Generic false, every other board true under override | the fold is wrong for a board |
| bats: `check_cache_rungs reports a manifest cache_dir the cascade lacks` | fixture manifest with an extra cache_dir yields a finding | the check is vacuous |

### Risks (Phase 1)

- `board::current()` is cached on first call. A test that sets an override must reset it
  (`set_current_override(std::nullopt)`) in teardown; `HelixTestFixture::reset_all()` gains
  that line so no test leaks a board.
- Function-local statics read `current()` (the tube stroker). The override must be set
  before first use in tests that care, the same constraint the memory probe has today.
- The updater change touches the manifest parser's contract. `test_update_checker.cpp`
  cases that expect `false` for a missing asset are rewritten, not deleted.
- C5 rows for `firmware_dims_backlight_after_boot` and `has_jz_pwm_piezo` are inferred
  (shared hooks, Preston's ruling on the piezo), not measured on a C5.

---

## Phase 2: filament systems answer for themselves

### What the code does today

`src/api/moonraker_discovery_sequence.cpp#build_subscription_objects` (the function taking
`hw, heaters, sensors, ..., afc_objects, filament_sensors, mcus`):

| Backend | How its objects get subscribed |
|---------|-------------------------------|
| AFC | `AmsBackendAfc::required_status_objects(afc_objects)`, unconditional, returns `json` |
| AD5X IFS | `AmsBackendAd5xIfs::required_status_objects(hw)` under `#if HELIX_HAS_IFS` and `mmu_type() == AD5X_IFS`, returns `vector<string>` |
| OpenAMS | `AmsBackendOpenAms::required_status_objects(hw)`, unconditional, returns `json` |
| Happy Hare | hardcoded 44-field `mmu` array when `mmu_type() == HAPPY_HARE` |
| ACE | hardcoded: `hw.ace_object_names()` or `ace` when `mmu_type() == ACE` |
| CFS | hardcoded `box`, `motor_control` when `mmu_type() == CFS` |
| Snapmaker | hardcoded `filament_detect`, two `filament_feed`, `print_task_config`, `machine_state_manager`, four `filament_motion_sensor e<N>_filament`, the batch macro, when `mmu_type() == SNAPMAKER` |
| QIDI Box | hardcoded `box_extras`, `save_variables` when `mmu_type() == QIDI_BOX` |

Plus a per-type logging block, and object bucketing that knows `zmod_ifs_switch_sensor ` and
`zmod_ifs_motion_sensor ` prefixes so the IFS backend's sensors land in `filament_sensors_`.

Runout ownership is two switches over `AmsType` in `src/system/runtime_config.cpp`
(`backend_owns_runout_surface`, `backend_owns_runout_during_job`), and
`src/print/filament_sensor_manager.cpp` computes `ad5x_idle_unload` from
`primary_type() == AD5X_IFS`. `ToolState::init_tools` (`src/printer/tool_state.cpp`)
hardcodes the U1's four toolheads under `hardware.has_snapmaker()`.

The precedent for the fix exists: `AmsBackend::sensor_belongs_to_backend(AmsType, ...)` is a
static per-type dispatch in `src/printer/ams_backend.cpp`, and every backend already has a
`static constexpr BackendTraits kTraits`. The name `AmsBackendRegistry` is taken (the live
backend instances); this phase adds no new "registry", only static dispatch beside the
existing one.

### Interfaces

`include/ams_backend.h`, static on `AmsBackend`, implemented in `ams_backend.cpp` next to
`sensor_belongs_to_backend` (that file already owns the `HELIX_HAS_IFS` / `HELIX_HAS_CFS` /
`HELIX_HAS_SNAPMAKER` arms):

```cpp
[[nodiscard]] static nlohmann::json required_status_objects(const helix::PrinterDiscovery& hw);
[[nodiscard]] static const std::vector<std::string>& filament_sensor_object_prefixes();
```

`required_status_objects(hw)` merges each backend's own static; each backend decides whether
it applies. New statics, each `[[nodiscard]] static nlohmann::json required_status_objects(const helix::PrinterDiscovery& hw)`:
`AmsBackendHappyHare`, `AmsBackendAce`, `printer::AmsBackendCfs`, `AmsBackendSnapmaker`,
`AmsBackendQidi`, plus an `AmsBackendAfc` overload over `hw.printer_objects()` that filters
`"AFC"` / `"AFC_*"` exactly as the bucketing does and calls the existing vector overload.
The IFS and OpenAMS statics keep their signatures; the dispatcher maps IFS names to `null`.
Each new static keeps today's gate: `hw.mmu_type() == <its type>` for HH, ACE, CFS,
Snapmaker, QIDI and IFS; AFC and OpenAMS stay unconditional.

`filament_sensor_object_prefixes()` returns the IFS backend's two prefixes, held as a
header constant `AmsBackendAd5xIfs::kSensorObjectPrefixes` so the bucketing stays
unconditional as it is today, with no `#if`.

`BackendTraits` gains three fields, all default false:

| Field | True for | Replaces |
|-------|----------|----------|
| `raises_runout_fault` | HAPPY_HARE, AFC, AD5X_IFS, CFS | `backend_owns_runout_surface()` |
| `raises_runout_fault_during_job` | HAPPY_HARE, AFC, CFS | `backend_owns_runout_during_job()` |
| `unloads_to_hub_between_prints` | AD5X_IFS | `ad5x_idle_unload` in `filament_sensor_manager.cpp` |

Values are copied from the two switches, case by case. The switch comments that justify
each value move onto the `kTraits` line that sets it.

`AmsBackendRegistry` and `AmsState` gain
`[[nodiscard]] std::optional<BackendTraits> primary_traits() const;`: backend 0's
`traits()` by value under the registry mutex, safe off the main thread exactly like
`primary_type()`. `filament_sensor_manager.cpp` reads it where it reads `primary_type()`
now; `RuntimeConfig::should_show_runout_modal()` reads `backend->traits()` on the main
thread. `backend_owns_runout_during_job` leaves `runtime_config.h`.

The comment on `backend_owns_runout_surface` argues for a switch because "does this class
override an error hook" is a fact about code, not data, and a flag can drift from it. The
test `runout_fault_traits_match_the_error_hooks` (below) closes that: it drives each
backend's existing runout fixture and checks the trait against what `classify_error()` /
`current_error()` actually produce. A trait left false keeps the generic modal, the same
safe direction the switch's fallthrough chose.

New module `include/fixed_tool_topology.h` + `src/printer/fixed_tool_topology.cpp`, z-offset
shape, added to the ESP32 `app_srcs.txt` (ToolState is in the cut):

```cpp
namespace helix::tool_topology {
struct FixedTool { std::string extruder; std::string heater; std::string fan; };
std::vector<FixedTool> fixed_toolheads(const PrinterDiscovery& hw);
const char* provider_name(const PrinterDiscovery& hw);
}
```

One provider row, keyed on `hw.has_snapmaker()`: extruders `extruder`, `extruder1`,
`extruder2`, `extruder3`, heater equal to the extruder, fans `fan`, `fan_generic e1_fan`,
`fan_generic e2_fan`, `fan_generic e3_fan`. `ToolState::init_tools` builds `ToolInfo` from
the list when it is non-empty and never names the vendor. `PrinterDiscovery::has_snapmaker()`
and `hardware_fingerprint.cpp` stay: discovery is the detection module.

### Steps

**2a.** Statics, dispatcher, prefix list; `build_subscription_objects` loses the six
hardcoded blocks, the `#if HELIX_HAS_IFS`, and the per-type log lines, which become one loop
logging `ams_type_to_string()` and the object names it added. `printer_discovery_parse.cpp`
is unchanged.

**2b.** Traits fields, `primary_traits()`, the two `runtime_config.cpp` switches deleted,
`ad5x_idle_unload` renamed `idle_unload_expected` and computed from the trait; the fixed
tool topology module.

### Tests (Phase 2)

| Test | Proves | Fails when |
|------|--------|-----------|
| `subscription objects are unchanged for every backend fixture` (`test_moonraker_discovery_subscriptions.cpp`, new or the existing subscription test file) | for HH, AFC, ACE (named and fallback), CFS, Snapmaker (with and without batch macro), QIDI, IFS, OpenAMS, toolchanger and a plain printer, the subscription map equals a golden captured from main before 2a | any object or field list drifts |
| `each backend's required_status_objects is empty off its own printer` | every static returns `{}` for a plain-printer discovery | a gate is lost and a printer subscribes another vendor's objects |
| `IFS sensor prefixes still bucket as filament sensors` | `zmod_ifs_switch_sensor head` lands in `filament_sensors_` | the prefix list is not consulted |
| `runout_fault_traits_match_the_error_hooks` (`test_ams_backend_traits.cpp`) | per backend, a runout fixture yields an error iff `raises_runout_fault` | a trait is set without an error hook, or an error hook is added without the trait |
| `runout ownership by traits matches the old switch` (`test_job_holds_machine_guards.cpp`, `test_runtime_config.cpp`) | every AmsType row of the deleted switches, through `traits()` | a value is mis-copied |
| `primary_traits is empty with no backend and safe off the main thread` | nullopt with no backend; a background-thread read under TSan-clean locking | the accessor hands out a pointer |
| `idle unload is expected only where the backend unloads to its hub` (`test_filament_sensor_manager*.cpp`) | an IFS backend idle-head-empty is swallowed, an ACE one is not | the trait is ignored |
| `fixed_toolheads gives the U1 four heads and everyone else none` (`test_fixed_tool_topology.cpp`) | the four rows; empty for a toolchanger and a plain printer | the provider row drifts |
| `ToolState builds the U1 heads from the fixed topology` (existing ToolState tests) | names, heaters and fans of all four tools | ToolState still forks on the vendor |

### Risks (Phase 2)

- Subscription parity is the whole contract; the golden must be captured from main before
  the refactor commit, in the same branch, and committed first.
- `primary_traits()` copies under the registry mutex; it must not call into a backend that
  takes its own lock first (the ABBA the sensor manager comment warns about). `traits()` is
  lock-free on every backend today; the test pins that with a TSan run of the new case.
- `moonraker_discovery_sequence.cpp`, `ams_backend.cpp`, `runtime_config.cpp`,
  `filament_sensor_manager.cpp` and `tool_state.cpp` are in the ESP32 cut:
  `scripts/check_esp32_app_srcs.py` and the no-exceptions gate apply.

---

## Phase 3: capability queries and leftovers

### Sites and changes

**Reduced effects, one rule for the tier.** `src/ui/ui_effects.cpp#full_style_effects_active`
and `src/ui/theme_lvgl_apply.cpp#full_style_effects_allowed_here` are two copies of the same
subject read; the second calls the first. `include/ui_spool_canvas.h` drops its duplicate
declaration of `helix::ui::reduced_effects()` and includes `filament_tube_stroker.h`. The
memory rule (`reduced_effects`) and the tier rule (`full_style_effects_allowed`) stay two
rules: they gate different costs (halo strokes versus a TRANSFORM layer per press), and
merging them changes the look on every BASIC-tier Pi. The K2 fact moved to the board table
in Phase 1.

**Config migrations.** `src/system/config_migrations.cpp#migrate_v12_to_v13` and
`#migrate_v14_to_v15` carry the same 25-line body twice. Both become calls to two
file-local helpers:

```cpp
bool config_names_printer(const nlohmann::json& config, const char* preset, const char* printer_type);
void restore_backlight_off_sleep(nlohmann::json& config, const char* log_tag);
```

Correction to the audit: the predicate must not move to `PrinterDetector`. A migration
freezes its detection rule at the version it repairs; asking `PrinterDetector` would make
an old migration's reach change whenever the printer database does. `ad5x` and
`FlashForge Adventurer 5X` stay as literals in this one file with `// VENDOR_OK:` on the call.

**AMS device-operations overlay.** `is_qidi_subject_` is already set from
`supports_configurable_eject_params()`; only the name is vendor. The subject becomes
`ams_device_ops_has_eject_params` (C++ and `ui_xml/ams_device_operations.xml`). The
persisted settings keys `qidi_eject_distance` / `qidi_eject_velocity` stay: renaming them
needs a config migration for no user benefit (out of scope).

**AMS context menu, eject hint.** `src/ui/ui_ams_context_menu.cpp` shows
`eject_force_move_hint` when `get_type() == QIDI_BOX && !supports_lane_eject()`. New virtual
on `AmsBackend`, mirroring the existing `unload_blocked_reason(int)`:

```cpp
[[nodiscard]] virtual std::string eject_unavailable_reason(int slot_index) const { return {}; }
```

`AmsBackendQidi` returns the translated force_move sentence. The XML's static hint label
binds to a `slot_eject_hint` string subject and its visibility subject, the same pattern as
`slot_unload_hint` a few lines above it.

**AMS device-section detail overlay, CFS actions.** The overlay checks
`get_type() == CFS` for `calibrate_purge_chute` and `calibrate_cutter`, and owns three CFS
statics (`s_cfs_cutter_phase`, `s_cfs_cutter_status`, its buffer) plus the
`cfs_cutter_status` row it creates in the calibration section. `DeviceAction` gains
`std::string guided_flow;`, and a new `include/ui_guided_device_flow.h`:

```cpp
namespace helix::ui {
struct GuidedDeviceFlow {
    std::function<void(lv_obj_t* actions_container)> attach_status;
    std::function<void(const std::string& label)> start;
};
void register_guided_device_flow(const std::string& id, GuidedDeviceFlow flow);
const GuidedDeviceFlow* find_guided_device_flow(const std::string& id);
}
```

`AmsBackendCfs` sets `guided_flow = "cfs_purge_chute"` and `"cfs_cutter"` on its two
actions. A new `src/ui/ui_cfs_device_flows.cpp` (under `HELIX_HAS_CFS`) registers both
flows and takes the cutter subjects, `confirm_cutter_calibration` and the chute overlay
launch out of the generic overlay. The overlay's rule becomes: an action with a registered
guided flow starts it, every other action executes and toasts.

**Pre-print send.** `PrintStartController::send_snapmaker_preprint_then` is already gated on
`requires_preprint_send()`. Rename to `send_backend_preprint_then`, and its log lines say
"pre-print config" instead of "U1 pre-print config".

**Slot override legacy migration.** `src/printer/filament_slot_override_store.cpp#try_migrate_legacy`
checks `backend_id != "ace" && backend_id != "cfs"` (once, not twice as the audit says). The
store's constructor gains `bool migrates_legacy_namespace` (default false); the ACE and CFS
backends pass true where they construct their store, and the string list goes.

**Leftovers that stay, baselined in the ratchet with a reason.**

| Site | Why it stays |
|------|--------------|
| `src/printer/printer_discovery_parse.cpp`, `include/printer_discovery.h` | the detection module: naming vendors is its job |
| `src/ui/ui_wizard_ams_identify.cpp` | the picker lists the systems by type |
| `src/system/telemetry_manager.cpp` | maps types to telemetry names, which is data |
| `src/ui/modals/buffer_status_modal.cpp`, `src/ui/ui_ams_detail.cpp` (`HAPPY_HARE`, `AFC` buffer shapes) | these read backend-specific `AmsSystemInfo` fields; the fix is a buffer presentation model on the backend, a separate design |
| `src/ui/filament_op_execute.cpp` (`TOOL_CHANGER`) | `is_tool_changer` versus `load_mounts_tool`: Snapmaker differs, needs its own look |
| `src/printer/hardware_validator.cpp`, `include/filament_op_dispatch.h` | small; next ratchet pass |
| `src/printer/plr_backend.cpp` `CREALITY_DATA_ROOTS` | inside the power-loss provider module, already the right shape |
| `src/system/config.cpp#stale_default_macro_text` | data keyed by preset name |
| `src/rendering/gcode_parser.cpp` `creality` thumbnail flag | a file-format name, not a vendor branch |

### Tests (Phase 3)

| Test | Proves | Fails when |
|------|--------|-----------|
| `both AD5X sleep migrations repair the same stale pair` (`test_config_migrations.cpp`) | v13 and v15 each flip `sleep_backlight_off`/`hardware_blank` for preset `ad5x` and for a saved `FlashForge Adventurer 5X` printer, leave others alone | the helper's predicate drifts |
| `QIDI explains an unavailable eject` (`test_ams_backend_qidi.cpp`) | non-empty reason with force_move off and filament present; empty otherwise; base class empty | the hint goes back to a type check |
| `the context menu shows a backend's eject reason` (`test_ams_context_menu.cpp`) | the hint subject carries the backend's text | the UI ignores the virtual |
| `CFS actions name their guided flows` (`test_ams_backend_cfs.cpp`) | the two action ids carry `guided_flow` | the routing returns to `get_type()` |
| `the section overlay starts a registered flow and executes the rest` | a test flow registered under an id is started; an unregistered action executes | the overlay ignores `guided_flow` |
| `only ACE and CFS stores migrate the legacy namespace` (`test_filament_slot_override_store.cpp`) | a store built with false never reads the legacy key | the flag is ignored |
| XML: `ams_device_operations.xml binds has_eject_params` | `scripts/check_esp32_xml_bindings.py` and the orphan-subject gate stay green with the rename | the XML still names the old subject |

### Risks (Phase 3)

- The guided-flow registry is new UI plumbing: registration must happen before the overlay
  is first built (`register_xml_components()` order), or the actions fall back to execute
  and toast, which runs the cutter sweep without its confirmation. The overlay test pins the
  confirm path.
- The XML subject rename needs the hot-reload check: a stale XML binding to the old name is
  silently skipped at runtime, so the binding gate is the only guard.

---

## Lint gate (Phase 1 step 1a)

`scripts/check_vendor_branches.py`, a per-file ratchet in the shape of
`scripts/check_comment_archaeology.py` (counts may fall and never rise;
`--write-baseline` writes `scripts/vendor_branch_baseline.txt`), scanning `src/` and
`include/` `.cpp`/`.h`. Three shapes, each skipped on a line carrying or preceded by
`// VENDOR_OK: <reason>`:

1. A preprocessor line naming `HELIX_PLATFORM_AD5M`, `_AD5X`, `_MIPS`, `_K1`, `_K2`, `_CC1`
   or `_SNAPMAKER_U1`. Owners exempt: `src/system/platform_table.cpp`,
   `src/system/platform_info.cpp`, `src/system/board_identity.cpp`. `_ESP32`, `_ANDROID`,
   `_X86`, `_X86_DESKTOP` and `_PI32` are build shapes, not vendors, and are not matched.
   Baseline today: `helix_cache_dir.cpp` 5, `touch_calibration.cpp` 2,
   `filament_tube_stroker.cpp` 1. Phase 1 drives all three to 0.
2. A comparison or `case` on any `AmsType::` value other than `NONE`. Owners exempt:
   `src/printer/ams_*`, `include/ams_*`, `*_mock*`. Baseline today: `runtime_config.cpp`
   18, `moonraker_discovery_sequence.cpp` 12, `telemetry_manager.cpp` 11,
   `printer_discovery_parse.cpp` 11, `ui_wizard_ams_identify.cpp` 9,
   `ui_ams_device_section_detail_overlay.cpp` 3, `buffer_status_modal.cpp` 2,
   `hardware_validator.cpp` 2, `ui_ams_detail.cpp` 1, `ui_ams_context_menu.cpp` 1,
   `filament_op_execute.cpp` 1, `filament_sensor_manager.cpp` 1, `printer_discovery.h` 1,
   `filament_op_dispatch.h` 1. Phase 2 and 3 drive `runtime_config`,
   `moonraker_discovery_sequence`, `filament_sensor_manager`, both AMS overlays and the
   context menu to 0.
3. A call to `flashforge_machine_id(`, `flashforge_mod_tree(` or `->name() ==`. Owners
   exempt: `platform_info.cpp`, `board_identity.cpp`, `log_collector.cpp`,
   `diagnostics.cpp`, `helix_cache_dir.cpp`. Baseline is taken after the 1b rename.

Wiring: `scripts/qc/vendor_branches.sh` (`qc_vendor_branches`, staged `src/`/`include/`
only), and in `tests/shell/test_code_lint.bats` one gate test (`no new vendor branches in
generic code`) plus one shape test (`the vendor-branch gate catches each shape and honours
VENDOR_OK`) that writes a temp file per shape and asserts each is reported and the opt-out
is not. A new `tests/shell/` case means the whole bats suite runs before landing.

## Migration and compatibility notes

- Renames: `ad5x_mod_layout_present` to `flashforge_mod_tree` (C++, ESP32 stub, comments in
  `helix-launcher.sh`, `platform_table.cpp`, `log_collector.h`); `ad5x_layout_fixture.h` to
  `flashforge_layout_fixture.h`; `send_snapmaker_preprint_then` to
  `send_backend_preprint_then`; subject `ams_device_ops_is_qidi` to
  `ams_device_ops_has_eject_params`.
- Shell twins stay in step without edits: the launcher's four probes equal
  `flashforge_mod_tree()`'s, and the installer's `ff_machine_id()` equals
  `flashforge_machine_id()`; both pairs keep their existing cross-pinned tests.
- `assets/config/platforms.json`: no schema change. The Creator 5 has no manifest row (it
  installs as the `ad5x` platform key); this work does not add one.
- Crash reports and bundles gain a `board` field; the dashboard can prefer it over
  `platform_model` but need not change.
- Telemetry keeps `platform::current_key()` (`"mips"`); adding the board is a product call,
  open question 3.
- Peer overlap: the `mismatch-toast` worktree edits `src/application/hardware_setup_prompter.cpp`
  and the notification history panel. Nothing here touches either.

## Out of scope

- Splitting `mod_flavor` into ZMOD versus stock FlashForge until the `/ZMOD` marker is
  checked from inside the Z-Mod chroot on an AD5X and a C5.
- Enabling the loopback camera probe on the AD5M.
- Shipping `fx-pwm` for Z-Mod boards so the jz_pwm piezo plays there.
- Subscribing every detected filament system rather than the primary one (a behaviour
  change; Phase 2 is parity).
- A buffer presentation model for HH/AFC buffer modals; `is_tool_changer` in
  `filament_op_execute.cpp`.
- Renaming persisted settings keys (`qidi_eject_*`).
- Measuring panel diagonals for the AD5X, Creator 5 and U1.
- The ESP32 `HELIX_PLATFORM_ESP32` arms: build shape, not vendor.

## Open questions for Preston (product calls only)

1. The Creator 5 refusal message: "v%s has no build for the %s, so it can't be installed
   here. Pick a channel with a newer release." Is that the wording, and should the updates
   panel also hide the Stable channel option on a board whose stable release has no build?
2. Z-Mod boards (AD5X and Creator 5 on Z-Mod) have the piezo but no `fx-pwm`, so they play
   UI sounds through M300. Do we want HelixScreen to carry its own `fx-pwm` so the native
   jz_pwm backend works there too? (Out of scope here either way.)
3. Telemetry and crash dashboards: report the resolved board (`creator5-pro`) alongside the
   platform key (`mips`)?

## Size estimate

| Phase | Files | Lines (incl. tests) |
|-------|-------|---------------------|
| 1 | about 32 (2 new, plus the gate script, baseline, qc file) | about 1,300: 150 module, 200 gate, 450 call sites and stubs, 500 tests |
| 2 | about 22 (2 new) | about 900: 250 moved into backends, 200 removed from discovery and runtime_config, 400 tests incl. goldens |
| 3 | about 18 (2 new) | about 650: 150 moved CFS flow, 200 call sites and XML, 300 tests |
