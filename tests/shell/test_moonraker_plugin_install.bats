#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Guards for moonraker-plugin/install.sh and helix_print.py: the installer
# and plugin stay phase-tracking-free, and every uninstall path
# (auto_uninstall and the interactive uninstall) strips any phase-tracking
# instrumentation a pre-1.1 HelixScreen wrote into PRINT_START, leaving the
# shared helix_macros.cfg and everything else in the config tree untouched.
# The marker-matching and safe-write logic itself lives in
# strip_phase_tracking.py and is pinned by moonraker-plugin/tests/
# test_strip_phase_tracking.py; these tests cover the shell-side wiring.

load helpers

REPO_ROOT="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)"

# Overridable so each guard can be proven to FIRE against a hand-broken copy
# of the script instead of the shipped one.
SCRIPT="${HELIX_PLUGIN_INSTALL_SH:-$REPO_ROOT/moonraker-plugin/install.sh}"
PLUGIN_PY="${HELIX_PLUGIN_PRINT_PY:-$REPO_ROOT/moonraker-plugin/helix_print.py}"
STRIP_PY="${HELIX_STRIP_PY:-$REPO_ROOT/moonraker-plugin/strip_phase_tracking.py}"

# A real, four-space-indent instrumented PRINT_START, shaped exactly like a
# pre-1.1 HelixScreen's own writer produced it (marker pairs after every
# matched phase line, plus the trailing HELIX_READY block) - not a shape a
# fixture only vaguely resembling one.
write_instrumented_printer_cfg() {
    cat > "$1" <<'EOF'
[include helix_macros.cfg]

[printer]
kinematics: corexy

[gcode_macro PRINT_START]
description: Start print
gcode:
    {% set BED_TEMP = params.BED_TEMP|default(60)|float %}
    {% set EXTRUDER_TEMP = params.EXTRUDER_TEMP|default(200)|float %}
    M190 S{BED_TEMP}
    # <<< HELIX_TRACKING v2 >>>
    HELIX_PHASE_HEATING_BED
    # <<< /HELIX_TRACKING >>>
    G28
    # <<< HELIX_TRACKING v2 >>>
    HELIX_PHASE_HOMING
    # <<< /HELIX_TRACKING >>>

    QUAD_GANTRY_LEVEL
    # <<< HELIX_TRACKING v2 >>>
    HELIX_PHASE_QGL
    # <<< /HELIX_TRACKING >>>
    BED_MESH_CALIBRATE ADAPTIVE=1
    # <<< HELIX_TRACKING v2 >>>
    HELIX_PHASE_BED_MESH
    # <<< /HELIX_TRACKING >>>
    M109 S{EXTRUDER_TEMP}
    # <<< HELIX_TRACKING v2 >>>
    HELIX_PHASE_HEATING_NOZZLE
    # <<< /HELIX_TRACKING >>>
    CLEAN_NOZZLE
    # <<< HELIX_TRACKING v2 >>>
    HELIX_PHASE_CLEANING
    # <<< /HELIX_TRACKING >>>
    PURGE_LINE   ; prime
    # <<< HELIX_TRACKING v2 >>>
    HELIX_PHASE_PURGING
    # <<< /HELIX_TRACKING >>>

    # <<< HELIX_TRACKING v2 >>>
    HELIX_READY
    # <<< /HELIX_TRACKING >>>
[extruder]
step_pin: PA1

[heater_bed]
heater_pin: PB1
EOF
}

# The same macro, before instrumentation - what a correct strip must produce.
write_original_printer_cfg() {
    cat > "$1" <<'EOF'
[include helix_macros.cfg]

[printer]
kinematics: corexy

[gcode_macro PRINT_START]
description: Start print
gcode:
    {% set BED_TEMP = params.BED_TEMP|default(60)|float %}
    {% set EXTRUDER_TEMP = params.EXTRUDER_TEMP|default(200)|float %}
    M190 S{BED_TEMP}
    G28

    QUAD_GANTRY_LEVEL
    BED_MESH_CALIBRATE ADAPTIVE=1
    M109 S{EXTRUDER_TEMP}
    CLEAN_NOZZLE
    PURGE_LINE   ; prime

[extruder]
step_pin: PA1

[heater_bed]
heater_pin: PB1
EOF
}

# curl proper (its --version starts "curl <digit>"), exiting $1 for every other
# call and logging its argv to $2 when given.
mock_real_curl() {
    local status="$1" log="${2:-}"
    local body='case "$1" in --version) echo "curl 8.5.0 (x86_64-pc-linux-gnu)"; exit 0;; esac'
    if [ -n "$log" ]; then
        body="$body
printf '%s\\n' \"\$*\" >> '$log'"
    fi
    mock_command_script "curl" "$body
exit $status"
}

# Build a scratch HOME shaped like a real printer's: a Moonraker checkout
# with the plugin symlinked in, and printer_data/config holding an
# instrumented printer.cfg plus the real helix_macros.cfg. Shims sudo,
# service and curl so auto_uninstall's restart/wait steps are no-ops;
# systemctl is shadowed inert by `load helpers` already.
setup_instrumented_home() {
    local home="$1"
    mkdir -p "$home/moonraker/moonraker/components" "$home/printer_data/config" "$home/plugin"
    : > "$home/plugin/helix_print.py"
    ln -s "$home/plugin/helix_print.py" "$home/moonraker/moonraker/components/helix_print.py"

    local c="$home/printer_data/config"
    cp "$REPO_ROOT/assets/config/helix_macros.cfg" "$c/helix_macros.cfg"
    printf '[server]\nhost: 0.0.0.0\n\n[helix_print]\n# HelixScreen plugin - auto-configured\n\n[octoprint_compat]\n' \
        > "$c/moonraker.conf"
    write_instrumented_printer_cfg "$c/printer.cfg"

    for cmd in sudo service; do
        mock_command_script "$cmd" 'exit 0'
    done
    mock_real_curl 0
    mock_command_script "wget" 'exit 8'
}

@test "install.sh passes POSIX syntax check" {
    [ -f "$SCRIPT" ]
    run sh -n "$SCRIPT"
    [ "$status" -eq 0 ]
}

@test "strip_phase_tracking.py passes a Python syntax check" {
    [ -f "$STRIP_PY" ]
    run python3 -m py_compile "$STRIP_PY"
    [ "$status" -eq 0 ]
}

@test "install.sh invokes strip_phase_tracking.py from SCRIPT_DIR on both uninstall paths" {
    # Confirms the wiring exists at all before the e2e tests below exercise
    # it by behaviour; this alone would not catch a commented-out call.
    grep -q 'strip_phase_tracking\.py' "$SCRIPT" || fail "install.sh never references strip_phase_tracking.py"
}

@test "uninstall preserves the shared helper macros (static)" {
    # helix_macros.cfg carries HELIX_START_PRINT / HELIX_CLEAN_NOZZLE and
    # other helpers that keep working without the plugin; deleting it on
    # uninstall would break features the user never uninstalled. Behavioural
    # proof (byte-identical after a real run) is in the e2e tests below.
    local auto_region="$BATS_TEST_TMPDIR/auto_uninstall.sh"
    local interactive_region="$BATS_TEST_TMPDIR/uninstall.sh"
    sed -n '/^auto_uninstall()/,/^}/p' "$SCRIPT" > "$auto_region"
    sed -n '/^uninstall()/,/^}/p' "$SCRIPT" > "$interactive_region"
    [ -s "$auto_region" ] || fail "auto_uninstall not found in $SCRIPT"
    [ -s "$interactive_region" ] || fail "uninstall not found in $SCRIPT"
    refute_grep 'rm.*helix_macros' "$auto_region"
    refute_grep 'rm.*helix_macros' "$interactive_region"
}

@test "installer carries no phase-tracking flag" {
    [ -f "$SCRIPT" ]
    refute_grep 'with-phase-tracking' "$SCRIPT"
    refute_grep 'ENABLE_PHASE_TRACKING' "$SCRIPT"
}

@test "plugin exposes no phase-tracking endpoints" {
    [ -f "$PLUGIN_PY" ]
    refute_grep 'phase_tracking' "$PLUGIN_PY"
}

# HelixScreen runs install.sh from a NoNewPrivileges service, where sudo can
# never work; it passes --no-restart and restarts Moonraker over its API.
@test "--no-restart leaves Moonraker alone on both auto paths" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_instrumented_home "$home"
    local log="$BATS_TEST_TMPDIR/sudo.log"
    mock_command_script "sudo" "echo \"\$*\" >> '$log'"
    mock_command_script "service" "echo \"service \$*\" >> '$log'"

    run env HOME="$home" sh "$SCRIPT" --no-restart --uninstall-auto
    [ "$status" -eq 0 ]
    [ ! -e "$log" ] || fail "uninstall restarted Moonraker: $(cat "$log")"

    run env HOME="$home" sh "$SCRIPT" --no-restart --auto
    [ "$status" -eq 0 ]
    [ ! -e "$log" ] || fail "install restarted Moonraker: $(cat "$log")"
    grep -q '^\[helix_print\]' "$home/printer_data/config/moonraker.conf"
}

@test "without --no-restart the auto install still restarts Moonraker" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_instrumented_home "$home"
    local log="$BATS_TEST_TMPDIR/sudo.log"
    mock_command_script "sudo" "echo \"\$*\" >> '$log'"
    mock_command_script "service" "echo \"service \$*\" >> '$log'"

    run env HOME="$home" sh "$SCRIPT" --auto
    [ "$status" -eq 0 ]
    grep -q 'restart' "$log" || fail "no restart attempted"
}

