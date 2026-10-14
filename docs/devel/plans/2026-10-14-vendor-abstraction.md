# Vendor Abstraction Implementation Plan

> **For agentic workers:** use superpowers:subagent-driven-development (one fresh implementer per task, one fresh reviewer per task) or superpowers:executing-plans. Steps use `- [ ]` checkboxes. Per this repo's CLAUDE.md "Superpowers Skills Here", this plan gives interfaces, tests and constraints; the bodies are yours.

**Goal:** one module answers each vendor question (which board, which filament objects, who owns a runout), and generic code asks it.

**Architecture:** Phase 1 adds `helix::board` (an enum, one row per board, a cached runtime resolve for the unified MIPS build) and points every board-keyed site at it. Phase 2 moves each filament backend's subscription list and runout ownership into the backend, behind static dispatch in `ams_backend.cpp` and three new `BackendTraits` fields. Phase 3 replaces the remaining `AmsType` checks in AMS UI with capability queries.

**Tech stack:** C++17, LVGL 9.5 XML, Catch2 (`make t F='[tag]'`), bats (`tests/shell/`), Python gate scripts, TypeScript telemetry worker (vitest).

**Spec:** `docs/devel/plans/2026-10-14-vendor-abstraction-design.md` (approved 2026-10-14). Read it before your task: it holds the board table, the cache-rung table, the touch-fit and panel values, and the reasons. This plan does not repeat the values; it cites the spec section.

## Global Constraints

- Rule: `.claude/rules/vendor-abstraction.md`. Reference shape: `include/z_offset_persistence.h` + `src/printer/z_offset_persistence.cpp`.
- Every file you touch that is listed in `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` builds `-fno-exceptions -fno-rtti`: no `try`/`throw`, no `j.value()`/`j.at()`, no one-arg `json::parse`. `scripts/check_esp32_app_srcs.py` gates it.
- `#include "hv/json.hpp"`, never `<nlohmann/json.hpp>`. spdlog only. SPDX header on new files: `// SPDX-License-Identifier: GPL-3.0-or-later`.
- No comment archaeology: comments state what the code does now. No commit SHAs, no "used to", no "before this change". Issue cites as bare `(prestonbrown/helixscreen#N)` only.
- No em-dashes in any prose, comment, commit message or translated string.
- Updater refusal message, verbatim, translated with `lv_tr`: `v%s has no build for the %s, so it can't be installed here. Pick a channel with a newer release.` Stable stays selectable; the message is the only change in the updates UI.
- Telemetry gains the resolved board key (`creator5-pro`) next to the platform key (`mips`). Deploying the telemetry worker is outward-facing: the task changes and tests the worker, and the deploy waits for Preston's explicit OK.
- Board keys, exactly: `generic`, `ad5m`, `ad5x`, `creator5`, `creator5-pro`, `k1`, `k2`, `cc1`, `snapmaker-u1`.
- Headless always: `HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy` on any app or UI-test run.
- Builds: plain `make` / `make t`, never `-jN`. Before building, `scripts/helix-claim check build:<tree>`; take it when FREE.
- Inner loop is `make t F='[tag]'`. `make mutate-diff`, ASAN and `make full-test-run` run once per phase, at the end (the last task of each phase), never per task.
- Commits: `git commit -m "..."` with a simple double-quoted string, explicit paths, then `git show --stat HEAD`.
- Test-first: every task writes its test, runs it and watches it fail for the stated reason, then implements. "The mutation" in each task is the edit a reviewer applies to prove the test can go red; the reviewer applies it, runs the tag, sees red, reverts.

## Review Focus

1. **A test leaks a board override.** `board::current()` is cached and overridable; a test that sets Creator5 and does not reset it changes every later test's board in the shard. Owned by Task 1.3: `HelixTestFixture::reset_all()` resets the override, and a test proves a fixture-derived case starts at the build's real board.
2. **A function-local static caches the board before a test sets it** (`reduced_effects()` in the tube stroker). Owned by Task 1.8: the reduced-effects test computes from `board::info()` rows through a pure helper, not through the cached function.
3. **The C5 refusal fires on an up-to-date release, or on a platform whose release simply lacks notes.** Owned by Task 1.4: `Same` with an empty URL stays `UpToDate`, and a pi build with a full asset list never sees the refusal.
4. **Subscription drift in Phase 2 that a field-level test misses** (an object subscribed with `nullptr` instead of a field list, which costs Klipper serialization on every tick). Owned by Task 2.1/2.3: the goldens compare values, not just keys.
5. **Guided flow not registered before the overlay builds**, so the cutter sweep runs with no confirmation. Owned by Task 3.4: a test builds the overlay before and after registration and asserts the unregistered path never executes `calibrate_cutter`.

---

## Worktrees, branches, and who owns what

| Phase | Worktree | Branch | Built by |
|-------|----------|--------|----------|
| 1 | `.worktrees/vendor-abstraction` (this tree) | `feature/vendor-abstraction` | agent A |
| 2 | `.worktrees/ams-status-dispatch` | `feature/ams-status-dispatch`, created from this branch's HEAD so it carries the spec and plan commits | agent B, at the same time as A |
| 3 | `.worktrees/vendor-abstraction-ui` | `feature/vendor-abstraction-ui`, created from `main` after Phases 1 and 2 have both landed | one agent |

Create Phase 2's tree from the main tree with `scripts/setup-worktree.sh --base feature/vendor-abstraction feature/ams-status-dispatch`, so it starts from the commit that carries the spec and this plan. Phase 3: `scripts/setup-worktree.sh feature/vendor-abstraction-ui` (default base, `origin/main`).

**Bug 1 hotfix.** A hotfix (a Creator 5 / Pro asks only for the `mips` asset and refuses a release without one) lands on `main` first. Phase 1 rewrites that code in Task 1.4. Agent A merges `origin/main` into `feature/vendor-abstraction` before starting Task 1.4 (`git -c merge.autoStash=false merge origin/main`, in this tree) and keeps any hotfix test whose assertion still holds, rewritten to the new signatures.

### Files both Phase 1 and Phase 2 touch

| File | Phase 1 change | Phase 2 change | How to avoid the conflict |
|------|----------------|----------------|---------------------------|
| `src/api/moonraker_discovery_sequence.cpp` | Task 1.8: one line in `#continue_discovery_objects` (`probe_local_cameras`), include `board_identity.h` | Tasks 2.3: `#build_subscription_objects` body, the subscription log block, the object bucketing in `#parse_objects`-side loop, includes of `ams_backend_*.h` | Hunks are hundreds of lines apart. The include block is the one likely textual conflict: Phase 1 adds `board_identity.h` and keeps `platform_info.h`; Phase 2 may drop `ams_backend_ace.h`-style includes. The second lander resolves by taking the union minus what its own phase removed, then lets clang-format sort. Neither phase reorders includes it does not own. |
| `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` | Task 1.3 adds `src/system/board_identity.cpp` | Task 2.6 adds `src/printer/fixed_tool_topology.cpp` | Different sorted sections (`src/system/` vs `src/printer/`); expect a clean merge. Each phase runs `scripts/check_esp32_app_srcs.py` after its own edit. |
| `scripts/vendor_branch_baseline.txt` | Task 1.1 creates it; Task 1.10 lowers it | lowers counts in `runtime_config.cpp`, `moonraker_discovery_sequence.cpp`, `filament_sensor_manager.cpp` without editing the file | **Phase 1 owns the file.** The gate fails only when a count rises, so Phase 2's lower counts pass against Phase 1's baseline untouched. Phase 2 never edits the baseline or the script. Whichever phase lands second runs `scripts/check_vendor_branches.py --write-baseline` once in its landing merge commit. If Phase 2 lands first, there is no baseline yet and nothing to do. |
| `scripts/check_vendor_branches.py`, `scripts/qc/vendor_branches.sh`, `tests/shell/test_code_lint.bats` | Task 1.1 | none | Phase 2 must not edit them. |
| `docs/devel/plans/2026-10-14-vendor-abstraction*.md` | none after this commit | none | Both branches carry the identical commits. Phase 3 deletes both files in its last task. |
| `docs/devel/CHANGELOG_1_1_DRAFT.md` | Task 1.10 adds the user-visible lines (updater refusal, K1 DPI source, board in telemetry) | none (Phase 2 is parity, nothing user-visible) | Only Phase 1 writes it. |

