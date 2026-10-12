# Installing HelixScreen on the FLSUN T1 Pro

How to run HelixScreen on the FLSUN T1 Pro's own screen, on the stock board and stock FLSUN OS, in place of FLSUN's screen app.

## Tested With

> **Preliminary.** Tested on one T1 Pro running stock FLSUN OS (Klipper fork 1.0.1.5, Debian 10). There is no installer support and no prebuilt release yet: you build the package yourself and install it by hand, as below.

## Prerequisites

- **Hardware:** FLSUN T1 Pro (Rockchip RV1126, 4.3" 480×272 touchscreen, armv7l)
- **Software:** stock FLSUN OS. Nothing needs flashing: Klipper and Moonraker are already there.
- **SSH access:** user `pi`, default password `1` (also the `sudo` password). Change it once you are done.
- **A build machine** with Docker, for `make flsun-t1-docker`

## Installation

### Step 1: Build the package

```bash
make flsun-t1-docker
make release-flsun-t1
```

This produces `releases/helixscreen-flsun-t1-<version>.tar.gz`. The build uses the same static armv7 toolchain as the Centauri Carbon, so it does not depend on the printer's old glibc.

### Step 2: Copy it to the printer and unpack it

The printer's SSH server has no SFTP, so `scp` needs `-O`:

```bash
scp -O releases/helixscreen-flsun-t1-*.tar.gz pi@<ip>:/home/pi/
ssh pi@<ip>
sudo tar xzf helixscreen-flsun-t1-*.tar.gz -C /home/pi
sudo chown -R root:root /home/pi/helixscreen
```

The install must be owned by root: HelixScreen runs as root, and its launcher refuses `config/helixscreen.env` in a directory another user can write to (the screen shows an "env file ignored" warning).

### Step 3: Start HelixScreen instead of the stock screen

FLSUN's `/etc/rc.local` starts the stock screen through `/home/pi/qt/out_linux/startboot.sh`. Keep a copy of it, then replace it:

```bash
sudo cp -p /home/pi/qt/out_linux/startboot.sh /home/pi/qt/out_linux/startboot.sh.flsun
sudo tee /home/pi/qt/out_linux/startboot.sh >/dev/null <<'EOF'
#!/bin/sh
HELIX_DIR=/home/pi/helixscreen
if [ -x "$HELIX_DIR/bin/helix-launcher.sh" ]; then
    unset LD_PRELOAD LD_LIBRARY_PATH QT_QPA_PLATFORM QT_QPA_GENERIC_PLUGINS QT_QPA_FB_TSLIB
    cd "$HELIX_DIR" || exit 1
    exec "$HELIX_DIR/bin/helix-launcher.sh" >/tmp/helixscreen-launcher.log 2>&1 </dev/null
fi
exec /home/pi/qt/out_linux/startboot.sh.flsun
EOF
sudo chmod 755 /home/pi/qt/out_linux/startboot.sh
sudo reboot
```

If HelixScreen is ever missing, the script falls back to the stock screen.

## Switching back to the stock screen

```bash
sudo cp /home/pi/qt/out_linux/startboot.sh.flsun /home/pi/qt/out_linux/startboot.sh
sudo reboot
```

## Service Control and Logs

```bash
# Logs go to the systemd journal
journalctl -t helix-screen -n 100
# Launcher output (startup, crash capture)
cat /tmp/helixscreen-launcher.log
```

## Quirks and Notes

- **Leave `/home/pi/qt/qt_start` at `1`.** `rc.local` only runs `startboot.sh` when it is `1`; at `0` it runs FLSUN's update script instead.
- **FLSUN firmware updates** may replace `/home/pi/qt`, which puts the stock screen back. Repeat Step 3 afterwards.
- **Touch.** The panel is a 272×480 portrait digitizer that declares a 480×272 range, so its raw axes are swapped and one is inverted. The `flsun_t1` preset carries the mapping as a stored touch range (`input.touch_range`); FLSUN's own tslib matrix in `/etc/pointercal` cannot be used as an affine calibration, because the out-of-range axis is clamped before the affine runs and the right of the screen becomes unreachable. If touch is off after an upgrade, run **Settings > Touch & Input > Touch Calibration**.
- **Buttons.** The preset points the **Bed Level** quick button at FLSUN's delta calibration (`BED_LEVEL_1`, which ends in `SAVE_CONFIG` and restarts Klipper) and clears the second quick button, whose default runs a helper macro the stock firmware does not have. The enclosure light is on the Home light button. FLSUN's other everyday macros are worth adding as **Macro Button** widgets on a second Home page: `BED_LEVEL_2` (heated bed mesh, also ends in `SAVE_CONFIG`), `boxfan_on` / `boxfan_off` (enclosure fan) and `ZUP` / `ZDOWN` (±0.025 mm Z offset).
- **PID tuning:** use FLSUN's `PID_EXTRUDER` / `PID_BED` macros. `PID_EXTRUDER` switches `relay_pin` on first, which HelixScreen's PID screen does not do.
- **Not covered yet:** FLSUN's power-loss resume prompt, Wi-Fi setup, the enclosure light and fan, and FLSUN's own firmware updates are features of the stock screen app. Use Mainsail or Fluidd for those for now.
- **Stopping the stock app by hand:** `/home/pi/qt/out_linux/kill9.sh` kills every process whose command line contains `TUI` — including an SSH command that mentions it.

**Testing on this printer?** Please report your results via [GitHub Issues](https://github.com/prestonbrown/helixscreen/issues) or [Discord](https://discord.gg/RZCT2StKhr).

## See Also

- [Supported printers](supported-printers.md#other-dedicated-builds)
- [Troubleshooting](../TROUBLESHOOTING.md)