@test "auto_uninstall strips PRINT_START, preserves helix_macros.cfg, and removes the plugin" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_instrumented_home "$home"
    local expected="$BATS_TEST_TMPDIR/expected_printer.cfg"
    write_original_printer_cfg "$expected"

    run env HOME="$home" sh "$SCRIPT" --uninstall-auto
    [ "$status" -eq 0 ]

    local c="$home/printer_data/config"
    diff "$expected" "$c/printer.cfg"
    cmp -s "$REPO_ROOT/assets/config/helix_macros.cfg" "$c/helix_macros.cfg" \
        || fail "helix_macros.cfg was modified by uninstall"
    [ ! -e "$home/moonraker/moonraker/components/helix_print.py" ]
    refute_grep '^\[helix_print\]' "$c/moonraker.conf"
}

@test "interactive uninstall strips PRINT_START and preserves helix_macros.cfg" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_instrumented_home "$home"
    local expected="$BATS_TEST_TMPDIR/expected_printer.cfg"
    write_original_printer_cfg "$expected"

    run env HOME="$home" sh "$SCRIPT" --uninstall
    [ "$status" -eq 0 ]

    local c="$home/printer_data/config"
    diff "$expected" "$c/printer.cfg"
    cmp -s "$REPO_ROOT/assets/config/helix_macros.cfg" "$c/helix_macros.cfg" \
        || fail "helix_macros.cfg was modified by uninstall"
    [ ! -e "$home/moonraker/moonraker/components/helix_print.py" ]
    # The interactive path only prints a reminder; it does not touch
    # moonraker.conf.
    grep -q '^\[helix_print\]' "$c/moonraker.conf"
}

@test "auto_uninstall warns and continues when python3 is unavailable" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_instrumented_home "$home"
    local before="$BATS_TEST_TMPDIR/before_printer.cfg"
    cp "$home/printer_data/config/printer.cfg" "$before"

    # A PATH with no python3 on it at all - every external command install.sh
    # itself calls, minus python3, plus the sudo/service/curl mocks above and
    # helpers' systemctl shim. The sandbox shim directory follows it so the
    # host sandbox's PATH layer still covers this callee, which is sh and so
    # carries none of its exported-function layer; the mocks come first and win.
    local no_py3="$BATS_TEST_TMPDIR/no-python3-path"
    mkdir -p "$no_py3"
    for tool in sh dirname cp mv rm ln grep awk date sleep; do
        p="$(command -v "$tool")" && ln -sf "$p" "$no_py3/$tool"
    done
    for cmd in sudo service curl systemctl; do
        ln -sf "$BATS_TEST_TMPDIR/bin/$cmd" "$no_py3/$cmd"
    done

    run env HOME="$home" PATH="$no_py3:$HELIX_TEST_SANDBOX_BIN" sh "$SCRIPT" --uninstall-auto
    # PRINT_START could not be checked at all, so this is "needs attention"
    # (2), not full success - the plugin itself is still gone.
    [ "$status" -eq 2 ]
    contains "python3 not found" "$output"

    # Nothing about PRINT_START was touched; only the symlink and
    # moonraker.conf section, which do not need python3, were removed.
    cmp -s "$before" "$home/printer_data/config/printer.cfg" \
        || fail "printer.cfg changed with no python3 available"
}

@test "auto_uninstall reports needs-attention when PRINT_START has an anomaly" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_instrumented_home "$home"
    local c="$home/printer_data/config"
    # An unmatched BEGIN marker: strip_phase_tracking.py skips the file
    # rather than failing, but it is not clean either.
    printf '[gcode_macro PRINT_START]\ngcode:\n    G28\n    # <<< HELIX_TRACKING v2 >>>\n    HELIX_PHASE_HOMING\n' \
        > "$c/printer.cfg"
    local before="$BATS_TEST_TMPDIR/before_printer.cfg"
    cp "$c/printer.cfg" "$before"

    run env HOME="$home" sh "$SCRIPT" --uninstall-auto
    [ "$status" -eq 2 ]
    contains "next step" "$output"

    [ ! -e "$home/moonraker/moonraker/components/helix_print.py" ]
    cmp -s "$before" "$c/printer.cfg" || fail "the anomalous file was modified"
}

@test "interactive uninstall reports needs-attention when PRINT_START has an anomaly" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_instrumented_home "$home"
    local c="$home/printer_data/config"
    printf '[gcode_macro PRINT_START]\ngcode:\n    G28\n    # <<< HELIX_TRACKING v2 >>>\n    HELIX_PHASE_HOMING\n' \
        > "$c/printer.cfg"

    run env HOME="$home" sh "$SCRIPT" --uninstall
    [ "$status" -eq 2 ]
    contains "next step" "$output"
    [ ! -e "$home/moonraker/moonraker/components/helix_print.py" ]
}

@test "auto_uninstall reports needs-attention when the strip cannot write a file" {
    [ "$(id -u)" -ne 0 ] || skip "running as root ignores the read-only mode"
    local home="$BATS_TEST_TMPDIR/home"
    setup_instrumented_home "$home"
    local c="$home/printer_data/config"
    # A second instrumented macro in a directory the strip cannot write to.
    # Chmod is scoped to this subdirectory, not the whole config tree, so the
    # top-level printer.cfg and moonraker.conf stay editable - only this
    # file's backup step fails.
    mkdir -p "$c/macros"
    write_instrumented_printer_cfg "$c/macros/print_start.cfg"
    chmod 0555 "$c/macros"

    run env HOME="$home" sh "$SCRIPT" --uninstall-auto
    chmod 0755 "$c/macros"  # restore so bats can clean up the tmpdir

    [ "$status" -eq 2 ]
    contains "failed to strip" "$output"
    [ ! -e "$home/moonraker/moonraker/components/helix_print.py" ]
}

@test "interactive uninstall reports needs-attention when the strip cannot write a file" {
    [ "$(id -u)" -ne 0 ] || skip "running as root ignores the read-only mode"
    local home="$BATS_TEST_TMPDIR/home"
    setup_instrumented_home "$home"
    local c="$home/printer_data/config"
    mkdir -p "$c/macros"
    write_instrumented_printer_cfg "$c/macros/print_start.cfg"
    chmod 0555 "$c/macros"

    run env HOME="$home" sh "$SCRIPT" --uninstall
    chmod 0755 "$c/macros"

    [ "$status" -eq 2 ]
    contains "failed to strip" "$output"
    [ ! -e "$home/moonraker/moonraker/components/helix_print.py" ]
}

@test "auto_uninstall reports a clean exit even when moonraker never comes back" {
    # curl always failing means wait_for_moonraker's own 30-attempt loop runs
    # to completion; sleep is stubbed to a no-op so that costs no real time.
    # A slow or unreachable Moonraker restart must not turn a completed,
    # clean uninstall into a reported failure.
    local home="$BATS_TEST_TMPDIR/home"
    setup_instrumented_home "$home"
    mock_real_curl 1
    mock_command_script "sleep" 'exit 0'
    local expected="$BATS_TEST_TMPDIR/expected_printer.cfg"
    write_original_printer_cfg "$expected"

    run env HOME="$home" sh "$SCRIPT" --uninstall-auto

    [ "$status" -eq 0 ]
    contains "did not come back up in time" "$output"
    contains "plugin removal itself completed" "$output"
    local c="$home/printer_data/config"
    diff "$expected" "$c/printer.cfg"
    [ ! -e "$home/moonraker/moonraker/components/helix_print.py" ]
}

@test "auto_uninstall's needs-attention outcome survives an unreachable moonraker" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_instrumented_home "$home"
    local c="$home/printer_data/config"
    printf '[gcode_macro PRINT_START]\ngcode:\n    G28\n    # <<< HELIX_TRACKING v2 >>>\n    HELIX_PHASE_HOMING\n' \
        > "$c/printer.cfg"
    mock_real_curl 1
    mock_command_script "sleep" 'exit 0'

    run env HOME="$home" sh "$SCRIPT" --uninstall-auto

    [ "$status" -eq 2 ]
    contains "next step" "$output"
    contains "did not come back up in time" "$output"
    [ ! -e "$home/moonraker/moonraker/components/helix_print.py" ]
}

@test "auto_uninstall reports needs-attention when moonraker.conf cannot be backed up" {
    [ "$(id -u)" -ne 0 ] || skip "running as root ignores the read-only mode"
    local home="$BATS_TEST_TMPDIR/home"
    setup_instrumented_home "$home"
    local c="$home/printer_data/config"
    # A clean (non-instrumented) printer.cfg so the phase-tracking strip has
    # nothing to write and exits 0 on its own - isolating this test to the
    # moonraker.conf backup/edit step.
    write_original_printer_cfg "$c/printer.cfg"
    chmod 0555 "$c"

    run env HOME="$home" sh "$SCRIPT" --uninstall-auto
    chmod 0755 "$c"  # restore so bats can clean up the tmpdir

    [ "$status" -eq 2 ]
    contains "Could not back up moonraker.conf" "$output"
    [ ! -e "$home/moonraker/moonraker/components/helix_print.py" ]
}