Files that look shared but are not: `include/ams_backend.h`, `include/ams_types.h`, `src/printer/ams_backend*.cpp`, `src/system/runtime_config.cpp`, `src/print/filament_sensor_manager.cpp`, `src/printer/tool_state.cpp` are Phase 2 only. `include/platform_info.h`, `src/system/platform_info.cpp`, `tests/helix_test_fixture.h`, the ESP32 stubs file and `src/system/telemetry_manager.cpp` are Phase 1 only. Phase 3 touches `ams_backend.h` and `ams_types.h` again, which is why it branches after both land.

---

## Phase 1: board identity

Parallelism: 1.1 and 1.2 can run in either order (disjoint files). 1.3 needs 1.2. Tasks 1.4 through 1.9 each need 1.3 and are otherwise independent of each other (disjoint files; noted where two share one), so a controller can hand them to sequential implementers in any order; they are not run concurrently in one worktree because they share one build tree. 1.9 also needs 1.2. 1.10 is last.

### Task 1.1: vendor-branch ratchet gate

**Files:**
- Create: `scripts/check_vendor_branches.py`, `scripts/vendor_branch_baseline.txt`, `scripts/qc/vendor_branches.sh`
- Modify: `scripts/quality-checks.sh` (register `qc_vendor_branches` the way `qc_l081` is registered, with a `QC_TRIGGER_vendor_branches` on `^(src|include)/`), `tests/shell/test_code_lint.bats`, `scripts/CLAUDE.md` (gate table row)

**Interfaces:**
- Produces: `scripts/check_vendor_branches.py [--list] [--write-baseline] [--staged-only] [--root DIR]`. Exit 0 when every per-file count is at or below baseline (prints a "can be lowered" line for files below it), exit 1 when any count rises or a file absent from the baseline has a nonzero count. `--root` points the scan at a fixture tree for the bats tests. Baseline format: one `<count> <path>:<shape>` per line, sorted, shape one of `platform-macro`, `amstype-branch`, `board-probe`.
- Shapes (spec "Lint gate"): (1) a preprocessor line naming `HELIX_PLATFORM_AD5M|AD5X|MIPS|K1|K2|CC1|SNAPMAKER_U1`; (2) `==`/`!=`/`case` on `AmsType::<X>` with X not `NONE`; (3) a call `flashforge_machine_id(`, `ad5x_mod_layout_present(`, `flashforge_mod_tree(`, or `->name() ==`. Exempt owners per shape exactly as the spec lists, plus `*_mock*` for shape 2. A line containing `VENDOR_OK:` or directly preceded by a comment line containing it is skipped.

- [ ] **Step 1: write the failing bats tests** in `tests/shell/test_code_lint.bats`:
  - `@test "no new vendor branches in generic code"`: `run python3 scripts/check_vendor_branches.py`; `[ "$status" -eq 0 ]`.
  - `@test "the vendor-branch gate catches each shape and honours VENDOR_OK"`: build `$BATS_TEST_TMPDIR/root/src/ui/x.cpp` with one line per shape, an empty baseline, run with `--root`; assert status 1 and that the output names all three shapes; then add `// VENDOR_OK: test` above each line and assert status 0.
- [ ] **Step 2:** run `bats tests/shell/test_code_lint.bats -f "vendor"`; expect both to fail (script missing).
- [ ] **Step 3:** implement the script, generate the baseline with `--write-baseline`, add the qc file and registration.
- [ ] **Step 4:** run the same bats filter; both pass. Check the baseline matches the spec's "Baseline today" numbers for shapes 1 and 2 (`ad5x_mod_layout_present` callers make shape 3's baseline).
- [ ] **Mutation:** delete shape 2's pattern from the script; the shape test goes red.
- [ ] **Step 5:** commit `"test(lint): ratchet vendor branches in generic code"`.

### Task 1.2: `flashforge_mod_tree` replaces `ad5x_mod_layout_present`

**Files:**
- Modify: `include/platform_info.h`, `src/system/platform_info.cpp`, `src/system/log_collector.cpp` (`#default_file_paths`), `include/system/log_collector.h` (comment), `src/system/diagnostics.cpp` (`#collect` mod_flavor line), `src/system/jz_pwm_sound_backend.cpp` (`#available`, keep behaviour: `!= None`), `src/system/update_checker.cpp` (`#this_build_release_asset_keys`, keep behaviour), `src/system/debug_bundle_collector.cpp` (keep behaviour), `src/system/platform_table.cpp` (comment), `scripts/helix-launcher.sh` (comment only), `firmware/helixscreen-esp32/components/helixapp/helixapp_platform_stubs2.cpp`
- Rename: `tests/test_helpers/ad5x_layout_fixture.h` to `tests/test_helpers/flashforge_layout_fixture.h`; update `tests/unit/test_platform_info.cpp`, `tests/unit/test_log_collector.cpp`, `tests/unit/test_diagnostics.cpp`
- `ad5x_board_present()` keeps working in this task (its body calls `flashforge_mod_tree`); Task 1.3 deletes it.

**Interfaces:**
- Produces: `enum class FlashForgeModTree { None, Zmod, ForgeX };`, `FlashForgeModTree flashforge_mod_tree(const std::string& probe_root = "/");`, `const char* flashforge_mod_tree_name(FlashForgeModTree tree);` returning `"none"`, `"ZMOD"`, `"Forge-X"`. In `helix::test`: `std::filesystem::path make_flashforge_layout(const std::filesystem::path& base, const std::string& layout, const std::string& machine = "");` where a non-empty `machine` also writes `<root>/usr/prog/app_startup.sh` containing `MACHINE=<machine>`.

- [ ] **Step 1:** rewrite the five layout cases in `test_platform_info.cpp` against the new API: `TEST_CASE("flashforge_mod_tree names each layout", "[platform_info]")` with `CHECK(flashforge_mod_tree(make_flashforge_layout(tmp, "zmod-marker")) == FlashForgeModTree::Zmod)`, `zmod-prog` Zmod, `forgex` ForgeX, `forgex-host` ForgeX, `plain` None; `CHECK(std::string(flashforge_mod_tree_name(FlashForgeModTree::ForgeX)) == "Forge-X")`. Keep the "requires the marker kinds, not just names" case. Add `TEST_CASE("make_flashforge_layout writes the machine id", "[platform_info]")`: `flashforge_machine_id(make_flashforge_layout(tmp, "zmod-prog", "Creator5Pro")) == FlashForgeMachine::Creator5Pro`.
- [ ] **Step 2:** `make t F='[platform_info]'` fails to compile (new names missing).
- [ ] **Step 3:** implement; same four stat probes, same order. Update the ESP32 stub to `flashforge_mod_tree` returning `None` and `flashforge_mod_tree_name`.
- [ ] **Step 4:** `make t F='[platform_info]'`, `make t F='[log_collector]'`, `make t F='[diagnostics]'` pass; `bats tests/shell/test_helix_launcher_env.bats` passes unchanged; `python3 scripts/check_esp32_app_srcs.py` passes.
- [ ] **Mutation:** make the Forge-X arm return `Zmod`; `[platform_info]` goes red.
- [ ] **Step 5:** commit `"refactor(platform): name the FlashForge mod-tree probe for what it detects"`.

