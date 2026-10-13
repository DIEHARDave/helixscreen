# QIDI Plus 5 — Real-Device Analysis

**Date**: 2026-10-13
**Status**: Real device data analyzed (stock firmware 01.01.01.07)
**Source**: Live stock QIDI Plus 5 with SSH, Moonraker `info` / `objects/list` /
`configfile` JSON, `journalctl`, and the on-device `qidiclient` install.

> The Plus 5 is the newest QIDI and shares the **new-generation board** class with
> the Q2 and Max 4 (Rockchip host + STM32 main MCU + Linux framebuffer display),
> but with a **sideways 480×800** panel and a **~325 mm** bed. This note records
> what the hardware exposes, so the database entry and preset key off real
> signals rather than assumptions.

---

## 0. Host / OS

| | |
|---|---|
| SoC | Rockchip **RK3308B-S**, 4× Cortex-A35 @ 1.1 GHz, **no GPU** |
| RAM / storage | ~498 MB / 29 GB eMMC (one ext4 root partition) |
| OS | Debian 11 (bullseye), glibc 2.31, kernel **5.10.160** (QIDI vendor) |
| **Virtual terminals** | **none** — `CONFIG_VT=n`, no `/dev/ttyN`, console is the serial `ttyFIQ0` |
| MCUs | **STM32F407** (main) + **STM32F103xe** (toolhead, `[mcu THR]` on `/dev/ttyS4`) |

Because there are no VTs, Xorg/KlipperScreen cannot start normally (`parse_vt_settings:
Cannot open /dev/tty0`). HelixScreen is unaffected: it renders through **SDL2 on
DRM/fbdev directly** and needs no display server. (For the record, the no-VT Xorg
workaround is `Xorg :0 vt08 -sharevts` with `/dev/tty8 -> /dev/null` — see the
companion community project.)

## 1. Display + stock UI

- **Panel**: 480×800 RGB panel on the SoC, exposed as `/dev/fb0` (`/proc/fb` →
  `rockchipdrmfb`). Touch: **Goodix** capacitive on `/dev/input/event0` (I2C),
  ABS range X 0..479 / Y 0..799, no calibration needed.
- **Orientation**: native framebuffer is portrait 480×800, but the panel is
  mounted **sideways**. `display.rotate = 90` (or `270`) renders it as landscape
  800×480 — the usable orientation, and what the preset sets.
- **Stock UI**: a native **LVGL** application, `/home/qidi/QIDI_Client/bin/qidiclient`
  (90 MB, systemd service **`qidi-client`**), assets in `QIDI_Client/access/*`.
  It is a closed binary (the community only re-themes its LVGL icons/fonts), and it
  is what the on-device install replaces. The host talks to Moonraker as WebSocket
  agent `QD_PLUS5` (`WebSocket++`).

## 2. Klipper fingerprint (what detection sees)

- **Kinematics**: corexy; 2 Z steppers (`stepper_z`, `stepper_z1`) with `z_tilt`.
- **Build volume**: stepper travel **X ≈ 333 mm** (`position_min -7` … `position_max 326`),
  **Y ≈ 345 mm** (`-5` … `340`). This is the model's discriminator inside the
  framebuffer family — Q2 ≈ 275 mm, Max 4 ≈ 390 mm. The `bed_mesh` probed area is
  290×290.
- **Heaters/sensors**: `heater_bed`, `extruder` (max 375 °C), active
  `heater_generic chamber` (0–70 °C), `temperature_sensor
  Chamber_Thermal_Protection_Sensor`.
- **Fans (6)**: `fan_generic cooling_fan`, `heater_fan hotend_fan`,
  `controller_fan chamber_fan`, `fan_generic chamber_circulation_fan`,
  `controller_fan board_fan`, **`fan_generic auxiliary_cooling_fan`** (single aux —
  the Max 4 carries a second `auxiliary_cooling_fan2`).
- **Lights**: `output_pin caselight` (+ `smart_output_pin caselight`), `neopixel RGB`.
- **Filament**: `filament_switch_sensor filament_switch_sensor`; QIDI Box host
  (`multi_color_controller`, `ai_func_manager`), compatible with the hub-style
  4-slot box chainable to 16.
- **Macros** (stock `klipper-macros-qd/*`): `M4029`/`M4030`/`M4031`/`M4032`
  calibration, `CLEAR_NOZZLE`, `M141`/`M191` chamber, `M603`/`M604` load/unload,
  `CUT_FILAMENT_1`, `EXTRUSION_AND_FLUSH`.
- 174 objects total; `mcu` reports `stm32f407xx`, `mcu THR` reports `stm32f103xe`.

### Detection caveat: hostname is not the host name

HelixScreen's detector hostname is Moonraker's **`machine_name`**, which QIDI sets
to the literal **`QIDI`** — *not* the real hostname (`qidi-plus5`), and not the
`/printer/info` hostname. So `hostname_match` patterns collapse to `qidi` on stock
firmware and cannot separate the models; the Plus 5 is identified by its **bed
size** instead (see § 3).

## 3. Database entry rationale

The `qidi_plus_5` entry uses the `build_volume_range` window **324–342 × 336–354 mm**
with **`"identifying": true`**: it is the one signal that names the model among the
Q2/Plus 4/Max 4, and it *only* matches a ~325 mm bed, so it never fires on the Q2
(≈275) or Max 4 (≈390) hardware. The QIDI-shared signals (chamber heater, chamber
thermal sensor, aux fan, `multi_color_controller`, `probe_air`, `M4029`,
`CLEAR_NOZZLE`, STM32F407) are marked **`corroborating`**, so they can support the
id but cannot make the Plus 5 lead on another QIDI's hardware.

Measured on the device (override via `printer_database.d/`, then detection replayed):

```
Detection complete: Qidi Plus 5 (confidence: 100%, 11 matches, margin 12 over 'Qidi Q2', 1 tied,
  reason: Stepper travel ~333x345mm: the Qidi Plus 5 bed. ... (+10 more))
Auto-detected printer: 'Qidi Plus 5' (100% confidence)
```

The preset (`assets/config/presets/qidi_plus5.json`) carries the hardware mappings,
`display.rotate = 90`, and the CLEAR_NOZZLE / M4029 quick macros.

## 4. On-device install notes

- Stock firmware runs standard **Moonraker on `:7125`**, so the installer's
  QIDI-class hook (`hooks-qidi.sh`) already stops/disables `qidi-client`. The
  screen app and QIDI cloud/camera stop working while HelixScreen runs; uninstalling
  brings them back.
- HelixScreen auto-selects its **SDL2 DRM** backend and the Goodix touch device;
  with rotation set it transparently uses the framebuffer path for software rotation.
- RAM is fine: HelixScreen ran at ~49 MB RSS alongside Klipper/Moonraker on the
  498 MB host.