# A Creality K1-series box: HOME is /root with no Moonraker in it, the Moonraker
# repo is nested inside /usr/data/moonraker, and printer_data is under
# /usr/data. Only the running process says where any of it is.
setup_creality_k1_layout() {
    local root="$1"
    local pkg="$root/usr/data/moonraker/moonraker/moonraker"
    local data="$root/usr/data/printer_data"
    mkdir -p "$pkg/components" "$data/config" "$root/home" "$root/proc/1444"
    : > "$pkg/moonraker.py"
    printf '[server]\nhost: 0.0.0.0\n' > "$data/config/moonraker.conf"
    printf '%s\0' "$root/usr/data/moonraker/moonraker-env/bin/python" "$pkg/moonraker.py" \
        -d "$data" > "$root/proc/1444/cmdline"
    for cmd in sudo service; do
        mock_command_script "$cmd" 'exit 0'
    done
    mock_real_curl 0
    mock_command_script "wget" 'exit 8'
}

@test "auto install finds a Creality K1 Moonraker from its running process" {
    local root="$BATS_TEST_TMPDIR/k1"
    setup_creality_k1_layout "$root"

    run env HOME="$root/home" HELIX_PROC_ROOT="$root/proc" sh "$SCRIPT" --no-restart --auto
    [ "$status" -eq 0 ] || fail "install failed: $output"
    [ -L "$root/usr/data/moonraker/moonraker/moonraker/components/helix_print.py" ]
    grep -q '^\[helix_print\]' "$root/usr/data/printer_data/config/moonraker.conf"
}

@test "auto uninstall finds a Creality K1 Moonraker from its running process" {
    local root="$BATS_TEST_TMPDIR/k1"
    setup_creality_k1_layout "$root"
    local target="$root/usr/data/moonraker/moonraker/moonraker/components/helix_print.py"
    ln -s "$REPO_ROOT/moonraker-plugin/helix_print.py" "$target"

    run env HOME="$root/home" HELIX_PROC_ROOT="$root/proc" sh "$SCRIPT" --no-restart --uninstall-auto
    [ "$status" -eq 0 ] || fail "uninstall failed: $output"
    [ ! -e "$target" ]
}

@test "plugin installer searches every Moonraker root the main installer knows" {
    local roots
    roots=$(sed -n '/^MOONRAKER_SRC_PATHS="/,/^"/p' "$REPO_ROOT/scripts/lib/installer/moonraker.sh" | grep '^/')
    [ -n "$roots" ] || fail "no roots parsed from scripts/lib/installer/moonraker.sh"
    local plugin_roots
    plugin_roots=$(sed -n '/^MOONRAKER_SRC_PATHS="/,/^"/p' "$SCRIPT" | grep '^/')
    local r
    for r in $roots; do
        printf '%s\n' "$plugin_roots" | grep -qx "$r" || fail "plugin install.sh misses Moonraker root $r"
    done
}

# Forge-X on an AD5M: Moonraker runs in a chroot at /data/.mod/.forge-x, so
# its argv names paths inside the chroot, and the installer runs either
# outside it (ssh) or inside it (HelixScreen's own install, which shares the
# chroot). moonraker.conf is the mod's own git-tracked file, reached through a
# symlink, and includes mod_data/user.moonraker.conf for the user's sections.
# The plugin sits under /opt/config, which the chroot sees at the same path.
setup_forgex_layout() {
    local root="$1"
    local chroot="$root/data/.mod/.forge-x"
    local pkg="$chroot/root/moonraker-env/moonraker"
    local cfg="$chroot/root/printer_data/config"
    mkdir -p "$pkg/components" "$cfg/mod" "$cfg/mod_data" "$root/home" "$root/proc/1925"
    : > "$pkg/moonraker.py"
    printf '[server]\nport: 7125\n\n[include mod_data/user.moonraker.conf]\n' > "$cfg/mod/moonraker.conf"
    ln -s mod/moonraker.conf "$cfg/moonraker.conf"
    printf '[authorization]\ntrusted_clients:\n  0.0.0.0/0\n' > "$cfg/mod_data/user.moonraker.conf"
    printf '%s\0' /root/moonraker-env/bin/python3 /root/moonraker-env/moonraker/moonraker.py \
        -d /root/printer_data > "$root/proc/1925/cmdline"
    ln -s "$chroot" "$root/proc/1925/root"
    # The plugin as installed under the bind-mounted /opt/config.
    mkdir -p "$chroot/opt/config/mod_data/helixscreen"
    cp -R "$REPO_ROOT/moonraker-plugin" "$chroot/opt/config/mod_data/helixscreen/"
    for cmd in sudo service; do
        mock_command_script "$cmd" 'exit 0'
    done
    mock_real_curl 0
    mock_command_script "wget" 'exit 8'
}

FORGEX_PLUGIN=/opt/config/mod_data/helixscreen/moonraker-plugin

@test "auto install from outside a Forge-X chroot links the plugin as Moonraker sees it" {
    local root="$BATS_TEST_TMPDIR/fx"
    setup_forgex_layout "$root"
    local chroot="$root/data/.mod/.forge-x"
    local cfg="$chroot/root/printer_data/config"
    cp "$cfg/mod/moonraker.conf" "$BATS_TEST_TMPDIR/mod.conf.before"

    run env HOME="$root/home" HELIX_PROC_ROOT="$root/proc" \
        sh "$chroot$FORGEX_PLUGIN/install.sh" --no-restart --auto
    [ "$status" -eq 0 ] || fail "install failed: $output"

    local link="$chroot/root/moonraker-env/moonraker/components/helix_print.py"
    [ -L "$link" ] || fail "no plugin link in the chroot's components"
    [ "$(readlink "$link")" = "$FORGEX_PLUGIN/helix_print.py" ] \
        || fail "link names $(readlink "$link"), not the in-chroot path"
    grep -q '^\[helix_print\]' "$cfg/mod_data/user.moonraker.conf" \
        || fail "section not in user.moonraker.conf"
    [ -L "$cfg/moonraker.conf" ] || fail "the mod's moonraker.conf symlink was replaced"
    cmp -s "$BATS_TEST_TMPDIR/mod.conf.before" "$cfg/mod/moonraker.conf" \
        || fail "the mod's own moonraker.conf was edited"
}

@test "auto install refuses a plugin Moonraker cannot see from its chroot" {
    local root="$BATS_TEST_TMPDIR/fx"
    setup_forgex_layout "$root"
    local chroot="$root/data/.mod/.forge-x"

    run env HOME="$root/home" HELIX_PROC_ROOT="$root/proc" sh "$SCRIPT" --no-restart --auto
    [ "$status" -ne 0 ] || fail "installed a link Moonraker cannot follow"
    contains "cannot see" "$output"
    [ ! -e "$chroot/root/moonraker-env/moonraker/components/helix_print.py" ] \
        && [ ! -L "$chroot/root/moonraker-env/moonraker/components/helix_print.py" ]
}

@test "auto uninstall from outside a Forge-X chroot removes the link and the user section" {
    local root="$BATS_TEST_TMPDIR/fx"
    setup_forgex_layout "$root"
    local chroot="$root/data/.mod/.forge-x"
    local cfg="$chroot/root/printer_data/config"
    run env HOME="$root/home" HELIX_PROC_ROOT="$root/proc" \
        sh "$chroot$FORGEX_PLUGIN/install.sh" --no-restart --auto
    [ "$status" -eq 0 ] || fail "install failed: $output"
    cp "$cfg/mod/moonraker.conf" "$BATS_TEST_TMPDIR/mod.conf.before"

    run env HOME="$root/home" HELIX_PROC_ROOT="$root/proc" \
        sh "$chroot$FORGEX_PLUGIN/install.sh" --no-restart --uninstall-auto
    [ "$status" -eq 0 ] || fail "uninstall failed: $output"
    [ ! -L "$chroot/root/moonraker-env/moonraker/components/helix_print.py" ]
    ! grep -q '^\[helix_print\]' "$cfg/mod_data/user.moonraker.conf"
    grep -q '^trusted_clients:' "$cfg/mod_data/user.moonraker.conf" \
        || fail "the user's own sections were lost"
    [ -L "$cfg/moonraker.conf" ]
    cmp -s "$BATS_TEST_TMPDIR/mod.conf.before" "$cfg/mod/moonraker.conf"
}

@test "with no service manager the auto install asks Moonraker to restart itself" {
    local root="$BATS_TEST_TMPDIR/fx"
    setup_forgex_layout "$root"
    local chroot="$root/data/.mod/.forge-x"
    local log="$BATS_TEST_TMPDIR/curl.log"
    mock_real_curl 0 "$log"

    local bin="$BATS_TEST_TMPDIR/no-service-path"
    mkdir -p "$bin"
    local tool p
    for tool in sh dirname basename cp mv rm ln grep awk sed tail tr cat date sleep readlink; do
        p="$(command -v "$tool")" && ln -sf "$p" "$bin/$tool"
    done
    ln -sf "$BATS_TEST_TMPDIR/bin/curl" "$bin/curl"
    # Present but managing no moonraker unit, as a stray systemctl would be.
    mock_command_script "systemctl" 'exit 1'
    ln -sf "$BATS_TEST_TMPDIR/bin/systemctl" "$bin/systemctl"

    run env HOME="$root/home" HELIX_PROC_ROOT="$root/proc" PATH="$bin:$HELIX_TEST_SANDBOX_BIN" \
        sh "$chroot$FORGEX_PLUGIN/install.sh" --auto
    [ "$status" -eq 0 ] || fail "install failed: $output"
    grep -q -- '-sf --max-time 5 -X POST http://127.0.0.1:7125/server/restart' "$log" \
        || fail "no self-restart request: $(cat "$log")"
}