### Task 1.3: the board module

**Files:**
- Create: `include/board_identity.h`, `src/system/board_identity.cpp`, `tests/unit/test_board_identity.cpp`
- Modify: `include/platform_info.h` + `src/system/platform_info.cpp` (delete `ad5x_board_present` and `local_camera_probe_safe`; fold `is_printer_embedded` and `platform_defaults_to_external_updates` onto the board; overrides kept), `src/ui/touch_calibration.cpp` (`helix::ad5x_board_present()` becomes `board::current() == board::Board::Ad5x`; Task 1.6 replaces it properly), `src/api/moonraker_discovery_sequence.cpp` (`helix::local_camera_probe_safe()` becomes `helix::board::current_info().local_camera_probe_safe`; add the include, keep `platform_info.h`) **(shared file, see overlap table)**, `tests/helix_test_fixture.h` (`reset_all()` calls `helix::board::set_current_override(std::nullopt)`), `lv_conf.h` and `tests/unit/test_android_platform.cpp` (drop the `HELIX_PLATFORM_AD5X` arm), `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` (add `src/system/board_identity.cpp`) **(shared file)**, `helixapp_platform_stubs2.cpp` (add `flashforge_machine_id` returning `std::nullopt`), `tests/unit/test_platform_info.cpp` (move the `ad5x_board_present` and `local_camera_probe_safe` cases to board tests), `Makefile` only if new `src/system/*.cpp` files are not picked up by wildcard (check first).

**Interfaces:** exactly the header in the spec, section "New module". `Board resolve(const std::string& platform_key, const std::string& probe_root = "/");`, `Board current();`, `const Info& info(Board board);`, `inline const Info& current_info();`, `void set_current_override(std::optional<Board> board);`. Rows: the spec's board table, value for value. `current()` caches `resolve(platform::current_key(), "/")` in a function-local static; the override, when set, wins over the cache. Thread note in the header: the cached value never changes after the first call; the override is test-only and main-thread only.

- [ ] **Step 1:** `test_board_identity.cpp`, tag `[board]`:
  - `"board::resolve names every FlashForge MIPS board from its machine id"`: for `AD5X`, `Creator5`, `Creator5Pro`, `resolve("mips", make_flashforge_layout(tmp, "zmod-prog", id))` is `Ad5x`, `Creator5`, `Creator5Pro`.
  - `"board::resolve calls a mod tree with no machine id an AD5X and nothing at all a K1"`: `forgex` root is `Ad5x`; `plain` root is `K1`.
  - `"board::resolve never probes for a compile-time board"`: `resolve("k2", <Creator5 root>) == K2`; `resolve("pi", <AD5X root>) == Generic`; `resolve("ad5m", <plain>) == Ad5m`; `resolve("esp32", <AD5X root>) == Generic`; `resolve("k1", <AD5X root>) == K1`.
  - `"every Board has a row and its key round-trips"`: for each enumerator, `info(b).board == b`, keys are the nine Global Constraints keys, unique.
  - `"board rows carry the spec's values"`: one `CHECK` per non-default cell of the spec's board table (camera false on Ad5m/Ad5x/Creator5/Creator5Pro; jz_pwm on Ad5x/Creator5/Creator5Pro only; reduced_effects K2 only; fw dims backlight Ad5m/Ad5x/Creator5/Creator5Pro; fw manages updates SnapmakerU1 only; legacy asset `"ad5x"` on Ad5x, `"k1"` on K1, nullptr elsewhere).
  - `"the override wins and the fixture resets it"`: a `HelixTestFixture`-derived case sets `Creator5Pro`, checks `current()`; a second fixture-derived case checks `current() == resolve(platform::current_key())`.
  - In `test_android_platform.cpp`: `"is_printer_embedded follows the board"`: under override, Generic false, each other board true.
- [ ] **Step 2:** `make t F='[board]'` fails to compile.
- [ ] **Step 3:** implement; delete the two `platform_info` functions and their tests; fix the two callers named above.
- [ ] **Step 4:** `make t F='[board]'`, `make t F='[platform_info]'`, `make t F='[android]'`, `make t F='[touch]'` pass; `scripts/check_esp32_app_srcs.py` passes.
- [ ] **Mutation:** in `resolve`, drop the machine-id step (go straight to the layout fallback); the first test goes red (Creator5 resolves Ad5x).
- [ ] **Step 5:** commit `"feat(platform): one board identity for every board-keyed decision"`.

### Task 1.4: the updater refuses a release with no build for this board

Before starting: merge `origin/main` (the bug 1 hotfix) into this branch.

**Files:**
- Modify: `include/platform_table.h`, `src/system/platform_table.cpp`, `include/system/update_checker.h`, `src/system/update_checker.cpp`, `tests/unit/test_update_checker.cpp`, `docs/devel/UPDATE_SYSTEM.md` (one paragraph: board-keyed asset fallback and the refusal)

**Interfaces:**
- Consumes: `board::Board`, `board::current()`, `board::info(b).legacy_release_asset`, `board::current_info().display_name`.
- Produces: `std::vector<std::string> helix::platform::release_asset_keys(const std::string& key, board::Board board);` (key, then the legacy asset when non-null and different). `bool helix::release_has_build_for_this_board(const UpdateChecker::ReleaseInfo& info);` (true iff `download_url` non-empty). `ShownVersionText` gains `bool no_build_for_board = false;`. `parse_release_manifest` returns true with an empty `download_url` (and `version`, `tag_name`, notes filled) when no key matches.
- Behaviour: in the check worker's `Newer` and `Older` arms, `!release_has_build_for_this_board(info)` reports `Status::Error` with the `info` attached. In `report_result`'s main-thread path, an `Error` carrying an `info` without a build sets `shown_ = {Status::Error, info->version, false, "", true}`. `render_version_text()` formats the Global Constraints message with the version and `board::current_info().display_name`, or `lv_tr("this printer")` when that is empty. `start_download()`'s empty-URL guard stays.

- [ ] **Step 1:** tests in `test_update_checker.cpp`, tag `[update_checker]`:
  - `"release_asset_keys gives a Creator 5 no legacy asset"`: `release_asset_keys("mips", Creator5) == {"mips"}`, same for `Creator5Pro`; `("mips", Ad5x) == {"mips","ad5x"}`; `("mips", K1) == {"mips","k1"}`; `("k1", K1) == {"k1"}`; `("pi", Generic) == {"pi"}`. Replace the old bool-overload cases.
  - `"a manifest without this board's asset parses with an empty URL"`: a manifest with only `ad5x` and `k1` assets, keys `{"mips"}`: returns true, `download_url.empty()`, `version == "1.0.5"`.
  - `"a Creator 5 is refused a release with no mips asset"`: with board override `Creator5Pro` and the v1.0.5-shaped GitHub release JSON (assets `helixscreen-ad5x-v1.0.5.tar.gz`, `helixscreen-k1-v1.0.5.tar.gz`, no mips), drive the check path the existing worker tests use and assert the reported status is `Error`, not `UpdateAvailable`, and the rendered version text contains `"no build for the FlashForge Creator 5 Pro"`.
  - `"an up-to-date release without an asset still reports UpToDate"`: current version equal to the release version, no matching asset: `UpToDate`.
  - `"a release with this board's asset is offered normally"`: board `K1`, release with a `k1` asset only: `UpdateAvailable`, URL is the k1 asset.