@test "with only wget, as outside Forge-X's chroot, the auto install restarts and waits" {
    local root="$BATS_TEST_TMPDIR/fx"
    setup_forgex_layout "$root"
    local chroot="$root/data/.mod/.forge-x"
    local log="$BATS_TEST_TMPDIR/wget.log"
    mock_command_script "wget" "printf '%s\\n' \"\$*\" >> '$log'; exit 0"
    mock_command_script "systemctl" 'exit 1'

    local bin="$BATS_TEST_TMPDIR/wget-only-path"
    mkdir -p "$bin"
    local tool p
    for tool in sh dirname basename cp mv rm ln grep awk sed tail tr cat date sleep readlink; do
        p="$(command -v "$tool")" && ln -sf "$p" "$bin/$tool"
    done
    ln -sf "$BATS_TEST_TMPDIR/bin/wget" "$bin/wget"
    ln -sf "$BATS_TEST_TMPDIR/bin/systemctl" "$bin/systemctl"

    run env HOME="$root/home" HELIX_PROC_ROOT="$root/proc" PATH="$bin:$HELIX_TEST_SANDBOX_BIN" \
        sh "$chroot$FORGEX_PLUGIN/install.sh" --auto
    [ "$status" -eq 0 ] || fail "install failed: $output"
    grep -q -- "--post-data= http://127.0.0.1:7125/server/restart" "$log" \
        || fail "no self-restart request: $(cat "$log")"
    grep -q "http://127.0.0.1:7125/server/info" "$log" || fail "never waited for Moonraker"
    contains "Moonraker is ready" "$output"
}

# Klipper running outside the chroot, as on Forge-X. $2 picks its
# virtual_sdcard: "hides" carries Forge-X's filter dropping dot-directories
# from the files SDCARD_PRINT_FILE can open (its d[0] == '.' spelling),
# "hides-startswith" the same filter spelled with str.startswith, and
# anything else lists every directory.
add_running_klipper() {
    local root="$1" mode="$2"
    local extras="$root/opt/klipper/klippy/extras"
    mkdir -p "$extras" "$root/proc/1820"
    : > "$root/opt/klipper/klippy/klippy.py"
    {
        printf '            for root, dirs, files in os.walk(\n'
        printf '                    self.sdcard_dirname, followlinks=True, topdown=True):\n'
        case "$mode" in
            hides)
                printf '                # Ignore hidden files and directories\n'
                printf "                files = [f for f in files if not f[0] == '.']\n"
                printf "                dirs[:] = [d for d in dirs if not d[0] == '.']\n"
                ;;
            hides-startswith)
                printf "                dirs[:] = [d for d in dirs if not d.startswith('.')]\n"
                ;;
        esac
        printf '                for name in files:\n'
    } > "$extras/virtual_sdcard.py"
    printf '%s\0' /opt/Python-3.7.11/bin/python3.7 "$root/opt/klipper/klippy/klippy.py" \
        /opt/config/printer.cfg > "$root/proc/1820/cmdline"
}

forgex_install() {
    local root="$1"
    run env HOME="$root/home" HELIX_PROC_ROOT="$root/proc" \
        sh "$root/data/.mod/.forge-x$FORGEX_PLUGIN/install.sh" --no-restart --auto
    [ "$status" -eq 0 ] || fail "install failed: $output"
}

user_conf_of() {
    printf '%s' "$1/data/.mod/.forge-x/root/printer_data/config/mod_data/user.moonraker.conf"
}

@test "a Klipper that skips dot-directories gets a visible symlink_dir" {
    local root="$BATS_TEST_TMPDIR/fx"
    setup_forgex_layout "$root"
    add_running_klipper "$root" hides

    forgex_install "$root"

    local conf
    conf=$(user_conf_of "$root")
    awk '/^\[helix_print\]/ { getline; print; exit }' "$conf" | grep -qx 'symlink_dir: helix_print' \
        || fail "symlink_dir not set under [helix_print]: $(cat "$conf")"
}

@test "re-installing does not add a second symlink_dir" {
    local root="$BATS_TEST_TMPDIR/fx"
    setup_forgex_layout "$root"
    add_running_klipper "$root" hides

    forgex_install "$root"
    forgex_install "$root"

    [ "$(grep -c '^symlink_dir:' "$(user_conf_of "$root")")" -eq 1 ]
}

@test "a symlink_dir the user set is kept" {
    local root="$BATS_TEST_TMPDIR/fx"
    setup_forgex_layout "$root"
    add_running_klipper "$root" hides
    local conf
    conf=$(user_conf_of "$root")
    printf '\n[helix_print]\nsymlink_dir: my_links\ncleanup_delay: 600\n' >> "$conf"

    forgex_install "$root"

    [ "$(grep -c '^symlink_dir:' "$conf")" -eq 1 ]
    grep -qx 'symlink_dir: my_links' "$conf"
    contains "Keeping the symlink_dir" "$output"
}

@test "a Klipper that lists dot-directories keeps the default hidden symlink_dir" {
    local root="$BATS_TEST_TMPDIR/fx"
    setup_forgex_layout "$root"
    add_running_klipper "$root" lists

    forgex_install "$root"

    grep -q '^\[helix_print\]' "$(user_conf_of "$root")"
    ! grep -q 'symlink_dir' "$(user_conf_of "$root")"
}

@test "install.sh and the main installer carry the same request_moonraker_restart" {
    local fn='/^request_moonraker_restart() {$/,/^}$/p'
    local a b
    a=$(sed -n "$fn" "$SCRIPT")
    b=$(sed -n "$fn" "$REPO_ROOT/scripts/lib/installer/moonraker.sh")
    [ -n "$a" ] || fail "no request_moonraker_restart in $SCRIPT"
    [ "$a" = "$b" ] || fail "the two copies differ"
}

@test "install.sh carries the main installer's _has_real_curl" {
    local fn='/^_has_real_curl() {$/,/^}$/p'
    local a b
    a=$(sed -n "$fn" "$SCRIPT")
    b=$(sed -n "$fn" "$REPO_ROOT/scripts/lib/installer/release.sh")
    [ -n "$a" ] || fail "no _has_real_curl in $SCRIPT"
    [ "$a" = "$b" ] || fail "the two copies differ"
}

@test "a curl that answers with an HTTP error does not count as Moonraker being up" {
    local root="$BATS_TEST_TMPDIR/fx"
    setup_forgex_layout "$root"
    local chroot="$root/data/.mod/.forge-x"
    # Moonraker answering 401: curl exits 0 unless -f makes that a 22.
    mock_command_script "curl" 'case "$1" in --version) echo "curl 8.5.0"; exit 0;; esac
for a in "$@"; do case "$a" in -f|-sf) exit 22;; esac; done
exit 0'
    mock_command_script "wget" 'exit 8'
    mock_command_script "sleep" 'exit 0'
    mock_command_script "systemctl" 'exit 1'

    run env HOME="$root/home" HELIX_PROC_ROOT="$root/proc" \
        sh "$chroot$FORGEX_PLUGIN/install.sh" --auto
    contains "did not respond" "$output"
    ! contains "Moonraker is ready" "$output"
}

@test "an include line with CRLF and trailing blanks still finds user.moonraker.conf" {
    local root="$BATS_TEST_TMPDIR/fx"
    setup_forgex_layout "$root"
    local cfg="$root/data/.mod/.forge-x/root/printer_data/config"
    printf '[server]\r\nport: 7125\r\n\r\n[include mod_data/user.moonraker.conf]  \t\r\n' \
        > "$cfg/mod/moonraker.conf"
    cp "$cfg/mod/moonraker.conf" "$BATS_TEST_TMPDIR/mod.conf.before"

    run env HOME="$root/home" HELIX_PROC_ROOT="$root/proc" \
        sh "$root/data/.mod/.forge-x$FORGEX_PLUGIN/install.sh" --no-restart --auto
    [ "$status" -eq 0 ] || fail "install failed: $output"

    grep -q '^\[helix_print\]' "$cfg/mod_data/user.moonraker.conf" \
        || fail "section not in user.moonraker.conf"
    cmp -s "$BATS_TEST_TMPDIR/mod.conf.before" "$cfg/mod/moonraker.conf" \
        || fail "the mod's own moonraker.conf was edited"
}

@test "a Klipper pruning dot-directories with startswith gets a visible symlink_dir too" {
    local root="$BATS_TEST_TMPDIR/fx"
    setup_forgex_layout "$root"
    add_running_klipper "$root" hides-startswith

    forgex_install "$root"

    grep -qx 'symlink_dir: helix_print' "$(user_conf_of "$root")"
}

@test "with no Klipper running the default hidden symlink_dir stays" {
    local root="$BATS_TEST_TMPDIR/fx"
    setup_forgex_layout "$root"

    forgex_install "$root"

    ! grep -q 'symlink_dir' "$(user_conf_of "$root")"
}

# A Pi-style HOME with a Moonraker checkout and a moonraker.conf that has no
# [helix_print] yet, the restart path shimmed as in setup_instrumented_home.
setup_plain_home() {
    local home="$1"
    mkdir -p "$home/moonraker/moonraker/components" "$home/printer_data/config"
    printf '[server]\nhost: 0.0.0.0\n' > "$home/printer_data/config/moonraker.conf"
    for cmd in sudo service; do
        mock_command_script "$cmd" 'exit 0'
    done
    mock_real_curl 0
    mock_command_script "wget" 'exit 8'
}

# Every [helix_print] header under config dir $1, across all its files but
# the backups an edit leaves.
count_helix_sections() {
    find "$1" -type f ! -name '*.bak.*' -exec grep -h '^\[helix_print\]' {} + | wc -l | tr -d ' '
}

@test "auto install leaves a [helix_print] in an included file as the only one" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_plain_home "$home"
    local c="$home/printer_data/config"
    mkdir -p "$c/conf.d"
    printf '[server]\nhost: 0.0.0.0\n\n[include conf.d/*.conf]\n' > "$c/moonraker.conf"
    printf '[helix_print]\nenabled: True\n' > "$c/conf.d/helix.conf"
    cp "$c/moonraker.conf" "$BATS_TEST_TMPDIR/conf.before"

    run env HOME="$home" sh "$SCRIPT" --no-restart --auto
    [ "$status" -eq 0 ] || fail "install failed: $output"

    [ "$(count_helix_sections "$c")" -eq 1 ] || fail "duplicate [helix_print]: $(cat "$c"/moonraker.conf "$c"/conf.d/*)"
    cmp -s "$BATS_TEST_TMPDIR/conf.before" "$c/moonraker.conf" || fail "moonraker.conf was edited"
    [ -L "$home/moonraker/moonraker/components/helix_print.py" ]
}

@test "auto uninstall removes a [helix_print] that lives in an included file" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_plain_home "$home"
    local c="$home/printer_data/config"
    printf '[server]\nhost: 0.0.0.0\n\n[include helix.conf]\n' > "$c/moonraker.conf"
    printf '[helix_print]\nenabled: True\n\n[octoprint_compat]\n' > "$c/helix.conf"
    ln -s "$REPO_ROOT/moonraker-plugin/helix_print.py" "$home/moonraker/moonraker/components/helix_print.py"

    run env HOME="$home" sh "$SCRIPT" --no-restart --uninstall-auto
    [ "$status" -eq 0 ] || fail "uninstall failed: $output"

    [ "$(count_helix_sections "$c")" -eq 0 ] || fail "[helix_print] survived: $(cat "$c/helix.conf")"
    grep -q '^\[octoprint_compat\]' "$c/helix.conf" || fail "the include's other sections were lost"
    [ ! -L "$home/moonraker/moonraker/components/helix_print.py" ]
}

@test "auto install refuses a components dir it cannot write, and writes nothing" {
    [ "$(id -u)" -ne 0 ] || skip "running as root ignores the read-only mode"
    local home="$BATS_TEST_TMPDIR/home"
    setup_plain_home "$home"
    local c="$home/printer_data/config"
    cp -R "$c" "$BATS_TEST_TMPDIR/config.before"
    chmod 0555 "$home/moonraker/moonraker/components"

    run env HOME="$home" sh "$SCRIPT" --no-restart --auto
    chmod 0755 "$home/moonraker/moonraker/components"

    [ "$status" -eq 1 ]
    contains "Cannot write to components directory" "$output"
    [ ! -e "$home/moonraker/moonraker/components/helix_print.py" ]
    diff -r "$BATS_TEST_TMPDIR/config.before" "$c" || fail "the config dir changed"
}

@test "auto install refuses a moonraker.conf it cannot write, and writes nothing" {
    [ "$(id -u)" -ne 0 ] || skip "running as root ignores the read-only mode"
    local home="$BATS_TEST_TMPDIR/home"
    setup_plain_home "$home"
    local c="$home/printer_data/config"
    chmod 0444 "$c/moonraker.conf"
    cp -R "$c" "$BATS_TEST_TMPDIR/config.before"

    run env HOME="$home" sh "$SCRIPT" --no-restart --auto
    chmod 0644 "$c/moonraker.conf"

    [ "$status" -eq 1 ]
    contains "Cannot write to $c/moonraker.conf" "$output"
    [ ! -e "$home/moonraker/moonraker/components/helix_print.py" ] \
        && [ ! -L "$home/moonraker/moonraker/components/helix_print.py" ]
    diff -r "$BATS_TEST_TMPDIR/config.before" "$c" || fail "the config dir changed"
}

# Pins behaviour that already holds: the section starts on a line of its own.
@test "auto install after a moonraker.conf with no trailing newline keeps its last line" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_plain_home "$home"
    local c="$home/printer_data/config"
    printf '[server]\nhost: 0.0.0.0' > "$c/moonraker.conf"

    run env HOME="$home" sh "$SCRIPT" --no-restart --auto
    [ "$status" -eq 0 ] || fail "install failed: $output"

    grep -qx 'host: 0.0.0.0' "$c/moonraker.conf" || fail "last line damaged: $(cat "$c/moonraker.conf")"
    grep -qx '\[helix_print\]' "$c/moonraker.conf" || fail "no section line: $(cat "$c/moonraker.conf")"
}

# A cat that fails part-way through writing a config file: the first bytes of
# a staged copy reach its stdout, then it errors. Any other cat runs normally.
mock_cat_failing_midway() {
    local real
    real=$(command -v cat)
    mock_command_script "cat" "case \"\$1\" in *.tmp|*.helixtmp*) head -c 12 \"\$1\"; exit 1;; esac
exec $real \"\$@\""
}

# An awk that fails part-way through the section removal's rewrite; any other
# awk runs normally.
mock_awk_failing_midway() {
    local real
    real=$(command -v awk)
    mock_command_script "awk" "case \"\$*\" in *'skip = 1'*|*'done = 1'*) printf '[server]\\n'; exit 1;; esac
exec $real \"\$@\""
}

# An awk whose write to a file stops short and still exits 0, as BusyBox awk
# does on a full disk, for the section removal's rewrite. Output into a pipe
# comes through whole; any other awk runs normally.
mock_awk_cut_short_exiting_0() {
    local real
    real=$(command -v awk)
    mock_command_script "awk" "case \"\$*\" in *'skip = 1'*)
    if [ -f /dev/stdout ]; then $real \"\$@\" | head -c 30; exit 0; fi ;; esac
exec $real \"\$@\""
}

@test "a section removal cut short by a full disk leaves moonraker.conf whole, though awk exits 0" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_instrumented_home "$home"
    local c="$home/printer_data/config"
    write_original_printer_cfg "$c/printer.cfg"
    cp "$c/moonraker.conf" "$BATS_TEST_TMPDIR/conf.before"
    mock_awk_cut_short_exiting_0

    run env HOME="$home" sh "$SCRIPT" --no-restart --uninstall-auto

    [ "$status" -eq 2 ] || fail "want needs-attention (2), got $status: $output"
    cmp -s "$BATS_TEST_TMPDIR/conf.before" "$c/moonraker.conf" ||
        fail "moonraker.conf replaced: $(cat "$c/moonraker.conf")"
    [ -z "$(find "$c" -name 'moonraker.conf.*tmp*')" ] || fail "staging file left: $(ls "$c")"
}

@test "a section removal whose write fails part-way leaves moonraker.conf whole" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_instrumented_home "$home"
    local c="$home/printer_data/config"
    write_original_printer_cfg "$c/printer.cfg"
    cp "$c/moonraker.conf" "$BATS_TEST_TMPDIR/conf.before"
    mock_cat_failing_midway

    run env HOME="$home" sh "$SCRIPT" --no-restart --uninstall-auto

    # Removed or left in place, but never cut short.
    grep -qx 'host: 0.0.0.0' "$c/moonraker.conf" && grep -qx '\[octoprint_compat\]' "$c/moonraker.conf" \
        || fail "moonraker.conf damaged: $(cat "$c/moonraker.conf")"
}

@test "a section removal whose rewrite fails reports it and leaves no staging file" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_instrumented_home "$home"
    local c="$home/printer_data/config"
    write_original_printer_cfg "$c/printer.cfg"
    cp "$c/moonraker.conf" "$BATS_TEST_TMPDIR/conf.before"
    mock_awk_failing_midway

    run env HOME="$home" sh "$SCRIPT" --no-restart --uninstall-auto

    [ "$status" -eq 2 ] || fail "want needs-attention (2), got $status: $output"
    cmp -s "$BATS_TEST_TMPDIR/conf.before" "$c/moonraker.conf" || fail "moonraker.conf damaged"
    [ -z "$(find "$c" -name 'moonraker.conf.*tmp*')" ] || fail "staging file left: $(ls "$c")"
}

@test "a symlink_dir insert whose write fails part-way leaves the user conf whole" {
    local root="$BATS_TEST_TMPDIR/fx"
    setup_forgex_layout "$root"
    add_running_klipper "$root" hides
    local conf
    conf=$(user_conf_of "$root")
    printf '\n[helix_print]\n' >> "$conf"
    cp "$conf" "$BATS_TEST_TMPDIR/conf.before"
    mock_cat_failing_midway

    run env HOME="$root/home" HELIX_PROC_ROOT="$root/proc" \
        sh "$root/data/.mod/.forge-x$FORGEX_PLUGIN/install.sh" --no-restart --auto

    # Set or left unset, but never cut short.
    grep -qx '  0.0.0.0/0' "$conf" && grep -qx '\[helix_print\]' "$conf" \
        || fail "user conf damaged: $(cat "$conf")"
}

@test "a section rewrite through a symlinked config keeps the link" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_instrumented_home "$home"
    local c="$home/printer_data/config"
    write_original_printer_cfg "$c/printer.cfg"
    mkdir -p "$home/real"
    mv "$c/moonraker.conf" "$home/real/moonraker.conf"
    ln -s "$home/real/moonraker.conf" "$c/moonraker.conf"

    run env HOME="$home" sh "$SCRIPT" --no-restart --uninstall-auto
    [ "$status" -eq 0 ] || fail "uninstall failed: $output"

    [ -L "$c/moonraker.conf" ] || fail "the symlink was replaced by a file"
    refute_grep '^\[helix_print\]' "$home/real/moonraker.conf"
    grep -q '^\[octoprint_compat\]' "$home/real/moonraker.conf"
}