- [ ] **Step 2:** `make t F='[update_checker]'` fails.
- [ ] **Step 3:** implement. Fold the hotfix's code into the new path; delete what it duplicates.
- [ ] **Step 4:** `make t F='[update_checker]'` passes, including every pre-existing case (rewrite, do not delete, the ones that expected `false` from the manifest parser).
- [ ] **Mutation:** make `release_asset_keys` append `"ad5x"` for Creator5; the C5 tests go red. Second mutation: report `UpdateAvailable` in the `Older` arm regardless of the build; the refusal test goes red.
- [ ] **Step 5:** commit `"fix(update): a board is never offered another board's build (refuses with a message instead)"`.

### Task 1.5: the board in diagnostics, crash reports, bundles and telemetry

**Files:**
- Modify: `include/system/diagnostics.h` + `src/system/diagnostics.cpp` (`identity.board`), `include/system/crash_reporter.h` + `src/system/crash_reporter.cpp` (`board` in JSON, text report Platform line `Platform:  <key> / <board> (<mod_flavor>)`, GitHub body line), `src/system/debug_bundle_collector.cpp#collect_printer_info` (the `if (platform == "mips")` block becomes `board::current_info().display_name`, empty for Generic; bundle `diagnostics` object gains `board`), `src/system/telemetry_manager.cpp` (session `app["board"]`, crash event `app_board` beside `app_platform`), `server/telemetry-worker/src/analytics.ts` (session blob11 = `app.board ?? ""`, replacing the first empty placeholder; comment like blob10's), `server/telemetry-worker/src/__tests__/index.test.ts`, `docs/devel/TELEMETRY_ADMIN.md` (blob map row)
- Tests: `tests/unit/test_diagnostics.cpp`, `tests/unit/test_crash_reporter.cpp`, `tests/unit/test_debug_bundle_collector.cpp`, `tests/unit/test_telemetry_manager.cpp`

**Interfaces:** consumes `board::current_info().key` / `.display_name`. No new functions.

- [ ] **Step 1:** tests:
  - `[diagnostics]` `"diagnostics names the board"`: override `Creator5Pro`; `collect().identity.board == "creator5-pro"`.
  - `[crash_reporter]` `"crash reports carry the board"`: the report JSON has `board == "creator5-pro"` under the same override.
  - `[debug-bundle]` `"the bundle names the board as platform_model"`: override `K1` on a build whose platform key is "mips" is not reachable on the host, so assert through the extracted helper: factor the decision into `std::string platform_model_for(const helix::platform::Info* row, helix::board::Board board)` (file-local made testable via the existing test-access pattern in `tests/test_helpers/`), and check `Creator5` gives `"FlashForge Creator 5"`, `Generic` gives `""`.
  - `[telemetry]` `"Session event: app.board is the resolved board"`: override `Creator5Pro`; `event["app"]["board"] == "creator5-pro"`; `app.platform` unchanged.
  - worker (vitest): `"session events map app.board to blob11"`: an event with `app.board = "creator5-pro"` yields `blobs[10] === "creator5-pro"`; an event without it yields `""`.
- [ ] **Step 2:** `make t F='[diagnostics]'`, `F='[crash_reporter]'`, `F='[debug-bundle]'`, `F='[telemetry]'` fail; `(cd server/telemetry-worker && npm test)` fails.
- [ ] **Step 3:** implement.
- [ ] **Step 4:** all five pass. Do NOT deploy the worker; report that the deploy waits for Preston.
- [ ] **Mutation:** write `board::info(Board::Generic).key` instead of `current_info().key` in telemetry; the telemetry test goes red.
- [ ] **Step 5:** commit `"feat(telemetry): report the resolved board beside the platform key"`.

### Task 1.6: factory touch fits keyed on the board

**Files:** `include/touch_calibration.h`, `src/ui/touch_calibration.cpp#platform_default_calibration`, `tests/unit/test_touch_calibration.cpp`

**Interfaces:** produces `std::optional<TouchCalibration> helix::factory_touch_fit(board::Board board);` with the spec's two rows (Phase 1 step 1d table). `platform_default_calibration()` keeps its signature: `factory_touch_fit(board::current())` then the existing validator check. No `#if`.

- [ ] **Step 1:** `[touch]` `"factory_touch_fit ships fits for the AD5X and AD5M only"`: `factory_touch_fit(Ad5x)` and `(Ad5m)` equal the matching `kShippedFits` entries coefficient by coefficient; `Creator5`, `Creator5Pro`, `K1`, `K2`, `Cc1`, `SnapmakerU1`, `Generic` are `nullopt`. `"platform_default_calibration follows the board"`: override `Ad5x` gives a valid cal equal to the AD5X fit; override `Creator5` gives `!valid`. Replace the `#if defined(HELIX_PLATFORM_AD5M)` test with these.
- [ ] **Step 2:** `make t F='[touch]'` fails.
- [ ] **Step 3:** implement.
- [ ] **Step 4:** `make t F='[touch]'` and `F='[touch-calibration]'` pass.
- [ ] **Mutation:** return the AD5X fit for `Creator5`; red.
- [ ] **Step 5:** commit `"refactor(touch): factory fits are a per-board table"`.

### Task 1.7: panel DPI keyed on the board

**Files:** `include/display_metrics.h`, `src/application/display_metrics.cpp`, `src/application/application.cpp` (the `resolve_dpi` call), `tests/unit/test_display_metrics.cpp`

**Interfaces:** `static std::optional<PanelSpec> known_panel(board::Board board);`, `static ResolvedDpi resolve_dpi(int user_override_dpi, std::optional<double> platform_measured, board::Board board);`. Rows per the spec's panel table. `kKnownPanels` keys on `board::Board`.

- [ ] **Step 1:** `[display_metrics]` `"known_panel finds the K1 on the unified build"`: `known_panel(K1)` is `{480, 800, 108.6}`; `resolve_dpi(0, std::nullopt, K1).source == DpiSource::KnownPanel`. Port every existing `[panel]`/`[resolve]` case from string keys to boards; `"platforms driving arbitrary panels have no entry"` checks `Generic`, `Ad5x`, `Creator5`, `Creator5Pro`, `SnapmakerU1`.
- [ ] **Step 2:** `make t F='[display_metrics]'` fails to compile.
- [ ] **Step 3:** implement; `application.cpp` passes `board::current()`.
- [ ] **Step 4:** `make t F='[display_metrics]'` passes.
- [ ] **Mutation:** key the K1 row on `Ad5x`; red.
- [ ] **Step 5:** commit `"fix(display): a release K1 finds its panel's DPI"`.

### Task 1.8: board rows for camera, piezo, backlight and reduced effects

**Files:** `src/system/jz_pwm_sound_backend.cpp#available`, `src/system/sound_manager.cpp` + `Makefile` (comments only: "the FlashForge MIPS boards"), `docs/devel/SOUND_SYSTEM.md` (backend table row), `src/application/display_manager.cpp#init` (backlight block), `src/ui/filament_tube_stroker.cpp#reduced_effects`, `include/filament_tube_stroker.h`, tests `tests/unit/test_jz_pwm_sound_backend.cpp`, `tests/unit/test_board_identity.cpp`, a tube-stroker test file (`grep -l reduced_effects tests/unit`)

The camera line in `moonraker_discovery_sequence.cpp` already moved in Task 1.3; this task does not touch that file.

**Interfaces:**
- Produces: `bool helix::ui::reduced_effects_for(const helix::board::Info& board, bool constrained_memory);` (pure: `board.reduced_effects || constrained_memory`); `reduced_effects()` keeps its cached static and calls it with `board::current_info()`.
- `JzPwmSoundBackend::available()` first line: `if (!helix::board::current_info().has_jz_pwm_piezo) return false;`.
- `DisplayManager::init`: the `name() == "Allwinner"` test becomes `helix::board::current_info().firmware_dims_backlight_after_boot`. `display_manager.cpp` is a critical path (CLAUDE.md): change only that condition.

- [ ] **Step 1:**
  - `[sound]` `"the jz_pwm piezo is a FlashForge MIPS fact"`: override `K1`, `available()` is false; override `Creator5`, the board gate passes (assert via the existing test seam for the device/binary probes, or assert `false` is returned for the device-missing reason by checking the backend's debug reason if one exists; otherwise add `static bool board_admits_piezo()` and test that).
  - `[board]` `"only FlashForge firmware re-asserts brightness after boot"`: rows true for Ad5m/Ad5x/Creator5/Creator5Pro, false for Cc1, K1, K2, SnapmakerU1, Generic.
  - `[ams]` or the stroker's tag: `"reduced effects follow the board row or low memory"`: `reduced_effects_for(info(K2), false)`, `(info(K1), true)` true; `(info(K1), false)`, `(info(Generic), false)` false.
- [ ] **Step 2:** the three tags fail.
- [ ] **Step 3:** implement.
- [ ] **Step 4:** `make t F='[sound]'`, `F='[board]'`, the stroker tag pass.
- [ ] **Mutation:** gate the piezo on `local_camera_probe_safe == false` instead (true on Ad5m); the K1 case still passes but add `"the AD5M has no jz_pwm piezo"` (`info(Ad5m).has_jz_pwm_piezo == false`, `available()` false under override) and watch it go red.
- [ ] **Step 5:** commit `"refactor(platform): piezo, backlight re-assert and reduced effects read the board row"`.

### Task 1.9: one board-keyed cache rung

**Files:** `include/system/helix_cache_dir_internal.h`, `src/system/helix_cache_dir.cpp` (`#cache_path_candidates`, `#migrate_legacy_state_roots`), `include/helix_install_roots.h` (`kStateRoots` += `"/opt/config/mod_data/helixscreen-state"`), `scripts/check_platform_manifest.py` (`check_cache_rungs`), `tests/unit/test_cache_dir.cpp`, a bats file for the manifest checker (`grep -l check_platform_manifest tests/shell`, or add a case to `tests/shell/test_platform_manifest.bats` if it exists)

**Interfaces:** `std::vector<CacheCandidate> helix::cache_internal::board_cache_candidates(board::Board board, FlashForgeModTree tree, const std::string& subdir);` per the spec's cache-rung table; `tier` is `board::info(board).key`; `platform = true`. The Android arm stays a compile-time arm in `cache_path_candidates`.

- [ ] **Step 1:** `[cache]` `"board_cache_candidates names each board's rung"`: one `CHECK` per spec row, including Ad5x+Zmod `/srv/helixscreen-state/cache/x`, Ad5x+ForgeX `/opt/config/mod_data/helixscreen-state/cache/x`, Creator5Pro+Zmod same as Ad5x, SnapmakerU1 `/userdata/helixscreen-state/cache/x`, K2 two rungs in order, Generic empty. Update the two existing cases that expect tier `"MIPS"`. Bats: `"check_cache_rungs reports a manifest cache_dir the cascade lacks"` with a fixture manifest carrying an extra `cache_dir`.
- [ ] **Step 2:** `make t F='[cache]'` fails; the bats case fails.
- [ ] **Step 3:** implement; `migrate_legacy_state_roots` returns early unless `board::current() == Board::Ad5m`.
- [ ] **Step 4:** `make t F='[cache]'`, `F='[diagnostics]'` pass; `python3 scripts/check_platform_manifest.py --strict` reports no new finding; the bats case passes.
- [ ] **Mutation:** return the K1 rung for Ad5x; red.
- [ ] **Step 5:** commit `"refactor(cache): the platform cache rung is a per-board table"`.

### Task 1.10: lower the baseline, docs, phase gate

**Files:** `scripts/vendor_branch_baseline.txt`, `docs/devel/CHANGELOG_1_1_DRAFT.md`, `docs/devel/architecture/` chapter that covers platform/system services (find with `grep -l platform_info docs/devel/architecture`; add a short "Board identity" subsection), `.claude/rules/vendor-abstraction.md` (add `board_identity.h` to the "pattern working" list)

- [ ] **Step 1:** `python3 scripts/check_vendor_branches.py --list`: shape 1 is zero outside owners; shape 3 has only owner hits. `--write-baseline`.
- [ ] **Step 2:** changelog lines in the file's style (`docs/devel/CHANGELOG_STYLE.md`): the refusal, K1 DPI source, board in telemetry and crash reports.
- [ ] **Step 3:** `make mutate-diff` once for the branch; fix any surviving mutant in a task's code by strengthening that task's test.
- [ ] **Step 4:** `make full-test-run`; `scripts/test-host-run.sh asan '[board]'`; `python3 scripts/check_esp32_app_srcs.py`.
- [ ] **Step 5:** commit `"docs(platform): board identity lands; ratchet lowered"`. Report: branch HEAD, gate results, the worker deploy waiting for Preston.

---

## Phase 2: filament systems answer for themselves

Parallelism: 2.1 first (the goldens must be captured on unchanged code). Then 2.2 and 2.6 are independent (disjoint files except `app_srcs.txt`, which only 2.6 edits). 2.3 needs 2.2. 2.4 and 2.5 are a pair: 2.5 needs 2.4. 2.4 is independent of 2.2/2.3. 2.7 is last.

### Task 2.1: subscription parity goldens

**Files:** create `tests/unit/test_subscription_parity.cpp`, `tests/fixtures/subscription_parity/*.json` (one per fixture)

**Interfaces:** consumes `static nlohmann::json MoonrakerDiscoverySequence::build_subscription_objects(...)` (public, `include/moonraker_discovery_sequence.h`), the fixture style of `tests/unit/test_moonraker_subscription_fields.cpp` (`DiscoveryFixture`).

- [ ] **Step 1:** `TEST_CASE("subscription objects are unchanged for every backend fixture", "[subscription_parity]")` with one `SECTION` per printer: Happy Hare, AFC BoxTurtle, ACE with named objects, ACE with no names (fallback `ace`), CFS, Snapmaker with batch macro, Snapmaker without, QIDI Box, AD5X IFS (module firmware), AD5X IFS (native Z-Mod `zmod_ifs`/`zmod_color`), OpenAMS, klipper-toolchanger, a plain printer. Each builds discovery from an object list, calls `build_subscription_objects`, and compares the whole JSON (keys AND values, `null` vs arrays) to `tests/fixtures/subscription_parity/<name>.json`. When `HELIX_WRITE_GOLDENS=1` is set, the test writes the file instead and `WARN`s; otherwise a missing file fails.
- [ ] **Step 2:** `HELIX_WRITE_GOLDENS=1 make t F='[subscription_parity]'` writes the goldens; then `make t F='[subscription_parity]'` passes. Read each golden once to confirm it looks like that printer (HH has the 44-field `mmu` array, CFS has `box` and `motor_control`).
- [ ] **Mutation:** delete `"leds"` from the HH `mmu` field list in the builder; red.
- [ ] **Step 3:** commit `"test(discovery): pin filament subscription objects per backend"` (test and goldens only, on unchanged code).

### Task 2.2: per-backend status-object statics and the dispatcher

**Files:** `include/ams_backend.h`, `src/printer/ams_backend.cpp`, `include/ams_backend_happy_hare.h` + `.cpp`, `include/ams_backend_ace.h` + `.cpp`, `include/ams_backend_cfs.h` + `.cpp`, `include/ams_backend_snapmaker.h` + `.cpp`, `include/ams_backend_qidi.h` + `.cpp`, `include/ams_backend_afc.h` + `.cpp` (hw overload), `include/ams_backend_ad5x_ifs.h` (`kSensorObjectPrefixes`), test `tests/unit/test_ams_status_objects.cpp`

**Interfaces:**
- `[[nodiscard]] static nlohmann::json AmsBackend::required_status_objects(const helix::PrinterDiscovery& hw);`
- `[[nodiscard]] static const std::vector<std::string>& AmsBackend::filament_sensor_object_prefixes();` returns `{"zmod_ifs_switch_sensor ", "zmod_ifs_motion_sensor "}` from `AmsBackendAd5xIfs::kSensorObjectPrefixes` (an `inline static const std::array<const char*, 2>` or equivalent in the header, usable without `HELIX_HAS_IFS`).
- New `[[nodiscard]] static nlohmann::json required_status_objects(const helix::PrinterDiscovery& hw);` on `AmsBackendHappyHare`, `AmsBackendAce`, `printer::AmsBackendCfs`, `AmsBackendSnapmaker`, `AmsBackendQidi`; and on `AmsBackendAfc` as an overload beside the vector one. Each returns `{object: field-array-or-null}`, gated exactly as today (spec Phase 2 table). The dispatcher merges all of them plus IFS (names mapped to `null`) and OpenAMS, with the `HELIX_HAS_*` arms living in `ams_backend.cpp` beside `sensor_belongs_to_backend`.
- Move each hardcoded list verbatim, with its comment, from `build_subscription_objects` into the backend's static. Do not change `build_subscription_objects` in this task.

- [ ] **Step 1:** `[ams]` `"each backend's required_status_objects matches its own printer and is empty off it"`: for each new static, the backend's fixture discovery gives the expected object set (HH `mmu` with 44 fields; CFS `box`, `motor_control`; QIDI `box_extras`, `save_variables`; ACE names or `ace`; Snapmaker list incl. `gcode_macro <batch>` only when the macro exists); a plain printer gives `{}` from every static. `"the dispatcher merges every backend"`: for the HH fixture, `AmsBackend::required_status_objects(hw)` equals the `mmu`/AFC/OpenAMS portion of the Task 2.1 HH golden.
- [ ] **Step 2:** `make t F='[ams]'` fails to compile.
- [ ] **Step 3:** implement.
- [ ] **Step 4:** `make t F='[ams]'` and `F='[subscription_parity]'` pass (builder untouched, so parity is trivially green).
- [ ] **Mutation:** drop the `mmu_type() == QIDI_BOX` gate in the QIDI static; the plain-printer assertion goes red.
- [ ] **Step 5:** commit `"refactor(ams): each backend names the status objects it needs"`.

### Task 2.3: the subscription builder asks the backends

**Files:** `src/api/moonraker_discovery_sequence.cpp` **(shared file, see overlap table)**: `#build_subscription_objects`, the subscription log block, the object-bucketing branch for filament sensors; `tests/unit/test_subscription_parity.cpp` (add the prefix case)

**Interfaces:** consumes Task 2.2's two statics. The six hardcoded blocks, the `#if HELIX_HAS_IFS` block and the AFC/OpenAMS loops become one merge of `AmsBackend::required_status_objects(hw)`. The per-type `spdlog::info` lines become one line per detected system naming `ams_type_to_string()` and the objects added. Bucketing: `filament_switch_sensor `/`filament_motion_sensor ` stay literal (Klipper stock); the extra prefixes come from `AmsBackend::filament_sensor_object_prefixes()`.

- [ ] **Step 1:** `[subscription_parity]` `"backend sensor prefixes bucket as filament sensors"`: a discovery object list containing `zmod_ifs_switch_sensor head` yields that name in the builder's filament-sensor subscriptions. (This passes on today's code; it pins the behaviour across the refactor.)
- [ ] **Step 2:** run it; it passes. Now apply the refactor's first edit (delete the hardcoded blocks without adding the dispatcher call) and watch `[subscription_parity]` go red: that is this task's failing state.
- [ ] **Step 3:** add the dispatcher call and the prefix lookup.
- [ ] **Step 4:** `make t F='[subscription_parity]'`, `F='[moonraker]'` pass with the goldens untouched.
- [ ] **Mutation:** skip the QIDI static in the dispatcher; the QIDI golden goes red.
- [ ] **Step 5:** commit `"refactor(discovery): the subscription builder asks each filament backend"`.

### Task 2.4: runout ownership and idle unload become traits

**Files:** `include/ams_backend.h` (`BackendTraits`, three fields + predicates `raises_runout_fault()`, `raises_runout_fault_during_job()`, `unloads_to_hub_between_prints()` beside the existing ones), each backend header's `kTraits` (`ams_backend_happy_hare.h`, `ams_backend_afc.h`, `ams_backend_cfs.h`, `ams_backend_ad5x_ifs.h`), test `tests/unit/test_ams_backend_traits.cpp`

**Interfaces:** `bool raises_runout_fault = false; bool raises_runout_fault_during_job = false; bool unloads_to_hub_between_prints = false;` in `BackendTraits` (it is all-bool with a `has_unique_object_representations` check; keep it all-bool). Values exactly per the spec's Phase 2 traits table. Move each switch-case comment from `runtime_config.cpp` onto the `kTraits` line that sets the value.

- [ ] **Step 1:** `[ams]` `"runout_fault_traits_match_the_error_hooks"`: for HH, AFC, AD5X IFS, CFS, ACE, QIDI, OpenAMS, toolchanger, Snapmaker, drive the backend's existing runout fixture (the one its `classify_error()`/`current_error()` tests use) and assert `traits().raises_runout_fault == (classify_error(...) or current_error() produced an error)`. `"runout ownership traits match the spec table"`: one `CHECK` per backend per field.
- [ ] **Step 2:** `make t F='[ams]'` fails to compile.
- [ ] **Step 3:** implement. `runtime_config.cpp` is untouched in this task.
- [ ] **Step 4:** `make t F='[ams]'` passes.
- [ ] **Mutation:** set `raises_runout_fault = true` on ACE; the hook test goes red.
- [ ] **Step 5:** commit `"refactor(ams): runout ownership is a backend trait"`.

### Task 2.5: callers read the traits

**Files:** `include/ams_backend_registry.h` + `src/printer/ams_backend_registry.cpp`, `include/ams_state.h` + `src/printer/ams_state.cpp`, `src/system/runtime_config.cpp`, `include/runtime_config.h`, `src/print/filament_sensor_manager.cpp`, tests `tests/unit/test_runtime_config.cpp`, `tests/unit/test_job_holds_machine_guards.cpp`, the filament-sensor test file that covers the idle head-empty toast (`grep -ln ad5x_idle_unload\|idle.*unload tests/unit`)

**Interfaces:** `[[nodiscard]] std::optional<BackendTraits> AmsBackendRegistry::primary_traits() const;` and the same on `AmsState`, copying backend 0's `traits()` under the registry mutex (same locking as `primary_type()`). `backend_owns_runout_surface` and `backend_owns_runout_during_job` are deleted; `RuntimeConfig::should_show_runout_modal()` reads `backend->raises_runout_fault()`; `filament_sensor_manager.cpp` reads `primary_traits()` once beside `primary_type()` and renames `ad5x_idle_unload` to `idle_unload_expected`.

- [ ] **Step 1:** `[runtime_config]` `"runout modal ownership follows the backend trait"`: with a mock backend whose traits set `raises_runout_fault`, bypass off, the modal is suppressed; without it, shown. `[ams]` `"primary_traits is empty with no backend"`. Port every `backend_owns_runout_during_job(AmsType::X)` assertion in `test_job_holds_machine_guards.cpp` to the backend's traits. Sensor test: `"idle unload is expected only where the backend unloads to its hub"`: IFS-trait backend, idle, head sensor empties, no toast; ACE-trait backend, same edge, toast.
- [ ] **Step 2:** the tags fail (functions removed / accessor missing).
- [ ] **Step 3:** implement.
- [ ] **Step 4:** `make t F='[runtime_config]'`, `F='[ams]'`, `F='[job-holds-machine]'`, the sensor test's tag pass; then `scripts/test-host-run.sh asan '[ams]'` is deferred to Task 2.7.
- [ ] **Mutation:** return `std::nullopt` from `primary_traits()` always; the idle-unload test goes red.
- [ ] **Step 5:** commit `"refactor(runout): callers ask the backend's traits, not its type"`.

### Task 2.6: fixed toolheads leave ToolState

**Files:** create `include/fixed_tool_topology.h`, `src/printer/fixed_tool_topology.cpp`, `tests/unit/test_fixed_tool_topology.cpp`; modify `src/printer/tool_state.cpp#init_tools`, `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` **(shared file)**, `docs/devel/TOOL_ABSTRACTION.md` (one paragraph)

**Interfaces:** spec Phase 2 `helix::tool_topology::FixedTool`, `fixed_toolheads(const PrinterDiscovery&)`, `provider_name(const PrinterDiscovery&)`; one provider row keyed on `hw.has_snapmaker()` with the spec's four extruder/heater/fan triples. `ToolState::init_tools` builds `ToolInfo` from the list when non-empty and logs `provider_name`.

- [ ] **Step 1:** `[tool]` `"fixed_toolheads gives the U1 four heads and everyone else none"`: U1 discovery gives the four rows (check `fan_generic e3_fan` on index 3); toolchanger and plain discoveries give empty. Existing ToolState U1 cases (`grep -n has_snapmaker\|Snapmaker tests/unit/test_tool_state*.cpp`) stay as they are and must still pass.
- [ ] **Step 2:** `make t F='[tool]'` fails to compile.
- [ ] **Step 3:** implement.
- [ ] **Step 4:** `make t F='[tool]'` passes; `scripts/check_esp32_app_srcs.py` passes.
- [ ] **Mutation:** swap the fans of index 1 and 2 in the provider row; red.
- [ ] **Step 5:** commit `"refactor(tools): firmware-fixed toolheads are a provider table"`.

### Task 2.7: docs and phase gate

**Files:** `docs/devel/FILAMENT_MANAGEMENT.md` (add-a-backend guide: "declare `required_status_objects(hw)` and the runout traits; do not edit the subscription builder"), the backend leaf docs whose subscription sections name the builder (`grep -l build_subscription_objects docs/devel/FILAMENT_BACKEND_*.md`)

- [ ] **Step 1:** docs.
- [ ] **Step 2:** `python3 scripts/check_vendor_branches.py` (if Phase 1 has landed on main and been merged in: it passes with lower counts; do not write the baseline unless this phase lands second, see overlap table).
- [ ] **Step 3:** `make mutate-diff`; `scripts/test-host-run.sh asan '[ams]'`; `make full-test-run`.
- [ ] **Step 4:** commit `"docs(ams): backends own their subscriptions and runout traits"`. Report HEAD and gate results.

---

## Phase 3: capability queries and leftovers

Branches from `main` after Phases 1 and 2 land. Tasks 3.1, 3.2, 3.3, 3.4 and 3.5 touch disjoint files except that 3.2 and 3.3 both edit XML under `ui_xml/` (different files) and 3.3 and 3.4 both edit `include/ams_backend.h` / `include/ams_types.h` (different declarations): run 3.3 before 3.4. 3.6 is last.

### Task 3.1: one helper for the two AD5X sleep migrations

**Files:** `src/system/config_migrations.cpp` (`#migrate_v12_to_v13`, `#migrate_v14_to_v15`), create `tests/unit/test_config_migration_v13_v15.cpp`

**Interfaces:** file-local `bool config_names_printer(const nlohmann::json& config, const char* preset, const char* printer_type);` and `void restore_backlight_off_sleep(nlohmann::json& config, const char* log_tag);`. The `"ad5x"` / `"FlashForge Adventurer 5X"` literals appear once, at the shared call, under `// VENDOR_OK: a migration freezes the printers it repairs`.

- [ ] **Step 1:** `[config][migration]` `"both AD5X sleep migrations repair the same stale pair"`: for each of v13 and v15 (call through the migration table the other `test_config_migration_v*.cpp` files use): preset `ad5x` with `sleep_backlight_off=false, hardware_blank=0` becomes `true, 1`; a config whose `printers.*.type` is `FlashForge Adventurer 5X` likewise; preset `ad5m` untouched; an AD5X config already at `true` untouched.
- [ ] **Step 2:** run `make t F='[migration]'`; it passes on today's code (it pins behaviour). Then delete the body of `migrate_v14_to_v15` and watch it go red; restore.
- [ ] **Step 3:** fold both bodies into the helpers.
- [ ] **Step 4:** `make t F='[migration]'` passes.
- [ ] **Mutation:** in `config_names_printer`, compare the preset only; the printer-type case goes red.
- [ ] **Step 5:** commit `"refactor(config): the two AD5X sleep migrations share one repair"`.

### Task 3.2: eject-params subject says what it means

**Files:** `src/ui/ui_ams_device_operations_overlay.cpp`, its header, `ui_xml/ams_device_operations.xml`

**Interfaces:** subject `ams_device_ops_is_qidi` becomes `ams_device_ops_has_eject_params`; the member becomes `has_eject_params_subject_`. Settings keys `qidi_eject_*` unchanged.

- [ ] **Step 1:** `[ams]` (in `test_ams_device_ops_subject_scope.cpp` or a sibling) `"eject params rows follow the backend capability"`: a mock backend with `supports_configurable_eject_params()` true sets `ams_device_ops_has_eject_params` to 1, false sets 0; `lv_xml_get_subject(nullptr, "ams_device_ops_is_qidi")` is null.
- [ ] **Step 2:** fails.
- [ ] **Step 3:** rename in C++ and XML.
- [ ] **Step 4:** the tag passes; `python3 scripts/check_esp32_xml_bindings.py` and the orphan-subject gate pass.
- [ ] **Mutation:** leave the XML binding on the old name; the binding gate fails.
- [ ] **Step 5:** commit `"refactor(ams-ui): the eject-params subject names the capability"`.

### Task 3.3: backends explain an unavailable eject

**Files:** `include/ams_backend.h`, `include/ams_backend_qidi.h` + `src/printer/ams_backend_qidi.cpp`, `src/ui/ui_ams_context_menu.cpp` + header, the context-menu XML that holds `eject_force_move_hint`, tests `tests/unit/test_ams_backend_qidi.cpp`, `tests/unit/test_ams_context_menu.cpp`

**Interfaces:** `[[nodiscard]] virtual std::string AmsBackend::eject_unavailable_reason(int slot_index) const { return {}; }`; QIDI returns the translated force_move sentence when eject is unavailable and filament is present. Context menu: `slot_eject_hint` string subject and `slot_eject_hint_visible` int subject, set beside `slot_unload_hint`; the XML label binds to them; the `get_type() == QIDI_BOX` check is gone.

- [ ] **Step 1:** `[ams]` `"QIDI explains an unavailable eject"`: force_move off and filament present gives a non-empty reason; force_move on gives empty; base `AmsBackend` gives empty. `"the context menu shows a backend's eject reason"`: with a mock backend returning `"x"`, the menu's `slot_eject_hint` subject is `"x"` and visible is 1.
- [ ] **Step 2:** fails to compile.
- [ ] **Step 3:** implement.
- [ ] **Step 4:** `make t F='[ams]'` passes.
- [ ] **Mutation:** return `{}` from QIDI's override; red.
- [ ] **Step 5:** commit `"refactor(ams-ui): the backend explains why eject is unavailable"`.

### Task 3.4: guided device flows

**Files:** create `include/ui_guided_device_flow.h`, `src/ui/ui_guided_device_flow.cpp`, `src/ui/ui_cfs_device_flows.cpp` (under `HELIX_HAS_CFS`); modify `include/ams_types.h` (`DeviceAction::guided_flow`), `src/printer/ams_backend_cfs.cpp` (set it on the two actions), `src/ui/ui_ams_device_section_detail_overlay.cpp` (drop the CFS statics, `confirm_cutter_calibration`, both `get_type() == CFS` checks, the `cfs_cutter_status` creation; route by `guided_flow`), the registration call site in `Application::register_xml_components()`'s order (before any overlay is built), `firmware/.../app_srcs.txt` for the two new files if the overlay is in the ESP32 cut (it is), tests `tests/unit/test_ams_backend_cfs.cpp`, a new `tests/unit/test_guided_device_flow.cpp`

**Interfaces:** spec Phase 3 `helix::ui::GuidedDeviceFlow`, `register_guided_device_flow`, `find_guided_device_flow`; `void register_cfs_device_flows();` called once at startup. Flow ids `cfs_purge_chute` and `cfs_cutter`.

- [ ] **Step 1:** `[ams]` `"CFS actions name their guided flows"`: the backend's action list has `calibrate_purge_chute.guided_flow == "cfs_purge_chute"` and `calibrate_cutter.guided_flow == "cfs_cutter"`. `[ui]` `"the section overlay starts a registered flow and executes the rest"`: register a test flow under `"t"`, an action with `guided_flow = "t"` calls its `start` and never `execute_device_action`; an action with no flow calls `execute_device_action`. `"an unregistered guided flow never executes its action"`: an action naming an unregistered flow logs a warning and does nothing (Review Focus 5).
- [ ] **Step 2:** fails to compile.
- [ ] **Step 3:** implement; move code verbatim from the overlay into `ui_cfs_device_flows.cpp`.
- [ ] **Step 4:** `make t F='[ams]'`, `F='[ui]'` pass.
- [ ] **Mutation:** in the overlay, fall back to `execute_device_action` when a named flow is missing; the unregistered-flow test goes red.
- [ ] **Step 5:** commit `"refactor(ams-ui): device actions route to registered guided flows"`.

### Task 3.5: small leftovers

**Files:** `include/ui_print_start_controller.h` + `src/ui/ui_print_start_controller.cpp` (rename to `send_backend_preprint_then`, log text "pre-print config"), `include/filament_slot_override_store.h` + `src/printer/filament_slot_override_store.cpp` (ctor flag `bool migrates_legacy_namespace = false`, delete the `"ace"`/`"cfs"` test), the ACE and CFS backends' store construction, `include/ui_spool_canvas.h` (drop the duplicate `reduced_effects()` declaration, include `filament_tube_stroker.h`), `src/ui/theme_lvgl_apply.cpp` (call `helix::ui::full_style_effects_active()`), tests `tests/unit/test_filament_slot_override_store.cpp`

- [ ] **Step 1:** `[filament_slot_override]` `"only stores built to migrate read the legacy namespace"`: a store built with `false` and backend id `"ace"` never issues the legacy-namespace read (assert on the mock API's recorded calls); built with `true` it does.
- [ ] **Step 2:** fails to compile.
- [ ] **Step 3:** implement all five edits.
- [ ] **Step 4:** `make t F='[filament_slot_override]'` passes; `make t F='[print_start]'` (or the controller's existing tag) passes.
- [ ] **Mutation:** ignore the flag and keep the id test; red.
- [ ] **Step 5:** commit `"refactor: name capabilities, not vendors, in the last generic call sites"`.

### Task 3.6: lower the baseline, retire the plan, phase gate

- [ ] **Step 1:** `python3 scripts/check_vendor_branches.py --write-baseline`; shape 2 is zero in `runtime_config.cpp`, `moonraker_discovery_sequence.cpp`, `filament_sensor_manager.cpp`, `ui_ams_device_section_detail_overlay.cpp`, `ui_ams_context_menu.cpp`.
- [ ] **Step 2:** durable knowledge lands where it belongs (board identity in the architecture chapter from Task 1.10, backends in `FILAMENT_MANAGEMENT.md` from Task 2.7, guided flows in `FILAMENT_MANAGEMENT.md` § device ops). Delete `docs/devel/plans/2026-10-14-vendor-abstraction-design.md` and this file.
- [ ] **Step 3:** `make mutate-diff`; `make full-test-run`.
- [ ] **Step 4:** commit `"docs: vendor abstraction shipped; plan retired"`.

---

## Self-review notes

- Spec coverage: every Phase 1 step (1a gate: 1.1; 1b module: 1.2, 1.3, 1.5, 1.8; 1c updater: 1.4; 1d: 1.6, 1.7; 1e: 1.9), Phase 2 (2a: 2.2, 2.3; 2b: 2.4, 2.5, 2.6), Phase 3 (each "Sites and changes" item: 3.1 to 3.5) and the telemetry addition Preston approved (1.5) has a task.
- Signatures used across tasks: `board::Board`, `board::current()`, `board::info()`, `board::current_info()`, `board::set_current_override()`, `FlashForgeModTree`, `flashforge_mod_tree()`, `release_asset_keys(key, Board)`, `AmsBackend::required_status_objects(hw)`, `AmsBackend::filament_sensor_object_prefixes()`, `primary_traits()`, `tool_topology::fixed_toolheads()`, `eject_unavailable_reason()`, `GuidedDeviceFlow`, all as defined in their producing task and in the spec, whose sketch compiled `-fsyntax-only`.