@test "install and uninstall cycles restore moonraker.conf byte for byte" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_plain_home "$home"
    local c="$home/printer_data/config"
    printf '[server]\nhost: 0.0.0.0\n\n[authorization]\ntrusted_clients:\n    127.0.0.1\n' \
        > "$c/moonraker.conf"
    cp "$c/moonraker.conf" "$BATS_TEST_TMPDIR/conf.before"

    local cycle
    for cycle in 1 2; do
        run env HOME="$home" sh "$SCRIPT" --no-restart --auto
        [ "$status" -eq 0 ] || fail "install $cycle failed: $output"
        run env HOME="$home" sh "$SCRIPT" --no-restart --uninstall-auto
        [ "$status" -eq 0 ] || fail "uninstall $cycle failed: $output"
    done

    cmp "$BATS_TEST_TMPDIR/conf.before" "$c/moonraker.conf" \
        || fail "moonraker.conf drifted: $(diff "$BATS_TEST_TMPDIR/conf.before" "$c/moonraker.conf")"
}

@test "an install and uninstall cycle restores Forge-X's user conf byte for byte" {
    local root="$BATS_TEST_TMPDIR/fx"
    setup_forgex_layout "$root"
    local conf
    conf=$(user_conf_of "$root")
    cp "$conf" "$BATS_TEST_TMPDIR/conf.before"

    forgex_install "$root"
    run env HOME="$root/home" HELIX_PROC_ROOT="$root/proc" \
        sh "$root/data/.mod/.forge-x$FORGEX_PLUGIN/install.sh" --no-restart --uninstall-auto
    [ "$status" -eq 0 ] || fail "uninstall failed: $output"

    cmp "$BATS_TEST_TMPDIR/conf.before" "$conf" \
        || fail "user conf drifted: $(diff "$BATS_TEST_TMPDIR/conf.before" "$conf")"
}

# Pins behaviour that already holds: removing a section between two others
# keeps the blank line that separates them.
@test "removing a [helix_print] between two sections keeps them apart" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_plain_home "$home"
    local c="$home/printer_data/config"
    printf '[server]\nhost: 0.0.0.0\n\n[helix_print]\nenabled: True\n\n[octoprint_compat]\n' \
        > "$c/moonraker.conf"

    run env HOME="$home" sh "$SCRIPT" --no-restart --uninstall-auto
    [ "$status" -eq 0 ] || fail "uninstall failed: $output"

    [ "$(cat "$c/moonraker.conf")" = "$(printf '[server]\nhost: 0.0.0.0\n\n[octoprint_compat]')" ] \
        || fail "unexpected result: $(cat "$c/moonraker.conf")"
}

@test "an install with no trailing newline uninstalls to the same lines" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_plain_home "$home"
    local c="$home/printer_data/config"
    printf '[server]\nhost: 0.0.0.0' > "$c/moonraker.conf"

    run env HOME="$home" sh "$SCRIPT" --no-restart --auto
    [ "$status" -eq 0 ] || fail "install failed: $output"
    run env HOME="$home" sh "$SCRIPT" --no-restart --uninstall-auto
    [ "$status" -eq 0 ] || fail "uninstall failed: $output"

    # The last line comes back terminated; nothing else changes.
    [ "$(cat "$c/moonraker.conf")" = "$(printf '[server]\nhost: 0.0.0.0')" ] \
        || fail "unexpected result: $(cat "$c/moonraker.conf")"
}

@test "install and uninstall keep only the newest of their own backups, and nobody else's" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_plain_home "$home"
    local c="$home/printer_data/config"
    printf 'mine\n' > "$c/moonraker.conf.bak.20200101_000000"
    printf 'old\n' > "$c/moonraker.conf.bak.helix.20200101_000001"

    run env HOME="$home" sh "$SCRIPT" --no-restart --auto
    [ "$status" -eq 0 ] || fail "install failed: $output"
    run env HOME="$home" sh "$SCRIPT" --no-restart --uninstall-auto
    [ "$status" -eq 0 ] || fail "uninstall failed: $output"

    grep -qx mine "$c/moonraker.conf.bak.20200101_000000" || fail "deleted a backup it did not make"
    local backups
    backups=$(find "$c" -name 'moonraker.conf.bak.helix.*' | wc -l | tr -d ' ')
    [ "$backups" -eq 1 ] || fail "want 1 backup of ours, have: $(ls "$c")"
    [ ! -e "$c/moonraker.conf.bak.helix.20200101_000001" ]
    # The survivor is the uninstall's: the file with the section in it.
    grep -q '^\[helix_print\]' "$c"/moonraker.conf.bak.helix.* || fail "kept the wrong backup"
}

@test "installing over a section set to enabled: False turns the plugin on" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_plain_home "$home"
    local c="$home/printer_data/config"
    printf '[server]\nhost: 0.0.0.0\n\n[helix_print]\nenabled: False\ncleanup_delay: 3600\n\n[octoprint_compat]\n' \
        > "$c/moonraker.conf"

    run env HOME="$home" sh "$SCRIPT" --no-restart --auto
    [ "$status" -eq 0 ] || fail "install failed: $output"

    [ "$(count_helix_sections "$c")" -eq 1 ] || fail "duplicate [helix_print]: $(cat "$c/moonraker.conf")"
    grep -qx 'enabled: True' "$c/moonraker.conf" || fail "not enabled: $(cat "$c/moonraker.conf")"
    ! grep -qi '^enabled[[:space:]]*[:=][[:space:]]*false' "$c/moonraker.conf" || fail "still disabled"
    grep -qx 'cleanup_delay: 3600' "$c/moonraker.conf" || fail "the section's other options were lost"
    grep -q '^\[octoprint_compat\]' "$c/moonraker.conf" || fail "the next section was lost"
    [ -L "$home/moonraker/moonraker/components/helix_print.py" ]
}

@test "installing turns on a disabled section in an included file, whatever the false spelling" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_plain_home "$home"
    local c="$home/printer_data/config"
    printf '[server]\nhost: 0.0.0.0\n\n[include helix.conf]\n' > "$c/moonraker.conf"
    printf '[helix_print]\n  Enabled = no\n' > "$c/helix.conf"
    cp "$c/moonraker.conf" "$BATS_TEST_TMPDIR/conf.before"

    run env HOME="$home" sh "$SCRIPT" --no-restart --auto
    [ "$status" -eq 0 ] || fail "install failed: $output"

    grep -qx 'enabled: True' "$c/helix.conf" || fail "not enabled: $(cat "$c/helix.conf")"
    [ "$(count_helix_sections "$c")" -eq 1 ]
    cmp -s "$BATS_TEST_TMPDIR/conf.before" "$c/moonraker.conf" || fail "moonraker.conf was edited"
}

@test "installing over an enabled section leaves the config untouched" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_plain_home "$home"
    local c="$home/printer_data/config"
    printf '[server]\nhost: 0.0.0.0\n\n[helix_print]\nenabled: True\n# enabled: False\n' > "$c/moonraker.conf"
    cp "$c/moonraker.conf" "$BATS_TEST_TMPDIR/conf.before"

    run env HOME="$home" sh "$SCRIPT" --no-restart --auto
    [ "$status" -eq 0 ] || fail "install failed: $output"

    cmp -s "$BATS_TEST_TMPDIR/conf.before" "$c/moonraker.conf" || fail "edited: $(cat "$c/moonraker.conf")"
    [ -z "$(find "$c" -name 'moonraker.conf.bak.*')" ] || fail "backed up a file it did not change"
}

@test "auto uninstall removes the updater entry a git-clone install wrote, and its marker" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_plain_home "$home"
    local c="$home/printer_data/config"
    cp "$c/moonraker.conf" "$BATS_TEST_TMPDIR/conf.before"
    printf '\n#~# --- HelixPrint Plugin ---\n[helix_print]\n# enabled: True\n\n[update_manager helix_print]\ntype: git_repo\npath: %s/helix_print\nmanaged_services: moonraker\n' \
        "$home" >> "$c/moonraker.conf"
    ln -s "$REPO_ROOT/moonraker-plugin/helix_print.py" "$home/moonraker/moonraker/components/helix_print.py"

    run env HOME="$home" sh "$SCRIPT" --no-restart --uninstall-auto
    [ "$status" -eq 0 ] || fail "uninstall failed: $output"

    cmp -s "$BATS_TEST_TMPDIR/conf.before" "$c/moonraker.conf" ||
        fail "moonraker.conf differs: $(diff "$BATS_TEST_TMPDIR/conf.before" "$c/moonraker.conf")"
}

@test "auto install drops an updater entry for a git checkout of the plugin" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_plain_home "$home"
    local c="$home/printer_data/config"
    printf '[server]\nhost: 0.0.0.0\n\n[update_manager helix_print]\ntype: git_repo\npath: ~/helix_print\n\n[octoprint_compat]\n' \
        > "$c/moonraker.conf"

    run env HOME="$home" sh "$SCRIPT" --no-restart --auto
    [ "$status" -eq 0 ] || fail "install failed: $output"

    ! grep -q '^\[update_manager helix_print\]' "$c/moonraker.conf" || fail "kept: $(cat "$c/moonraker.conf")"
    grep -q '^\[octoprint_compat\]' "$c/moonraker.conf" || fail "lost the next section"
    [ "$(count_helix_sections "$c")" -eq 1 ]
}

# A plugin installed from an INSTALL_DIR that has since moved: the link names
# the old tree, the [helix_print] section is in place.
setup_moved_install() {
    local home="$1"
    setup_plain_home "$home"
    printf '\n[helix_print]\n' >> "$home/printer_data/config/moonraker.conf"
    ln -s "$home/old-helixscreen/moonraker-plugin/helix_print.py" \
        "$home/moonraker/moonraker/components/helix_print.py"
}

@test "relink re-points a link left by a moved install at this tree" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_moved_install "$home"
    local c="$home/printer_data/config"
    cp -R "$c" "$BATS_TEST_TMPDIR/config.before"

    run env HOME="$home" sh "$SCRIPT" --no-restart --relink
    [ "$status" -eq 3 ] || fail "want 3 (relinked), got $status: $output"

    [ "$(readlink "$home/moonraker/moonraker/components/helix_print.py")" = "$(dirname "$SCRIPT")/helix_print.py" ] ||
        fail "link: $(readlink "$home/moonraker/moonraker/components/helix_print.py")"
    diff -r "$BATS_TEST_TMPDIR/config.before" "$c" || fail "relink edited the config"
}

@test "relink leaves a link that already names this tree alone" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_moved_install "$home"
    ln -sf "$(dirname "$SCRIPT")/helix_print.py" "$home/moonraker/moonraker/components/helix_print.py"

    run env HOME="$home" sh "$SCRIPT" --no-restart --relink
    [ "$status" -eq 0 ] || fail "want 0 (current), got $status: $output"
}

@test "relink installs nothing where the plugin was never linked" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_plain_home "$home"
    printf '\n[helix_print]\n' >> "$home/printer_data/config/moonraker.conf"

    run env HOME="$home" sh "$SCRIPT" --no-restart --relink
    [ "$status" -eq 4 ] || fail "want 4 (not ours), got $status: $output"
    [ ! -e "$home/moonraker/moonraker/components/helix_print.py" ] &&
        [ ! -L "$home/moonraker/moonraker/components/helix_print.py" ] || fail "created a link"
}

@test "relink leaves a link with no [helix_print] section alone" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_plain_home "$home"
    ln -s "$home/old-helixscreen/moonraker-plugin/helix_print.py" \
        "$home/moonraker/moonraker/components/helix_print.py"

    run env HOME="$home" sh "$SCRIPT" --no-restart --relink
    [ "$status" -eq 4 ] || fail "want 4 (not ours), got $status: $output"
    [ "$(readlink "$home/moonraker/moonraker/components/helix_print.py")" = "$home/old-helixscreen/moonraker-plugin/helix_print.py" ]
}

@test "relink leaves a regular helix_print.py file alone" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_plain_home "$home"
    printf '\n[helix_print]\n' >> "$home/printer_data/config/moonraker.conf"
    printf '# copied by hand\n' > "$home/moonraker/moonraker/components/helix_print.py"

    run env HOME="$home" sh "$SCRIPT" --no-restart --relink
    [ "$status" -eq 4 ] || fail "want 4 (not ours), got $status: $output"
    [ ! -L "$home/moonraker/moonraker/components/helix_print.py" ]
    grep -q 'copied by hand' "$home/moonraker/moonraker/components/helix_print.py"
}

@test "relink from outside a Forge-X chroot links the plugin as Moonraker sees it" {
    local root="$BATS_TEST_TMPDIR/fx"
    setup_forgex_layout "$root"
    forgex_install "$root"
    local chroot="$root/data/.mod/.forge-x"
    local link="$chroot/root/moonraker-env/moonraker/components/helix_print.py"
    [ "$(readlink "$link")" = "$FORGEX_PLUGIN/helix_print.py" ] || fail "setup: link $(readlink "$link")"
    ln -sf /opt/config/mod_data/old-helixscreen/moonraker-plugin/helix_print.py "$link"

    run env HOME="$root/home" HELIX_PROC_ROOT="$root/proc" \
        sh "$chroot$FORGEX_PLUGIN/install.sh" --no-restart --relink
    [ "$status" -eq 3 ] || fail "want 3 (relinked), got $status: $output"
    [ "$(readlink "$link")" = "$FORGEX_PLUGIN/helix_print.py" ] || fail "link: $(readlink "$link")"

    run env HOME="$root/home" HELIX_PROC_ROOT="$root/proc" \
        sh "$chroot$FORGEX_PLUGIN/install.sh" --no-restart --relink
    [ "$status" -eq 0 ] || fail "want 0 (current) on the second run, got $status: $output"
}

# The app on the host side of Forge-X's bind mount: its tree and the chroot's
# view of it are two paths to the same files. A hard link stands in for the
# bind mount, so the two paths resolve apart and name one inode.
setup_forgex_bind_mounted_app() {
    local root="$1"
    local chroot="$root/data/.mod/.forge-x"
    local host="$root/opt/config/mod_data/helixscreen/moonraker-plugin"
    mkdir -p "$(dirname "$host")" "$chroot$host"
    cp -R "$REPO_ROOT/moonraker-plugin" "$host"
    ln "$host/helix_print.py" "$chroot$host/helix_print.py"
    printf '%s' "$host"
}

@test "relink from the host side of a Forge-X bind mount finds its own link current" {
    local root="$BATS_TEST_TMPDIR/fx"
    setup_forgex_layout "$root"
    local host
    host=$(setup_forgex_bind_mounted_app "$root")
    local link="$root/data/.mod/.forge-x/root/moonraker-env/moonraker/components/helix_print.py"
    run env HOME="$root/home" HELIX_PROC_ROOT="$root/proc" sh "$host/install.sh" --no-restart --auto
    [ "$status" -eq 0 ] || fail "install failed: $output"
    [ "$(readlink "$link")" = "$host/helix_print.py" ] || fail "setup: link $(readlink "$link")"

    local run_no
    for run_no in 1 2; do
        run env HOME="$root/home" HELIX_PROC_ROOT="$root/proc" sh "$host/install.sh" --no-restart --relink
        [ "$status" -eq 0 ] || fail "relink run $run_no: want 0 (current), got $status: $output"
    done
}

@test "an include cycle through .. is walked once" {
    local c="$BATS_TEST_TMPDIR/cfg"
    mkdir -p "$c/conf.d"
    printf '[server]\n\n[include conf.d/*.conf]\n' > "$c/moonraker.conf"
    printf '[helix_print]\n\n[include ../moonraker.conf]\n' > "$c/conf.d/a.conf"

    run timeout 10 sh -c '. "$1"; moonraker_conf_files "$2"' sh \
        "$REPO_ROOT/moonraker-plugin/helix_print_conf.sh" "$c/moonraker.conf"
    [ "$status" -eq 0 ] || fail "walk did not end (status $status)"
    [ "$(printf '%s\n' "$output" | wc -l)" -eq 2 ] || fail "files: $(printf '%s\n' "$output" | head -n 4)"
}

# A systemd host where the moonraker unit is listed but sudo refuses, as for
# a user whose sudo wants a password. $1 logs every curl call.
mock_systemd_without_sudo() {
    mock_command_script "systemctl" 'case "$1" in list-units) echo "moonraker.service loaded active running";; esac; exit 0'
    mock_command_script "sudo" 'echo "sudo: a password is required" >&2; exit 1'
    mock_real_curl 0 "$1"
}

@test "an auto install whose sudo restart fails asks Moonraker to restart itself" {
    local home="$BATS_TEST_TMPDIR/home" log="$BATS_TEST_TMPDIR/curl.log"
    setup_plain_home "$home"
    mock_systemd_without_sudo "$log"

    run env HOME="$home" sh "$SCRIPT" --auto
    [ "$status" -eq 0 ] || fail "install failed: $output"
    grep -q -- '-X POST http://127.0.0.1:7125/server/restart' "$log" || fail "no self-restart request: $(cat "$log" 2>/dev/null)"
    contains "Asked Moonraker to restart itself" "$output"
    contains "Auto-install complete" "$output"
}

@test "an auto uninstall whose sudo restart fails asks Moonraker to restart itself" {
    local home="$BATS_TEST_TMPDIR/home" log="$BATS_TEST_TMPDIR/curl.log"
    setup_instrumented_home "$home"
    mock_systemd_without_sudo "$log"

    run env HOME="$home" sh "$SCRIPT" --uninstall-auto
    [ "$status" -eq 0 ] || fail "uninstall failed: $output"
    grep -q -- '-X POST http://127.0.0.1:7125/server/restart' "$log" || fail "no self-restart request: $(cat "$log" 2>/dev/null)"
    contains "Auto-uninstall complete" "$output"
}

@test "an auto install no restart reaches says Moonraker needs one, never complete" {
    local home="$BATS_TEST_TMPDIR/home" log="$BATS_TEST_TMPDIR/curl.log"
    setup_plain_home "$home"
    mock_systemd_without_sudo "$log"
    mock_real_curl 22 "$log"
    mock_command_script "sleep" 'exit 0'

    run env HOME="$home" sh "$SCRIPT" --auto
    [ "$status" -eq 0 ] || fail "install failed: $output"
    [ -L "$home/moonraker/moonraker/components/helix_print.py" ]
    contains "Moonraker needs a restart" "$output"
    if printf '%s' "$output" | grep -qi 'complete'; then fail "claimed completion: $output"; fi
    if grep -q '/server/info' "$log"; then fail "waited on a Moonraker nothing restarted"; fi
}

@test "an auto uninstall no restart reaches says Moonraker needs one, never complete" {
    local home="$BATS_TEST_TMPDIR/home" log="$BATS_TEST_TMPDIR/curl.log"
    setup_instrumented_home "$home"
    mock_systemd_without_sudo "$log"
    mock_real_curl 22 "$log"
    mock_command_script "sleep" 'exit 0'

    run env HOME="$home" sh "$SCRIPT" --uninstall-auto
    [ "$status" -eq 0 ] || fail "uninstall failed: $output"
    [ ! -e "$home/moonraker/moonraker/components/helix_print.py" ]
    contains "Moonraker needs a restart" "$output"
    if printf '%s' "$output" | grep -qi 'complete'; then fail "claimed completion: $output"; fi
}

@test "auto install refuses a components dir that takes no link though test -w passes, and writes nothing" {
    local home="$BATS_TEST_TMPDIR/home"
    setup_plain_home "$home"
    local c="$home/printer_data/config"
    cp -R "$c" "$BATS_TEST_TMPDIR/config.before"
    local real_ln
    real_ln=$(command -v ln)
    # The components dir stays mode 755, as a read-only mount looks to root.
    mock_command_script "ln" "for a; do case \"\$a\" in */components/*) echo \"ln: \$a: Read-only file system\" >&2; exit 1;; esac; done; exec '$real_ln' \"\$@\""

    run env HOME="$home" sh "$SCRIPT" --no-restart --auto
    [ "$status" -eq 1 ]
    contains "Cannot write to components directory" "$output"
    [ ! -L "$home/moonraker/moonraker/components/helix_print.py" ]
    diff -r "$BATS_TEST_TMPDIR/config.before" "$c" || fail "the config dir changed"
}

@test "moonraker_conf_port reads [server] port from moonraker.conf or a file it includes" {
    local c="$BATS_TEST_TMPDIR/cfg" conf_sh="$REPO_ROOT/moonraker-plugin/helix_print_conf.sh"
    mkdir -p "$c"
    printf '[server]\nhost: 0.0.0.0\nport: 80\r\n\n[webcam]\nport: 8080\n' > "$c/moonraker.conf"
    run sh -c '. "$1"; moonraker_conf_port "$2"' sh "$conf_sh" "$c/moonraker.conf"
    [ "$output" = 80 ] || fail "got '$output'"

    printf '[webcam]\nport: 8080\n\n[include server.conf]\n' > "$c/moonraker.conf"
    printf '[server]\nport = 7130\n' > "$c/server.conf"
    run sh -c '. "$1"; moonraker_conf_port "$2"' sh "$conf_sh" "$c/moonraker.conf"
    [ "$output" = 7130 ] || fail "got '$output'"

    printf '[server]\nhost: 0.0.0.0\n\n[webcam]\nport: 8080\n' > "$c/moonraker.conf"
    run sh -c '. "$1"; moonraker_conf_port "$2"' sh "$conf_sh" "$c/moonraker.conf"
    [ "$output" = 7125 ] || fail "got '$output'"

    run sh -c '. "$1"; moonraker_conf_port "$2"' sh "$conf_sh" "$c/missing.conf"
    [ "$output" = 7125 ] || fail "got '$output'"
}

@test "an auto install restarts and waits on the port moonraker.conf names" {
    local home="$BATS_TEST_TMPDIR/home" log="$BATS_TEST_TMPDIR/curl.log"
    setup_plain_home "$home"
    printf '[server]\nhost: 0.0.0.0\nport: 80\n' > "$home/printer_data/config/moonraker.conf"
    mock_systemd_without_sudo "$log"

    run env HOME="$home" sh "$SCRIPT" --auto
    [ "$status" -eq 0 ] || fail "install failed: $output"
    grep -q -- '-X POST http://127.0.0.1:80/server/restart' "$log" || fail "restart not on port 80: $(cat "$log")"
    grep -q 'http://127.0.0.1:80/server/info' "$log" || fail "wait not on port 80: $(cat "$log")"
    if grep -q ':7125' "$log"; then fail "reached for 7125: $(cat "$log")"; fi
}

@test "an auto uninstall restarts on the port moonraker.conf names" {
    local home="$BATS_TEST_TMPDIR/home" log="$BATS_TEST_TMPDIR/curl.log"
    setup_instrumented_home "$home"
    printf '[server]\nport: 80\n\n[helix_print]\n' > "$home/printer_data/config/moonraker.conf"
    mock_systemd_without_sudo "$log"

    run env HOME="$home" sh "$SCRIPT" --uninstall-auto
    [ "$status" -eq 0 ] || fail "uninstall failed: $output"
    grep -q -- '-X POST http://127.0.0.1:80/server/restart' "$log" || fail "restart not on port 80: $(cat "$log")"
    if grep -q ':7125' "$log"; then fail "reached for 7125: $(cat "$log")"; fi
}

# A Pi-style home whose Moonraker runs with uid $2 and groups $3 (its
# primary first), and a copy of the plugin under $PERM_TMP/app, which the
# test gives its modes. PERM_TMP sits directly in /tmp: every directory above
# the plugin is then searchable by anyone, which bats' own 0700 temp
# directories are not.
setup_moonraker_as() {
    local home="$1" uid="$2" groups="$3"
    setup_plain_home "$home"
    mkdir -p "$home/proc/700"
    printf '%s\0' python3 "$home/moonraker/moonraker/moonraker.py" -d "$home/printer_data" \
        > "$home/proc/700/cmdline"
    printf 'Name:\tpython3\nUid:\t%s\t%s\t%s\t%s\nGid:\t%s\t%s\t%s\t%s\nGroups:\t%s\n' \
        "$uid" "$uid" "$uid" "$uid" "${groups%% *}" "${groups%% *}" "${groups%% *}" "${groups%% *}" \
        "$groups" > "$home/proc/700/status"
    [ -n "${PERM_TMP:-}" ] || PERM_TMP=$(mktemp -d /tmp/helix-plugin-perm.XXXXXX)
    chmod 0755 "$PERM_TMP"
    rm -rf "$PERM_TMP/app"
    mkdir -p "$PERM_TMP/app"
    cp -R "$REPO_ROOT/moonraker-plugin" "$PERM_TMP/app/"
    chmod 0755 "$PERM_TMP/app" "$PERM_TMP/app/moonraker-plugin"
    chmod 0644 "$PERM_TMP/app/moonraker-plugin"/*.py
}

teardown() {
    [ -z "${PERM_TMP:-}" ] || rm -rf "$PERM_TMP"
}

# Run the copy of install.sh under $PERM_TMP with HOME $1 and the rest as arguments.
run_perm_install() {
    local home="$1"
    shift
    run env HOME="$home" HELIX_PROC_ROOT="$home/proc" sh "$PERM_TMP/app/moonraker-plugin/install.sh" "$@"
}

@test "auto install refuses a plugin the Moonraker user cannot read, and writes nothing" {
    [ "$(id -u)" -ne 0 ] || skip "root reads everything"
    local home="$BATS_TEST_TMPDIR/home"
    setup_moonraker_as "$home" 4242 4242
    chmod 0700 "$PERM_TMP/app"
    local c="$home/printer_data/config"
    cp -R "$c" "$BATS_TEST_TMPDIR/config.before"

    run_perm_install "$home" --no-restart --auto
    [ "$status" -eq 1 ] || fail "want 1, got $status: $output"
    contains "cannot read" "$output"
    contains "$PERM_TMP/app" "$output"
    [ ! -L "$home/moonraker/moonraker/components/helix_print.py" ]
    diff -r "$BATS_TEST_TMPDIR/config.before" "$c" || fail "the config dir changed"
}

@test "auto install refuses a plugin file the Moonraker user cannot read" {
    [ "$(id -u)" -ne 0 ] || skip "root reads everything"
    local home="$BATS_TEST_TMPDIR/home"
    setup_moonraker_as "$home" 4242 4242
    chmod 0600 "$PERM_TMP/app/moonraker-plugin/helix_3mf.py"

    run_perm_install "$home" --no-restart --auto
    [ "$status" -eq 1 ] || fail "want 1, got $status: $output"
    contains "helix_3mf.py" "$output"
    [ ! -L "$home/moonraker/moonraker/components/helix_print.py" ]
}

@test "auto install accepts a plugin the Moonraker user reads as owner, group or other" {
    [ "$(id -u)" -ne 0 ] || skip "root reads everything"
    local home="$BATS_TEST_TMPDIR/home" me grp
    me=$(id -u)
    grp=$(id -g)

    setup_moonraker_as "$home" 4242 4242
    run_perm_install "$home" --no-restart --auto
    [ "$status" -eq 0 ] || fail "other: $output"

    rm -rf "$home"
    setup_moonraker_as "$home" "$me" "$grp"
    chmod 0700 "$PERM_TMP/app"
    run_perm_install "$home" --no-restart --auto
    [ "$status" -eq 0 ] || fail "owner: $output"

    rm -rf "$home"
    setup_moonraker_as "$home" 4242 "4243 $grp"
    chmod 0750 "$PERM_TMP/app"
    run_perm_install "$home" --no-restart --auto
    [ "$status" -eq 0 ] || fail "group: $output"
}

@test "relink refuses to point the link at a plugin the Moonraker user cannot read" {
    [ "$(id -u)" -ne 0 ] || skip "root reads everything"
    local home="$BATS_TEST_TMPDIR/home"
    setup_moonraker_as "$home" 4242 4242
    printf '\n[helix_print]\n' >> "$home/printer_data/config/moonraker.conf"
    ln -s "$home/old/moonraker-plugin/helix_print.py" "$home/moonraker/moonraker/components/helix_print.py"
    chmod 0700 "$PERM_TMP/app"

    run_perm_install "$home" --no-restart --relink
    [ "$status" -eq 1 ] || fail "want 1, got $status: $output"
    [ "$(readlink "$home/moonraker/moonraker/components/helix_print.py")" = "$home/old/moonraker-plugin/helix_print.py" ]
}
